/*
 * cdxa.c — see cdxa.h.
 */
#include "cdxa.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* The two Galois tables                                                      */
/* ------------------------------------------------------------------------- */
/*
 * The EDC is a CRC-32 with the reversed polynomial 0xD8018001, and the ECC is
 * Reed-Solomon over GF(2^8) with the primitive polynomial x^8 + x^4 + x^3 +
 * x^2 + 1 (0x11D). Both tables are built rather than transcribed: 512 bytes of
 * constants copied by hand is 512 chances to be wrong in a way that only shows
 * up on hardware.
 */
static u8  g_ecc_f[256];
static u8  g_ecc_b[256];
static u32 g_edc[256];
static bool g_built;

static void cd_build_tables(void)
{
    u32 i, j;

    if (g_built)
        return;

    for (i = 0; i < 256; i++) {
        /* Multiply by x in GF(2^8), reducing by 0x11D. */
        j = (i << 1) ^ ((i & 0x80u) ? 0x11Du : 0u);
        g_ecc_f[i] = (u8)j;
        g_ecc_b[i ^ (u8)j] = (u8)i;

        j = i;
        {
            u32 k;

            for (k = 0; k < 8; k++)
                j = (j >> 1) ^ ((j & 1u) ? 0xD8018001u : 0u);
        }
        g_edc[i] = j;
    }

    g_built = true;
}

u32 cd_edc(const u8 *p, u32 n)
{
    u32 edc = 0;

    cd_build_tables();
    while (n--)
        edc = (edc >> 8) ^ g_edc[(edc ^ *p++) & 0xFFu];
    return edc;
}

/*
 * One parity pass.
 *
 * P and Q are the same computation over two different readings of the same
 * 2064 bytes: P takes 86 columns of 24, Q takes 52 diagonals of 43. The index
 * arithmetic below is the standard's, and the `if (index >= size)` wrap is what
 * makes Q diagonal rather than merely striped.
 */
static void cd_ecc_block(const u8 *src, u32 major_count, u32 minor_count,
                         u32 major_mult, u32 minor_inc, u8 *dest)
{
    u32 size = major_count * minor_count;
    u32 major;

    for (major = 0; major < major_count; major++) {
        u32 index = (major >> 1) * major_mult + (major & 1u);
        u8  ecc_a = 0, ecc_b = 0;
        u32 minor;

        for (minor = 0; minor < minor_count; minor++) {
            u8 t = src[index];

            index += minor_inc;
            if (index >= size)
                index -= size;
            ecc_a ^= t;
            ecc_b ^= t;
            ecc_a = g_ecc_f[ecc_a];
        }

        ecc_a = g_ecc_b[g_ecc_f[ecc_a] ^ ecc_b];
        dest[major]               = ecc_a;
        dest[major + major_count] = (u8)(ecc_a ^ ecc_b);
    }
}

void cd_ecc_generate(u8 sector[CD_SECTOR_RAW])
{
    u8 address[4];
    int i;

    cd_build_tables();

    /*
     * MODE 2'S RULE: the parity is computed with the address bytes zeroed, so
     * that a Form 1 sector's ECC does not depend on where on the disc it sits.
     * Mode 1 includes them. This is the difference that makes a whole image's
     * worth of parity wrong at once.
     */
    for (i = 0; i < 4; i++) {
        address[i] = sector[12 + i];
        sector[12 + i] = 0;
    }

    cd_ecc_block(sector + 0x0C, 86, 24,  2, 86, sector + 0x81C);   /* P */
    cd_ecc_block(sector + 0x0C, 52, 43, 86, 88, sector + 0x8C8);   /* Q */

    for (i = 0; i < 4; i++)
        sector[12 + i] = address[i];
}

/* ------------------------------------------------------------------------- */
static u8 to_bcd(u32 v)
{
    return (u8)(((v / 10u) << 4) | (v % 10u));
}

void cd_sector_build(u8 out[CD_SECTOR_RAW], u32 lba,
                     u8 file, u8 channel, u8 submode, u8 coding,
                     const u8 *payload, bool form2)
{
    static const u8 k_sync[12] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
    };
    u32 amsf = lba + 150u;      /* the two-second lead-in the stamp counts from */
    u32 edc;

    if (!out)
        return;

    memset(out, 0, CD_SECTOR_RAW);
    memcpy(out, k_sync, sizeof(k_sync));

    out[12] = to_bcd(amsf / (75u * 60u));
    out[13] = to_bcd((amsf / 75u) % 60u);
    out[14] = to_bcd(amsf % 75u);
    out[15] = 2;                                  /* Mode 2 */

    /* The subheader, and its copy. Both are written because a drive is allowed
     * to read either, and a mastering tool that writes one leaves a disc that
     * works on some hardware. */
    if (form2)
        submode |= (u8)CD_SUBMODE_FORM2;
    else
        submode = (u8)(submode & ~(unsigned)CD_SUBMODE_FORM2);

    out[16] = file;    out[17] = channel; out[18] = submode; out[19] = coding;
    out[20] = file;    out[21] = channel; out[22] = submode; out[23] = coding;

    if (payload)
        memcpy(out + 24, payload, form2 ? CD_SECTOR_FORM2 : CD_SECTOR_FORM1);

    if (form2) {
        /* Form 2 spends the parity area on data and carries only an EDC. */
        edc = cd_edc(out + 0x10, 0x91C);
        q2_wr_u32(out + 0x92C, edc);
    } else {
        edc = cd_edc(out + 0x10, 0x808);
        q2_wr_u32(out + 0x818, edc);
        /* The eight bytes between the EDC and the P parity are reserved and
         * zero; `out` was cleared, so they already are. */
        cd_ecc_generate(out);
    }
}

u32 cd_sector_check(const u8 raw[CD_SECTOR_RAW])
{
    u8  copy[CD_SECTOR_RAW];
    u32 bad = 0;
    u32 edc;

    if (!raw)
        return 7;

    if (raw[18] & CD_SUBMODE_FORM2) {
        edc = cd_edc(raw + 0x10, 0x91C);
        /* A blank Form 2 EDC is legal — the standard makes it optional — so a
         * zero there is not a disagreement, it is an absence. */
        if (q2_rd_u32(raw + 0x92C) != 0 && q2_rd_u32(raw + 0x92C) != edc)
            bad |= 1u;
        return bad;
    }

    edc = cd_edc(raw + 0x10, 0x808);
    if (q2_rd_u32(raw + 0x818) != edc)
        bad |= 1u;

    memcpy(copy, raw, CD_SECTOR_RAW);
    cd_ecc_generate(copy);
    if (memcmp(copy + 0x81C, raw + 0x81C, 172) != 0)
        bad |= 2u;
    if (memcmp(copy + 0x8C8, raw + 0x8C8, 104) != 0)
        bad |= 4u;

    return bad;
}

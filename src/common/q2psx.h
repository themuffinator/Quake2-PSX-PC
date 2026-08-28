/*
 * q2psx.h — shared primitive types, byte access and diagnostics.
 *
 * The PlayStation is little-endian MIPS R3000A, so every integer on the disc is
 * little-endian. The host may not be, hence the explicit readers below: nothing in
 * this project ever casts a raw disc pointer straight to a wider integer type.
 */
#ifndef Q2PSX_H
#define Q2PSX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Short type names, matching the style of the era's engine source.           */
/* ------------------------------------------------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define Q2PSX_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#if defined(_MSC_VER)
#  define Q2PSX_INLINE __forceinline
#  define Q2PSX_PRINTF(fmt, args)
#else
#  define Q2PSX_INLINE static inline __attribute__((always_inline))
#  define Q2PSX_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#endif

/* ------------------------------------------------------------------------- */
/* Little-endian unaligned readers.                                           */
/* ------------------------------------------------------------------------- */
Q2PSX_INLINE u8 q2_rd_u8(const void *p)
{
    return ((const u8 *)p)[0];
}

Q2PSX_INLINE u16 q2_rd_u16(const void *p)
{
    const u8 *b = (const u8 *)p;
    return (u16)((u32)b[0] | ((u32)b[1] << 8));
}

Q2PSX_INLINE u32 q2_rd_u32(const void *p)
{
    const u8 *b = (const u8 *)p;
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

Q2PSX_INLINE s16 q2_rd_s16(const void *p) { return (s16)q2_rd_u16(p); }
Q2PSX_INLINE s32 q2_rd_s32(const void *p) { return (s32)q2_rd_u32(p); }

/* ------------------------------------------------------------------------- */
/* ...and the writers.                                                        */
/* ------------------------------------------------------------------------- */
/*
 * These are new, and it is worth saying why a project that only ever READ the
 * disc now has them: an encoder (stxenc.h) is the strictest test a format
 * reading can be put to, because a decoder can be wrong in ways no picture
 * shows and a round trip cannot. Unaligned and little-endian, to match the
 * readers exactly — the console's own byte order.
 */
Q2PSX_INLINE void q2_wr_u16(void *p, u16 v)
{
    u8 *b = (u8 *)p;
    b[0] = (u8)(v & 0xFF);
    b[1] = (u8)((v >> 8) & 0xFF);
}

Q2PSX_INLINE void q2_wr_u32(void *p, u32 v)
{
    u8 *b = (u8 *)p;
    b[0] = (u8)(v & 0xFF);
    b[1] = (u8)((v >> 8) & 0xFF);
    b[2] = (u8)((v >> 16) & 0xFF);
    b[3] = (u8)((v >> 24) & 0xFF);
}

/* Big-endian — needed only for ISO9660's "both-endian" fields, where we read the
 * LE half, and for a couple of CD structures. */
Q2PSX_INLINE u32 q2_rd_u32be(const void *p)
{
    const u8 *b = (const u8 *)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}

/* ------------------------------------------------------------------------- */
/* Result codes. Every fallible API returns one of these.                     */
/* ------------------------------------------------------------------------- */
typedef enum q2_result {
    Q2_OK = 0,
    Q2_ERR_IO,            /* host filesystem or device failure               */
    Q2_ERR_NOT_FOUND,     /* file/entry does not exist                       */
    Q2_ERR_BAD_FORMAT,    /* structurally invalid data                       */
    Q2_ERR_UNSUPPORTED,   /* recognised, but not implemented yet             */
    Q2_ERR_NO_MEMORY,
    Q2_ERR_RANGE,         /* index or offset out of bounds                   */
    Q2_ERR_INVALID_ARG
} q2_result;

const char *q2_result_str(q2_result r);

/* ------------------------------------------------------------------------- */
/* Logging.                                                                   */
/* ------------------------------------------------------------------------- */
typedef enum q2_log_level {
    Q2_LOG_ERROR = 0,
    Q2_LOG_WARN,
    Q2_LOG_INFO,
    Q2_LOG_DEBUG,
    Q2_LOG_TRACE
} q2_log_level;

void q2_log_set_level(q2_log_level level);
q2_log_level q2_log_get_level(void);
void q2_log(q2_log_level level, const char *fmt, ...) Q2PSX_PRINTF(2, 3);

#define Q2_ERROR(...) q2_log(Q2_LOG_ERROR, __VA_ARGS__)
#define Q2_WARN(...)  q2_log(Q2_LOG_WARN,  __VA_ARGS__)
#define Q2_INFO(...)  q2_log(Q2_LOG_INFO,  __VA_ARGS__)
#define Q2_DEBUG(...) q2_log(Q2_LOG_DEBUG, __VA_ARGS__)
#define Q2_TRACE(...) q2_log(Q2_LOG_TRACE, __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Buffers.                                                                   */
/* ------------------------------------------------------------------------- */
typedef struct q2_buf {
    u8    *data;
    size_t size;
} q2_buf;

q2_result q2_buf_alloc(q2_buf *buf, size_t size);
void      q2_buf_free(q2_buf *buf);

/* Bounds-checked slice of a buffer; returns NULL if the range escapes it. */
const u8 *q2_buf_at(const q2_buf *buf, size_t offset, size_t need);

/*
 * Copy a string into a fixed-width field, truncating rather than overflowing,
 * and always terminating.
 *
 * This exists because `strncpy(dst, src, sizeof dst - 1)` on a pre-zeroed
 * struct is correct and unreadable: it relies on the zeroing for the
 * terminator, which is somewhere else entirely, and GCC's
 * -Wstringop-truncation cannot see that and says so on every call.
 *
 * It also reads no further than `cap - 1` bytes of `src`, so a fixed-width
 * on-disc field carrying no NUL -- which is most of them -- is safe to pass.
 */
void q2_str_copy(char *dst, size_t cap, const char *src);

#endif /* Q2PSX_H */

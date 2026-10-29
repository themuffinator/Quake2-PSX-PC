/* Native export-2 sound initialization: branch/load delay, handle aliases,
 * packed arguments, bounds, and every relocated module on the user's disc. */
#include "creature.h"
#include "reloc.h"
#include "disc.h"
#include "dat.h"
#include "vag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(0)
static void put(u8 *p,u32 v) { unsigned i; for(i=0;i<4;i++) p[i]=(u8)(v>>(i*8)); }
static u8 synthetic[4096]; static u32 pc;
static void emit(u32 w) { put(synthetic+pc,w); pc+=4; }
static void imm(u32 r,u32 x) { emit(0x3C000000u|(r<<16)|(x>>16)); emit(0x34000000u|(r<<21)|(r<<16)|(x&65535)); }
static void sound(const char *name)
{
    char n[12]={0}; unsigned i; memcpy(n,name,strlen(name));
    for(i=0;i<3;i++) imm(4+i,q2_rd_u32((const u8 *)n+4*i));
    emit(0x8E190024u); emit(0); emit(0x0320F809u); emit(0);
}
static bool primary_only(const char *name,void *user) { return !strcmp(name,user?"primary":"fallback"); }
static bool missing(const char *name,void *user) { (void)name; (void)user; return false; }
static q2_sfx_request *shared_request(const char *name,void *user)
{ return !strcmp(name,"primary") ? user : NULL; }
static void test_synthetic(void)
{
    q2_cre_sound_bind out[8]; u32 n, fault, branch, target, i; u8 original[sizeof(synthetic)];
    pc=0x130; put(synthetic+8,0x80100000u+pc);
    emit(0x27BDFFE0u); emit(0xAFBF001Cu); imm(16,0x80100000u);
    sound("primary"); emit(0xAFA20010u);
    for(i=0;i<160;i++) emit(0); /* defeats the old bounded literal scan */
    emit(0x8FA80010u); emit(0); /* lw t0,16(sp), load delay */
    branch=pc; emit(0); emit(0xAE080E00u); /* bne t0,zero,end; sw in delay slot */
    sound("fallback"); emit(0xAE020E00u);
    target=pc; put(synthetic+branch,0x15000000u|((target-branch-4)/4));
    emit(0x8E080E00u); emit(0); emit(0xAE080E04u); /* handle-to-handle alias */
    emit(0x8FBF001Cu); emit(0x27BD0020u); emit(0x03E00008u); emit(0);
    memcpy(original,synthetic,sizeof(original));
    CHECK(q2_creature_init_sounds(synthetic,sizeof(synthetic),0x80100000u,
          primary_only,(void *)1,out,8,&n,&fault)==Q2_OK);
    CHECK(n==2 && !strcmp(out[0].name,"primary") && !strcmp(out[1].name,"primary"));
    CHECK(q2_creature_init_sounds(synthetic,sizeof(synthetic),0x80100000u,
          primary_only,NULL,out,8,&n,&fault)==Q2_OK);
    CHECK(n==2 && out[0].addr==0x80100E00u && !strcmp(out[0].name,"fallback"));
    CHECK(out[1].addr==0x80100E04u && !strcmp(out[1].name,"fallback"));
    CHECK(q2_creature_init_sounds(synthetic,sizeof(synthetic),0x80100000u,
          missing,NULL,out,8,&n,&fault)==Q2_OK && n==0);
    CHECK(!memcmp(synthetic,original,sizeof(original)));
    CHECK(q2_creature_sound_bindings(synthetic,sizeof(synthetic),0x80100000u,out,8)==4);
    /* A branch immediately after lw sees old t0=0, so even an available
     * primary is replaced by the missing fallback. Immediate-load emulation
     * incorrectly skips that fallback and leaves two primary handles. */
    put(synthetic+branch-4,0x15000000u|((target-branch)/4));
    put(synthetic+branch,0xAE080E00u); put(synthetic+branch+4,0);
    CHECK(q2_creature_init_sounds(synthetic,sizeof(synthetic),0x80100000u,
          primary_only,(void *)1,out,8,&n,&fault)==Q2_OK && n==0);
    put(synthetic+0x130,0xFFFFFFFFu);
    CHECK(q2_creature_init_sounds(synthetic,sizeof(synthetic),0x80100000u,
          NULL,NULL,out,8,&n,&fault)==Q2_ERR_BAD_FORMAT && !n && fault);
    put(synthetic+0x130,0x0804004Cu); /* bounded self-loop */
    put(synthetic+0x134,0);
    CHECK(q2_creature_init_sounds(synthetic,sizeof(synthetic),0x80100000u,
          NULL,NULL,out,8,&n,&fault)==Q2_ERR_BAD_FORMAT && !n);
}
static void test_request_clones(void)
{
    q2_cre_sound_bind out[8]; u32 n, fault, params_at, return_at;
    q2_sfx_request shared, before;
    memset(synthetic,0,sizeof(synthetic)); pc=0x130;
    put(synthetic+8,0x80100000u+pc);
    emit(0x27BDFFE0u); emit(0xAFBF001Cu); imm(16,0x80100000u);
    sound("primary"); emit(0xAE020E00u); /* original request */
    emit(0x00402021u); emit(0x8E19003Cu); emit(0); emit(0x0320F809u); emit(0);
    emit(0xAE020E04u); emit(0xAE020E08u); /* clone and an alias of the clone */
    params_at=pc;
    emit(0x00402021u); imm(5,50); imm(6,23); imm(7,0xFFFFFFFFu);
    emit(0x8E190044u); emit(0); emit(0x0320F809u); emit(0);
    return_at=pc;
    emit(0x8FBF001Cu); emit(0x27BD0020u); emit(0x03E00008u); emit(0);
    CHECK(q2_creature_init_sounds(synthetic,sizeof(synthetic),0x80100000u,
          NULL,NULL,out,8,&n,&fault)==Q2_OK && n==3);
    CHECK(out[0].request_id != out[1].request_id);
    CHECK(out[1].request_id == out[2].request_id);
    CHECK(out[0].request.pitch.current==35 && out[0].request.volume==63);
    CHECK(out[1].request.pitch.current==50 && out[1].request.volume==23);
    CHECK(out[2].request.pitch.current==50 && out[2].request.volume==23);
    CHECK(!out[0].cloned && out[1].cloned && out[2].cloned);

    /* SoundIndex aliases existing bank state across modules; a clone copies
     * that state once and SetRequestParams changes only the selected request. */
    q2_sfx_request_init(&shared);
    q2_sfx_pitch_set_range(&shared.pitch,8,0);
    shared.pitch.current=41; shared.volume=39; before=shared;
    CHECK(q2_creature_init_sound_requests(synthetic,sizeof(synthetic),0x80100000u,
          NULL,&shared,shared_request,out,8,&n,&fault)==Q2_OK && n==3);
    CHECK(out[0].request.pitch.current==41 && out[0].request.volume==39);
    CHECK(out[1].request.pitch.current==50 && out[1].request.volume==23);
    CHECK(out[1].request.pitch.varying && out[1].request.pitch.lower==35);
    CHECK(!memcmp(&shared,&before,sizeof(shared)));

    /* Point the parameter call back at the original handle. Its update must
     * be visible to another module, while the earlier clone retains 41/39. */
    put(synthetic+params_at,0x8E040E00u); /* lw a0,0xE00(s0) */
    CHECK(q2_creature_init_sound_requests(synthetic,sizeof(synthetic),0x80100000u,
          NULL,&shared,shared_request,out,8,&n,&fault)==Q2_OK && n==3);
    CHECK(shared.pitch.current==50 && shared.volume==23);
    CHECK(out[0].request.pitch.current==50 && out[0].request.volume==23);
    CHECK(out[1].request.pitch.current==41 && out[1].request.volume==39);

    /* An unsupported initializer must not publish partial parameter changes. */
    shared=before; put(synthetic+return_at,0xFFFFFFFFu);
    CHECK(q2_creature_init_sound_requests(synthetic,sizeof(synthetic),0x80100000u,
          NULL,&shared,shared_request,out,8,&n,&fault)==Q2_ERR_BAD_FORMAT && !n);
    CHECK(!memcmp(&shared,&before,sizeof(shared)));
}

static bool bank_has(const char *name,void *user)
{
    q2_sound_bank *bank=user; u32 i;
    for(i=0;i<bank->count;i++) { q2_vag vag; if(q2_sound_bank_get(bank,i,&vag) && !strcmp(name,vag.name)) return true; }
    return false;
}
static bool subset(const char *name,void *user)
{
    u32 h=2166136261u,seed=*(u32 *)user;
    if (!seed) return false;
    while(*name) h=(h^(u8)*name++)*16777619u;
    return ((h ^ (h >> (seed%17+1)) ^ seed) & 3)!=0;
}
static bool one_sample(const char *name,void *user) { return !strcmp(name,(const char *)user); }
static void test_native_fallbacks(const char *module,const u8 *image,size_t size)
{
    static const struct { const char *module,*sample; u32 slots[3]; } cases[]={
        {"Soldier","sol_pain2",{0x32AC,0x32B0,0x32B4}},
        {"Tankcomm","tnk_step",{0x2100,0x2110,0}},
        {"Hover","hov_pain2",{0x1460,0x1464,0}},
        {"Hover","hov_deth2",{0x1468,0x146C,0}},
        {"Hover","hov_srch2",{0x1474,0x1478,0}},
        {"Strider","str_step",{0x2BC0,0x2BD0,0}},
        {"Strider","tnk_step",{0x2BD0,0,0}},
        {"Arachner","ara_pain2",{0x19A0,0x19A4,0}}
    };
    unsigned i,k;
    for(i=0;i<Q2PSX_ARRAY_COUNT(cases);i++)if(!strncmp(module,cases[i].module,12)) {
        q2_cre_sound_bind out[128]; u32 n=0,fault=0;
        CHECK(q2_creature_init_sounds(image,size,0x80100000u,one_sample,
              (void *)cases[i].sample,out,128,&n,&fault)==Q2_OK);
        for(k=0;k<3 && cases[i].slots[k];k++) {
            /* MAGDEMO's 13172-byte Soldier has its BSS eight bytes later:
             * +0xE54..+0xEB8 fills +0x32B4/+0x32B8/+0x32BC. */
            u32 shift=!strncmp(module,"Soldier",12) && size==13172?8:0;
            const char *name=q2_creature_sound_for_addr(out,n,0x80100000u+cases[i].slots[k]+shift);
            if(!name || strcmp(name,cases[i].sample))
                fprintf(stderr,"%.12s slot %X with only %s resolved %s\n",module,cases[i].slots[k],cases[i].sample,name?name:"NULL");
            CHECK(name && !strcmp(name,cases[i].sample));
        }
        if(!strncmp(module,"Arachner",12)) {
            const q2_cre_sound_bind *normal=NULL,*clone=NULL; u32 j;
            for(j=0;j<n;j++) {
                if(out[j].addr==0x801019A0u)normal=&out[j];
                if(out[j].addr==0x801019A4u)clone=&out[j];
            }
            CHECK(normal && clone && normal->request_id!=clone->request_id);
            CHECK(normal && normal->request.pitch.current==35);
            CHECK(clone && clone->request.pitch.current==50);
            CHECK(normal && clone && normal->request.volume==63 && clone->request.volume==63);
        }
    }
}
static void test_disc(const char *path)
{
    disc *d=NULL; unsigned modules=0, runs=0; int i;
    CHECK(disc_open(&d,path)==Q2_OK); if(!d) return;
    for(i=0;i<disc_file_count(d);i++) {
        const char *file=disc_file_at(d,i)->path,*tail; char map[32];
        q2_buf b={0}; dat_archive ar; const dat_chunk *bin,*rel; u32 boff=0,roff=0;
        q2_sound_bank bank={0}; bool have_bank;
        if(*file=='/')file++;
        if(strncmp(file,"Q2DATA/LEVELS/",14))continue;
        tail=strchr(file+14,'/'); if(!tail || (size_t)(tail-file-14)>=sizeof(map))continue;
        if(strcmp(tail+1,"COMMON.DAT") && strncmp(tail+1,"ZONE",4))continue;
        memcpy(map,file+14,(size_t)(tail-file-14));map[tail-file-14]=0;
        CHECK(disc_read_file(d,file,&b)==Q2_OK); if(!b.data)continue;
        CHECK(dat_open_buf(&ar,&b)==Q2_OK); if(b.data) {q2_buf_free(&b);continue;}
        bin=dat_find(&ar,"CreAIBin"); rel=dat_find(&ar,"CreAIRel");
        have_bank=q2_sound_bank_load(&bank,d,map)==Q2_OK;
        while(bin && rel && boff+16<bin->size && roff+16<rel->size) {
            u32 bn=q2_rd_u32(bin->data+boff+12),rn=q2_rd_u32(rel->data+roff+12),seed;
            size_t size=(bn?bn:bin->size)-boff-16;
            u8 *img=malloc(size),*copy; q2_reloc_stats stats;
            CHECK(img!=NULL);if(!img)exit(2);
            memcpy(img,bin->data+boff+16,size);
            CHECK(q2_reloc_apply(img,size,rel->data+roff+16,(rn?rn:rel->size)-roff-16,0x80100000u,&stats)==Q2_OK);
            copy=malloc(size);if(!copy)exit(2);memcpy(copy,img,size);
            { unsigned before=failures;
                test_native_fallbacks((const char *)bin->data+boff,img,size);
                if(before!=failures)fprintf(stderr,"fallback fixture: %s module size %zu\n",file,size);
            }
            for(seed=0;seed<18;seed++) {
                q2_cre_sound_bind out[128]; u32 n=0,fault=0,j,k;
                bool (*has)(const char *,void *)=seed==17?bank_has:seed==16?NULL:subset;
                void *user=seed==17?(void *)&bank:(void *)&seed;
                q2_result rc;
                if(seed==17 && !have_bank)continue;
                rc=q2_creature_init_sounds(img,size,0x80100000u,has,user,out,128,&n,&fault);
                if(rc!=Q2_OK)fprintf(stderr,"%s %.12s mask %u stopped at %08X\n",file,bin->data+boff,seed,fault);
                CHECK(rc==Q2_OK);CHECK(n<=128);runs++;
                for(j=0;j<n && j<128;j++) {
                    CHECK(out[j].addr>=0x80100000u && out[j].addr-0x80100000u+4<=size && !(out[j].addr&3));
                    CHECK(!has || has(out[j].name,user));
                    for(k=0;k<j;k++)CHECK(out[j].addr!=out[k].addr);
                }
                CHECK(!memcmp(img,copy,size));
            }
            free(copy);free(img);modules++;
            if(!bn || !rn)break;boff=bn;roff=rn;
        }
        if(have_bank)q2_sound_bank_free(&bank);dat_close(&ar);
    }
    printf("%u native module initializers, %u bank configurations\n",modules,runs);
    CHECK(modules==166);disc_close(d);
}
int main(int argc,char **argv)
{
    test_synthetic();
    test_request_clones();
    if(argc==3 && !strcmp(argv[1],"--disc"))test_disc(argv[2]);
    printf("%u creature initialization checks, %u failures\n",checks,failures);
    return failures?1:0;
}

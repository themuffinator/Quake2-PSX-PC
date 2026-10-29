/* Native creature actor movement. The optional disc test executes the PAL
 * instructions themselves; no copyrighted executable bytes are embedded. */
#include "cremotion.h"
#include "monster.h"
#include "aimove.h"
#include "combat.h"
#include "exe.h"
#include "worldscale.h"
#include "area.h"
#include "scene.h"
#include "trig.h"
#include "model.h"
#include "reloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static s32 seen[3], thought_clock;
static void advance(q2_monster *m)
{
    memcpy(seen, m->pos, sizeof(seen));
    thought_clock = q2_level_state.time;
    m->pos[0] = (s32)((u32)m->pos[0] + 90u);
    m->pos[2] = (s32)((u32)m->pos[2] - 90u);
    q2_link_entity(m, 0x81);
    m->next_think = q2_level_state.time + 1;
}
static void init(q2_monster *m)
{
    q2_level_reset(); q2_ai_set_link_hooks(NULL, NULL, NULL);
    q2_monster_init(m); m->in_use = true; m->health = 100;
    q2_cre_motion_init(m);
}
static void test_actor(void)
{
    q2_monster m;
    q2_actor actor;
    q2_cre_motion saved;
    q2_cre_motion_world world = {0};
    init(&m);
    m.pos[0] = 90; m.pos[2] = -90; m.angles[2] = 4090;
    q2_link_entity(&m, 0x85);
    CHECK(m.motion.ent.pos[0] == 0 && m.motion.target[0] == 90);
    CHECK(m.motion.yaw == 0 && m.motion.target_yaw == 4090);
    q2_cre_motion_tick(&m, 10, &world);
    CHECK(m.motion.ent.pos[0] == 29 && m.motion.ent.pos[2] == -29);
    CHECK(m.motion.yaw == -1 && m.motion.phase == 10);
    q2_cre_motion_tick(&m, 10, &world);
    CHECK(m.motion.ent.pos[0] == 49 && m.motion.ent.pos[2] == -49);
    m.think = advance; m.next_think = 1;
    CHECK(q2_cre_motion_tick(&m, 10, &world) == 1);
    CHECK(seen[0] == 62 && seen[2] == -62 && thought_clock == 1);
    CHECK(m.pos[0] == 152 && m.motion.target[0] == 152);
    CHECK(m.motion.ent.pos[0] == 62 && m.motion.phase == 0);
    CHECK(q2_level_state.time == 0); /* actor restores the general clock */
    m.pos[0] = 1000; m.angles[2] = 1024; q2_link_entity(&m, 5);
    CHECK(m.motion.ent.pos[0] == 1000 && m.motion.yaw == 1024);
    m.motion.velocity[0] = 320; m.motion.impulse[0] = 640;
    m.motion.impulse[1] = -6000; m.motion.flags |= 0x4000u;
    q2_actor_init(&actor); q2_actor_from_monster(&actor, &m);
    CHECK(actor.knocked && actor.knockback[0] == 640 && actor.origin[0] == 1000);
    q2_cre_motion_tick(&m, 10, &world);
    CHECK(m.motion.velocity[0] == 960 && m.motion.velocity[1] == -3072);
    CHECK(m.motion.delta[0] == 30 && !m.motion.impulse[0]);
    CHECK(!(m.motion.flags & 0x4000u) && m.motion.ent.pos[0] == 1000);
    q2_cre_motion_tick(&m, 10, &world);
    CHECK(m.motion.ent.pos[0] == 1000); /* next interpolation replaces delta */
    m.motion.impulse[2] = 777; m.motion.flags |= 0x4000u;
    m.svflags |= Q2_SVF_DEADMONSTER; m.dead = true; m.health = -1; m.gib_health = -100;
    saved = m.motion;
    q2_cre_motion_tick(&m, 10, &world);
    CHECK(m.corpse && m.motion.impulse[2] == 777);
    CHECK(!memcmp(saved.ent.pos, m.motion.ent.pos, sizeof(saved.ent.pos)));
    CHECK(!memcmp(saved.velocity, m.motion.velocity, sizeof(saved.velocity)));
    m.dissolve_arm = Q2_CORPSE_DISSOLVE_FX0; m.dissolve = 4096;
    q2_cre_motion_tick(&m, 10, &world);
    CHECK(m.dissolve == 3456); /* render dt, not an entire AI interval */
    init(&m); m.motion.phase_base = 25; m.motion.phase = 5;
    world.suspended = true; q2_level_state.time = 27;
    q2_cre_motion_tick(&m, 10, &world);
    CHECK(m.motion.clock == 27 && m.motion.phase == 5);
    world.suspended = false; m.think = advance; m.next_think = 28;
    CHECK(q2_cre_motion_tick(&m, 20, &world) == 1);
    CHECK(m.motion.phase == 25 && thought_clock == 28);

    /* The deferred turn is a signed halfword value, not a pointer. */
    m.motion.advanced = true; m.motion.anim_flags = 2;
    m.motion.queued_yaw = 65535; m.motion.yaw = 100;
    m.motion.target_yaw = 200; m.angles[2] = 300;
    q2_cre_motion_animate(&m, 0, 10, 0);
    CHECK(m.motion.yaw == 99 && m.motion.target_yaw == 199 && m.angles[2] == 299);
    CHECK(m.motion.queued_yaw == 0);
}

/* A deliberately small R3000 instruction runner for these functions only.
 * Unsupported instructions and runaway control flow fail the test. External
 * presentation/collision calls are explicit stubs, never silently skipped. */
enum { RAM = 0x200000, ACT = 0x1F0000, EDICT = 0x1F1000 };
typedef struct cpu { u8 *mem; u32 r[32], hi, lo, pc, next; bool actor; } cpu;
static u32 rd(cpu *c, u32 a, unsigned n)
{
    u32 v = 0; unsigned k; a &= 0x1FFFFFu;
    if (a + n > RAM) abort();
    for (k = 0; k < n; k++) v |= (u32)c->mem[a+k] << (8*k);
    return v;
}
static void wr(cpu *c, u32 a, u32 v, unsigned n)
{
    unsigned k; a &= 0x1FFFFFu;
    if (a + n > RAM) abort();
    for (k = 0; k < n; k++) c->mem[a+k] = (u8)(v >> (8*k));
}
static bool run(cpu *c, u32 start, u32 end);
static bool hook(cpu *c)
{
    u32 pc = c->pc, a = c->r[4]; unsigned k;
    if (pc == 0x80089E18u) memset(c->mem+(a&0x1FFFFFu), (int)c->r[5], c->r[6]);
    else if (c->actor && (pc == 0x8005B880u || pc == 0x8007E9DCu ||
                         pc == 0x80089E38u || pc == 0x8005F4A8u)) { }
    else if (c->actor && pc == 0x800578D8u) c->r[2] = 0;
    else if (c->actor && pc == 0x8004583Cu) {
        /* Geometry-free move: interpolation supplies a halfword delta. */
        u32 ret = c->r[31], integrate = c->r[6];
        for (k = 0; k < 3; k++) wr(c, a+84+4*k,
            rd(c,a+84+4*k,4)+(u32)(s32)(s16)rd(c,a+236+2*k,2),4);
        wr(c,a+102,0,2); wr(c,a+104,4096,2); wr(c,a+106,0,2);
        if (integrate) {
            u32 regs[32], hi=c->hi, lo=c->lo;
            memcpy(regs,c->r,sizeof(regs)); c->r[16]=a;
            if (!run(c,0x80045E74u,0x80046204u)) return false;
            memcpy(c->r,regs,sizeof(regs)); c->hi=hi; c->lo=lo;
        }
        c->r[31]=ret;
    } else if (c->actor && pc == 0x801E0000u) {
        wr(c,a,rd(c,a,4)+90,4); wr(c,a+8,rd(c,a+8,4)-90,4);
        for (k=0;k<3;k++) wr(c,ACT+32+4*k,rd(c,a+4*k,4),4);
        wr(c,a+144,rd(c,0x800E46DCu,4)+1,4);
    } else return false;
    c->pc=c->r[31]; c->next=c->pc+4; return true;
}
static bool run(cpu *c, u32 start, u32 end)
{
    unsigned steps;
    c->pc=start; c->next=start+4;
    for (steps=0;steps<200000 && c->pc!=end;steps++) {
        u32 pc=c->pc, w, op, s,t,d,h,f, branch=0, addr; s32 imm;
        if (hook(c)) continue;
        w=rd(c,pc,4); op=w>>26; s=(w>>21)&31; t=(w>>16)&31;
        d=(w>>11)&31; h=(w>>6)&31; f=w&63; imm=(s16)w;
        c->pc=c->next; c->next+=4; addr=c->r[s]+(u32)imm;
        if (!op) switch (f) {
        case 0: c->r[d]=c->r[t]<<h; break;
        case 2: c->r[d]=c->r[t]>>h; break;
        case 3: c->r[d]=(u32)((s32)c->r[t]>>h); break;
        case 4: c->r[d]=c->r[t]<<(c->r[s]&31); break;
        case 8: branch=c->r[s]; break;
        case 9: c->r[d]=pc+8; branch=c->r[s]; break;
        case 16: c->r[d]=c->hi; break;
        case 18: c->r[d]=c->lo; break;
        case 24: { s64 v=(s64)(s32)c->r[s]*(s32)c->r[t]; c->lo=(u32)v; c->hi=(u32)((u64)v>>32); break; }
        case 26: if (!c->r[t]) return false; c->lo=(u32)((s64)(s32)c->r[s]/(s32)c->r[t]); c->hi=(u32)((s64)(s32)c->r[s]%(s32)c->r[t]); break;
        case 33: c->r[d]=c->r[s]+c->r[t]; break;
        case 35: c->r[d]=c->r[s]-c->r[t]; break;
        case 36: c->r[d]=c->r[s]&c->r[t]; break;
        case 37: c->r[d]=c->r[s]|c->r[t]; break;
        case 38: c->r[d]=c->r[s]^c->r[t]; break;
        case 39: c->r[d]=~(c->r[s]|c->r[t]); break;
        case 42: c->r[d]=(s32)c->r[s]<(s32)c->r[t]; break;
        case 43: c->r[d]=c->r[s]<c->r[t]; break;
        default: goto unsupported;
        } else switch(op) {
        case 1: if (t>1) goto unsupported; if (((s32)c->r[s]<0)==(t==0)) branch=pc+4+(u32)(imm*4); break;
        case 2: branch=((pc+4)&0xF0000000u)|((w&0x3FFFFFFu)<<2); break;
        case 3: c->r[31]=pc+8; branch=((pc+4)&0xF0000000u)|((w&0x3FFFFFFu)<<2); break;
        case 4: if(c->r[s]==c->r[t]) branch=pc+4+(u32)(imm*4); break;
        case 5: if(c->r[s]!=c->r[t]) branch=pc+4+(u32)(imm*4); break;
        case 6: if((s32)c->r[s]<=0) branch=pc+4+(u32)(imm*4); break;
        case 7: if((s32)c->r[s]>0) branch=pc+4+(u32)(imm*4); break;
        case 9: c->r[t]=addr; break;
        case 10: c->r[t]=(s32)c->r[s]<imm; break;
        case 11: c->r[t]=c->r[s]<(u32)imm; break;
        case 12: c->r[t]=c->r[s]&(w&65535); break;
        case 13: c->r[t]=c->r[s]|(w&65535); break;
        case 14: c->r[t]=c->r[s]^(w&65535); break;
        case 15: c->r[t]=w<<16; break;
        case 32: c->r[t]=(u32)(s32)(s8)rd(c,addr,1); break;
        case 33: c->r[t]=(u32)(s32)(s16)rd(c,addr,2); break;
        case 34: { unsigned n=addr&3, shift=8*(3-n); u32 mask=0xFFFFFFFFu<<shift; c->r[t]=(c->r[t]&~mask)|(rd(c,addr&~3u,4)<<shift); break; }
        case 35: c->r[t]=rd(c,addr,4); break;
        case 36: c->r[t]=rd(c,addr,1); break;
        case 37: c->r[t]=rd(c,addr,2); break;
        case 38: { unsigned shift=8*(addr&3); u32 mask=0xFFFFFFFFu>>shift; c->r[t]=(c->r[t]&~mask)|(rd(c,addr&~3u,4)>>shift); break; }
        case 40: wr(c,addr,c->r[t],1); break;
        case 41: wr(c,addr,c->r[t],2); break;
        case 42: { unsigned n=(addr&3)+1; wr(c,addr&~3u,c->r[t]>>(8*(4-n)),n); break; }
        case 43: wr(c,addr,c->r[t],4); break;
        case 46: wr(c,addr,c->r[t],4-(addr&3)); break;
        default: goto unsupported;
        }
        if(branch) c->next=branch;
        c->r[0]=0; continue;
unsupported:
        fprintf(stderr,"unsupported native instruction %08X at %08X\n",w,pc); return false;
    }
    return c->pc==end;
}
static u32 random_word(u32 *seed) { *seed=*seed*1664525u+1013904223u; return *seed; }
static void test_native_edge_branches(dat_archive *ar,cpu *c)
{
    const dat_chunk *bin=dat_find(ar,"CreAIBin"),*rel=dat_find(ar,"CreAIRel");
    u32 boff=0,roff=0;
    while(bin && rel && boff+16<bin->size && roff+16<rel->size) {
        u32 bn=q2_rd_u32(bin->data+boff+12),rn=q2_rd_u32(rel->data+roff+12);
        u32 size=(bn?bn:bin->size)-boff-16;
        bool strider=!strncmp((const char *)bin->data+boff,"Strider",12);
        bool boss1=!strncmp((const char *)bin->data+boff,"Boss1",12);
        if(strider || boss1) {
            q2_reloc_stats stats; unsigned sample,k;
            CHECK(size<0xE0000); if(size>=0xE0000)return;
            memcpy(c->mem+0x100000,bin->data+boff+16,size);
            CHECK(q2_reloc_apply(c->mem+0x100000,size,rel->data+roff+16,
                  (rn?rn:rel->size)-roff-16,0x80100000u,&stats)==Q2_OK);
            for(sample=0;sample<4;sample++) {
                memset(c->r,0,sizeof(c->r)); c->actor=false;
                if(strider) {
                    /* Enter at the exact-yaw branch, before its delay slot.
                     * Equal yaw preserves s2; unequal yaw overwrites it. */
                    c->r[16]=512; c->r[2]=sample<2?512:513;
                    c->r[18]=sample&1; c->r[17]=0x801F1000u;
                    wr(c,0x801F108Cu,20,2);
                    CHECK(run(c,0x80101278u,0x801012C4u));
                    CHECK(c->r[18]==(sample<2?(sample&1):1));
                } else {
                    /* Same actor, different prior stack contents: the NULL
                     * enemy branch forwards those bytes to fire_bullet. */
                    c->r[17]=0x801F1000u; c->r[18]=0x80100000u; c->r[29]=0x801FE000u;
                    wr(c,0x801F10BCu,0,4);
                    for(k=0;k<3;k++)wr(c,0x801FE048u+4*k,0x12340000u+sample*100+k,4);
                    CHECK(run(c,0x80100B44u,0x80100C14u));
                    CHECK(c->r[6]==0x801FE048u);
                    for(k=0;k<3;k++)CHECK(rd(c,c->r[6]+4*k,4)==0x12340000u+sample*100+k);
                }
            }
        }
        if(!bn || !rn)break; boff=bn;roff=rn;
    }
}
static void test_native_poses(disc *d,cpu *c)
{
    int file; unsigned clips=0, poses=0, parts=0;
    c->actor=false;
    for(file=0;file<disc_file_count(d);file++) {
        const char *path=disc_file_at(d,file)->path,*tail;
        q2_buf b={0}; dat_archive ar; q2_common_file view={0}; q2_model_bank bank={0};
        u32 model;
        if(*path=='/')path++;
        if(strncmp(path,"Q2DATA/LEVELS/",14))continue;
        tail=strchr(path+14,'/');
        if(!tail || (strcmp(tail+1,"COMMON.DAT") && strncmp(tail+1,"ZONE",4)))continue;
        CHECK(disc_read_file(d,path,&b)==Q2_OK); if(!b.data)continue;
        CHECK(dat_open_buf(&ar,&b)==Q2_OK); if(b.data) {q2_buf_free(&b);continue;}
        test_native_edge_branches(&ar,c);
        view.chunk[Q2_COMMON_CAST_LIST]=dat_find(&ar,"CastList");
        if(!view.chunk[Q2_COMMON_CAST_LIST]) {dat_close(&ar);continue;}
        CHECK(q2_model_bank_from_common(&bank,&view)==Q2_OK);
        for(model=0;model<bank.count;model++) {
            q2_model m; q2_model_anim clip; u32 j;
            if(q2_model_get(&bank,model,&m)!=Q2_OK) {CHECK(false);continue;}
            CHECK(m.size<0xE0000 && m.hdr.num_parts<=256);
            if(m.size>=0xE0000 || m.hdr.num_parts>256)continue;
            memcpy(c->mem+0x100000,m.base,m.size);
            for(j=0;q2_model_anim_get(&m,j,&clip);j++)
                wr(c,0x80100000u+clip.offset,clip.frames*10u,2); /* BodgeAnimData */
            for(j=0;q2_model_anim_get(&m,j,&clip);j++) {
                q2_model_anim next; unsigned sample;
                if(!(clip.flags&1) || !clip.frames)continue;
                if(!q2_model_anim_get(&m,j+1,&next) || !(next.flags&1))next=clip;
                clips++;
                for(sample=0;sample<8;sample++) {
                    q2_model_pose got[256]; u32 last=(clip.frames-1u)*10u,part,k;
                    u32 tick=sample==0?0:sample==1?1:sample==2?5:sample==3?last/2:
                             sample==4?(last?last-1:0):sample==5?last:sample==6?last+5:last+10;
                    bool transition=sample>=6,ok; unsigned old_fail=failures;
                    memset(got,0xCD,sizeof(got)); memset(c->mem+0xDD960,0xCD,4096);
                    memset(c->r,0,sizeof(c->r)); c->r[4]=0x80100000u+clip.offset;
                    c->r[5]=transition?0x80100000u+next.offset:0; c->r[6]=tick;
                    c->r[7]=m.hdr.num_parts; c->r[29]=0x801FF000u; c->r[31]=0x801FFFFCu;
                    ok=run(c,0x8006B4DCu,0x801FFFFCu); CHECK(ok);
                    CHECK(q2_model_pose_transition(&m,&clip,tick,transition?&next:NULL,got)==Q2_OK);
                    if(ok)for(part=0;part<m.hdr.num_parts;part++) {
                        for(k=0;k<3;k++)CHECK(got[part].t[k]==(s16)rd(c,0x800DD960u+part*16+2*k,2));
                        for(k=0;k<4;k++)CHECK(got[part].q[k]==(s16)rd(c,0x800DD966u+part*16+2*k,2));
                        parts++;
                    }
                    if(failures!=old_fail) {
                        fprintf(stderr,"pose mismatch %s %s clip %u tick %u transition %u\n",path,m.hdr.name,j,tick,transition);
                        dat_close(&ar); return;
                    }
                    poses++;
                }
            }
        }
        dat_close(&ar);
    }
    printf("%u variable clips, %u native poses, %u part poses\n",clips,poses,parts);
    CHECK(clips>3000);
}
static void test_native(const char *path)
{
    disc *d=NULL; q2_exe exe={0}; cpu c={0}; u32 seed=19; unsigned i,k;
    CHECK(disc_open(&d,path)==Q2_OK);
    if (!d) return;
    CHECK(q2_exe_load(&exe,d,NULL)==Q2_OK);
    if (!exe.file.data) { disc_close(d); return; }
    CHECK(!strcmp(exe.name,"SLES_015.34"));
    c.mem=calloc(1,RAM); if(!c.mem) abort();
    memcpy(c.mem+(exe.text_addr&0x1FFFFFu),exe.file.data+Q2_EXE_HEADER_SIZE,exe.text_size);
    /* Read every native trigonometric entry, then execute the decoder,
     * quaternion matrix and slerp instructions on independently packed keys. */
    for (i=0;i<4096;i++) {
        CHECK(q2_acos12(((s32)i-2048)*2)==(s16)rd(&c,0x8009FC44u+2*i,2));
        CHECK(q2_sin12((s32)i)==(s16)rd(&c,0x800A5430u+4*i,2));
        CHECK(q2_cos12((s32)i)==(s16)rd(&c,0x800A5432u+4*i,2));
    }
    for (i=0;i<=1024;i++)
        CHECK(q2_ratan2((s32)i,1024)==(s16)rd(&c,0x800A9430u+2*i,2));
    for (i=0;i<10000;i++) {
        s32 v[3]={(s32)random_word(&seed),0,(s32)random_word(&seed)};
        if (i<4096) { v[0]>>=16; v[2]>>=16; }
        if (i%20==0) v[0]=0;
        if (i%21==0) v[2]=0;
        if (v[0]==INT_MIN) v[0]++;
        if (v[2]==INT_MIN) v[2]++;
        memset(c.r,0,sizeof(c.r)); c.actor=false;
        c.r[4]=(u32)v[0]; c.r[5]=(u32)v[2]; c.r[31]=0x801FFFFCu;
        CHECK(run(&c,0x8008A358u,0x801FFFFCu));
        CHECK(q2_ratan2(v[0],v[2])==(s32)c.r[2]);
        for(k=0;k<3;k++) wr(&c,0x801F0100u+4*k,(u32)v[k],4);
        memset(c.r,0,sizeof(c.r)); c.r[4]=0x801F0100u;
        c.r[29]=0x801FF000u; c.r[31]=0x801FFFFCu;
        CHECK(run(&c,0x8005F8E8u,0x801FFFFCu));
        CHECK(q2_vectoyaw(v)==(s32)c.r[2]);
    }
    for (i=0;i<5000;i++) {
        u32 packed=random_word(&seed); s16 qa[4],qb[4],got[4],matrix[3][3];
        s32 t=i%5==0?0:i%5==1?4096:(s32)(random_word(&seed)%4097);
        memset(c.r,0,sizeof(c.r)); c.actor=false;
        c.r[4]=packed; c.r[5]=0x801F0100u; c.r[29]=0x801FF000u; c.r[31]=0x801FFFFCu;
        CHECK(run(&c,0x800699E8u,0x801FFFFCu));
        q2_model_key_rotation(packed,qa);
        for(k=0;k<4;k++) CHECK(qa[k]==(s16)rd(&c,0x801F0100u+2*k,2));
        q2_model_key_rotation(random_word(&seed),qb);
        if (i%7==0) memcpy(qb,qa,sizeof(qb));
        if (i%7==1) for(k=0;k<4;k++) qb[k]=(s16)-qa[k];
        q2_quat_slerp(qa,qb,t,got);
        for(k=0;k<4;k++) wr(&c,0x801F0120u+2*k,(u16)qb[k],2);
        memset(c.r,0,sizeof(c.r)); c.r[4]=0x801F0100u; c.r[5]=0x801F0120u;
        c.r[6]=(u32)t; c.r[7]=0x801F0140u; c.r[29]=0x801FF000u; c.r[31]=0x801FFFFCu;
        CHECK(run(&c,0x80069C64u,0x801FFFFCu));
        for(k=0;k<4;k++) CHECK(got[k]==(s16)rd(&c,0x801F0140u+2*k,2));
        q2_quat_to_matrix(matrix,got);
        memset(c.r,0,sizeof(c.r)); c.r[4]=0x801F0140u; c.r[5]=0x801F0160u;
        c.r[29]=0x801FF000u; c.r[31]=0x801FFFFCu;
        CHECK(run(&c,0x80069B54u,0x801FFFFCu));
        for(k=0;k<9;k++) CHECK(matrix[k/3][k%3]==(s16)rd(&c,0x801F0160u+2*k,2));
    }
    for(i=0;i<6000;i++) {
        q2_move_ent ent={0}; s16 v[3],impulse[3],delta[3]; u32 flags;
        s32 dt=1+(s32)(random_word(&seed)%60); bool mp=(i&1)!=0;
        memset(c.mem+ACT,0,1024); memset(c.r,0,sizeof(c.r));
        c.actor=false; c.r[16]=0x80000000u+ACT; c.r[28]=0x800AE600u; c.r[29]=0x801FF000u;
        flags=random_word(&seed)&0x5000u; ent.flags=random_word(&seed)&0x29F8u;
        wr(&c,ACT+268,flags,4); wr(&c,ACT+152,ent.flags,4);
        wr(&c,0x800B2DB4u,(u32)dt,4); wr(&c,0x800AE924u,32,4); wr(&c,0x800AEBCCu,mp,4);
        for(k=0;k<3;k++) {
            v[k]=(s16)random_word(&seed); impulse[k]=(s16)random_word(&seed);
            ent.ground_normal[k]=(s16)random_word(&seed); ent.last_normal[k]=(s16)random_word(&seed);
            wr(&c,ACT+224+2*k,(u16)v[k],2); wr(&c,ACT+760+2*k,(u16)impulse[k],2);
            wr(&c,ACT+96+2*k,(u16)ent.ground_normal[k],2); wr(&c,ACT+102+2*k,(u16)ent.last_normal[k],2);
        }
        if(!run(&c,0x80045E74u,0x80046204u)) { CHECK(false); break; }
        q2_move_integrate(&ent,v,impulse,&flags,32,dt,mp,delta);
        CHECK(flags==rd(&c,ACT+268,4) && ent.flags==rd(&c,ACT+152,4));
        for(k=0;k<3;k++) {
            CHECK(v[k]==(s16)rd(&c,ACT+224+2*k,2));
            CHECK(delta[k]==(s16)rd(&c,ACT+236+2*k,2));
            CHECK(impulse[k]==(s16)rd(&c,ACT+760+2*k,2));
            CHECK(ent.ground_normal[k]==(s16)rd(&c,ACT+96+2*k,2));
            CHECK(ent.last_normal[k]==(s16)rd(&c,ACT+102+2*k,2));
        }
    }
    for(i=0;i<2000;i++) {
        q2_monster m; q2_cre_motion_world world={0}; s32 dt=1+(s32)(random_word(&seed)%60);
        init(&m); m.think=advance; m.next_think=1;
        memset(c.mem+ACT,0,1024); memset(c.mem+EDICT,0,512); memset(c.r,0,sizeof(c.r));
        c.actor=true; c.r[4]=0x80000000u+ACT; c.r[28]=0x800AE600u; c.r[29]=0x801FF000u; c.r[31]=0x801FFFFCu;
        wr(&c,ACT+748,0x80000000u+EDICT,4); wr(&c,ACT+264,100,2);
        wr(&c,ACT+268,1,4); wr(&c,EDICT+36,0x80000000u+ACT,4);
        wr(&c,EDICT+28,0x20000000u,4); wr(&c,EDICT+144,1,4); wr(&c,EDICT+148,0x801E0000u,4);
        wr(&c,0x800B2DB4u,(u32)dt,4); wr(&c,0x800AE924u,32,4); wr(&c,0x800AEBCCu,0,4);
        m.motion.phase_base=(u8)(random_word(&seed)%30); m.motion.phase=(u8)(random_word(&seed)%30);
        wr(&c,EDICT+44,m.motion.phase_base,1); wr(&c,EDICT+45,m.motion.phase,1);
        m.motion.yaw=(s16)random_word(&seed); m.motion.target_yaw=(s16)random_word(&seed);
        wr(&c,ACT+232,(u16)m.motion.yaw,2); wr(&c,ACT+218,(u16)m.motion.target_yaw,2);
        for(k=0;k<3;k++) {
            m.motion.ent.pos[k]=(s32)random_word(&seed); m.motion.target[k]=(s32)random_word(&seed);
            wr(&c,ACT+84+4*k,(u32)m.motion.ent.pos[k],4); wr(&c,ACT+32+4*k,(u32)m.motion.target[k],4);
        }
        if(!run(&c,0x8007EBC4u,0x801FFFFCu)) { CHECK(false); break; }
        q2_cre_motion_tick(&m,dt,&world);
        CHECK(m.motion.phase==rd(&c,EDICT+45,1));
        CHECK(m.motion.yaw==(s16)rd(&c,ACT+232,2));
        CHECK(m.motion.clock==(s32)rd(&c,EDICT+52,4));
        for(k=0;k<3;k++) {
            CHECK(m.motion.ent.pos[k]==(s32)rd(&c,ACT+84+4*k,4));
            CHECK(m.motion.target[k]==(s32)rd(&c,ACT+32+4*k,4));
            CHECK(m.motion.delta[k]==(s16)rd(&c,ACT+236+2*k,2));
        }
    }
    for(i=0;i<2000;i++) {
        q2_monster m; u32 base, flags;
        init(&m); memset(c.r,0,sizeof(c.r));
        memset(c.mem+ACT,0,1024); memset(c.mem+EDICT,0,512);
        c.actor=false; c.r[4]=0x80000000u+ACT; c.r[31]=0x801FFFFCu;
        m.frame=(s16)(8+random_word(&seed)%16);
        m.motion.anim_base=(u16)(random_word(&seed)%1000);
        m.motion.anim_previous=(s16)random_word(&seed);
        m.motion.anim_position=(s16)random_word(&seed);
        m.motion.anim_flags=(u8)(random_word(&seed)&3u);
        m.motion.flags=random_word(&seed)&0x40u;
        m.motion.phase=(u8)(random_word(&seed)%30);
        m.motion.advanced=(random_word(&seed)&1u)!=0;
        c.r[5]=m.motion.advanced; c.r[6]=m.motion.phase;
        wr(&c,ACT+748,0x80000000u+EDICT,4); wr(&c,EDICT+40,0x801F2000u,4);
        wr(&c,0x801F2000u,10,2); wr(&c,0x801F2002u,20,2);
        wr(&c,0x801F2012u,300,2); wr(&c,0x801F2014u,65535,2);
        wr(&c,EDICT+56,(u16)m.frame,2); wr(&c,EDICT+46,(u16)m.motion.anim_previous,2);
        wr(&c,ACT+256,(u16)m.motion.anim_position,2); wr(&c,ACT+268,m.motion.flags,4);
        base=((u32)m.motion.anim_base<<17)|m.motion.anim_flags;
        wr(&c,ACT+176,base,4);
        if(!run(&c,0x8007E9DCu,0x801FFFFCu)) { CHECK(false); break; }
        q2_cre_motion_animate(&m,10,20,300);
        flags=rd(&c,ACT+176,4);
        CHECK(m.motion.anim_base==(flags>>17));
        CHECK(m.motion.anim_flags==(flags&3u));
        CHECK(m.motion.anim_previous==(s16)rd(&c,EDICT+46,2));
        CHECK(m.motion.anim_position==(s16)rd(&c,ACT+256,2));
        CHECK(m.motion.flags==rd(&c,ACT+268,4));
    }
    for(i=0;i<1000;i++) {
        enum { GRAPH=0x1E1000, SCENE=0x1E2000, AREAS=12 };
        q2_area_graph graph={c.mem+GRAPH,2048,AREAS};
        q2_scene scene={0}; u32 mask[3], at=2+2*AREAS, area;
        q2_monster m; s32 camera[3]; bool visible=(i%3)==0;
        u32 frame=1+i%4, players=1+i%2;
        u8 start=(u8)(random_word(&seed)%AREAS);
        memset(c.mem+GRAPH,0,2048); memset(c.mem+SCENE,0,52*16);
        wr(&c,GRAPH,AREAS,2);
        scene.nodes=c.mem+SCENE; scene.node_count=16;
        for(k=0;k<16;k++) wr(&c,SCENE+52*k+8,random_word(&seed)&0x8000u,4);
        for(area=1;area<AREAS;area++) {
            u32 count=1+random_word(&seed)%4, payload=at+1+count;
            payload+=(1+count)&1;
            wr(&c,GRAPH+2+2*area,at,2); wr(&c,GRAPH+at,count,1);
            for(k=0;k<count;k++) {
                wr(&c,GRAPH+at+1+k,random_word(&seed)%AREAS,1);
                wr(&c,GRAPH+payload+8*k,random_word(&seed)%16,2);
            }
            at=payload+count*8;
        }
        c.actor=false; memset(c.r,0,sizeof(c.r));
        c.r[28]=0x800AE600u; c.r[29]=0x801FF000u; c.r[31]=0x801FFFFCu;
        wr(&c,0x800B2CA4u,0x80000000u+GRAPH,4);
        wr(&c,0x800B2C78u,0x80000000u+GRAPH+2,4);
        wr(&c,0x800B2C3Cu,0x80000000u+SCENE,4);
        wr(&c,0x800B2DE4u,frame,4); wr(&c,0x800B2C2Cu,players,4);
        wr(&c,0x800D5CCEu,start,2);
        if(!run(&c,0x800651D4u,0x801FFFFCu)) { CHECK(false); break; }
        q2_cre_active_areas(&graph,&scene,NULL,0,start,frame==1||players!=1,mask);
        for(k=0;k<3;k++) CHECK(mask[k]==rd(&c,0x800DD944u+4*k,4));
        init(&m); area=random_word(&seed)%96;
        m.motion.flags=random_word(&seed);
        wr(&c,ACT+158,area,1); wr(&c,ACT+268,m.motion.flags,4);
        for(k=0;k<3;k++) {
            camera[k]=(s32)random_word(&seed);
            m.motion.ent.pos[k]=(s32)((u32)camera[k]+(random_word(&seed)%32768)-16384u);
            wr(&c,0x800D5C30u+4*k,(u32)camera[k],4);
            wr(&c,ACT+84+4*k,(u32)m.motion.ent.pos[k],4);
        }
        wr(&c,0x800B2D68u,123,2);
        for(k=0;k<4;k++) wr(&c,0x800D8D78u+area*64+16*k,visible?123:122,2);
        memset(c.r,0,sizeof(c.r)); c.r[4]=0x80000000u+ACT;
        c.r[29]=0x801FF000u; c.r[31]=0x801FFFFCu;
        if(!run(&c,0x800578D8u,0x801FFFFCu)) { CHECK(false); break; }
        CHECK(q2_cre_motion_suspend(&m,mask,(s32)area,visible,camera)==(c.r[2]!=0));
        CHECK(m.motion.flags==rd(&c,ACT+268,4));
    }
    test_native_poses(d,&c);
    free(c.mem); q2_exe_free(&exe); disc_close(d);
}
int main(int argc,char **argv)
{
    test_actor();
    if(argc==3 && !strcmp(argv[1],"--disc")) test_native(argv[2]);
    printf("%u creature motion checks, %u failures\n",checks,failures);
    return failures?1:0;
}

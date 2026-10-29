/* Execute a creature's export-2 initializer in isolated R3000 memory.
 * Only the initialization imports listed below are provided. No PS1 code is
 * executed on the host, and malformed instructions, addresses and loops fail.
 * Sound handles carry provenance separately from their zero/nonzero value:
 * a missing sample must still take the module's actual fallback branches. */
#include "creature.h"
#include <stdlib.h>
#include <string.h>

#define INIT_STACK 0x7FFE0000u
#define INIT_IMPORT 0x7FFD0000u
#define INIT_MODEL 0x7FFC0000u
#define INIT_RETURN 0x7FFFFFFCu
#define INIT_HANDLE 0x70000000u
#define INIT_LIMIT 128u

typedef struct init_value { u32 word; u16 sound; } init_value;
typedef struct init_vm {
    u8 *image; u16 *tags; size_t size; u32 base;
    u8 stack[4096]; u16 stack_tags[1024];
    u8 model[256]; u16 model_tags[64];
    init_value r[32], pending; u32 pending_reg, pc, next, hi, lo, fault_pc;
    char names[INIT_LIMIT][13]; u32 names_count;
    q2_sfx_request requests[INIT_LIMIT]; bool cloned[INIT_LIMIT];
    q2_cre_sound_bind trace[INIT_LIMIT]; u32 trace_count;
    bool (*has)(const char *, void *); void *user;
    q2_sfx_request *(*lookup)(const char *, void *);
    bool failed;
} init_vm;

static u8 *init_ptr(init_vm *v, u32 addr, u32 n, u16 **tag)
{
    u32 off = addr - v->base;
    if (off <= v->size && n <= v->size - off) {
        if (tag) *tag = v->tags + off / 4;
        return v->image + off;
    }
    off = addr - INIT_STACK;
    if (off <= sizeof(v->stack) && n <= sizeof(v->stack) - off) {
        if (tag) *tag = v->stack_tags + off / 4;
        return v->stack + off;
    }
    off = addr - INIT_MODEL;
    if (off <= sizeof(v->model) && n <= sizeof(v->model) - off) {
        if (tag) *tag = v->model_tags + off / 4;
        return v->model + off;
    }
    v->failed = true;
    return NULL;
}
static init_value init_read(init_vm *v, u32 addr, u32 n)
{
    init_value x = {0}; u16 *tag = NULL; u8 *p = init_ptr(v, addr, n, &tag); u32 i;
    if (p) {
        for (i = 0; i < n; i++) x.word |= (u32)p[i] << (i * 8);
        if (n == 4 && !(addr & 3)) x.sound = *tag;
    }
    return x;
}
static void init_write(init_vm *v, u32 addr, init_value x, u32 n)
{
    u16 *tag = NULL; u8 *p = init_ptr(v, addr, n, &tag); u32 i;
    if (!p) return;
    for (i = 0; i < n; i++) p[i] = (u8)(x.word >> (i * 8));
    *tag = n == 4 && !(addr & 3) ? x.sound : 0;
    if (addr >= v->base && addr - v->base < v->size && x.sound && n == 4) {
        const char *name = v->names[x.sound - 1];
        for (i = 0; i < v->trace_count; i++)
            if (v->trace[i].addr == addr && !strcmp(v->trace[i].name, name)) return;
        if (i == INIT_LIMIT) { v->failed = true; return; }
        v->trace[i].addr = addr; memcpy(v->trace[i].name, name, 13);
        v->trace[i].request_id = x.sound;
        v->trace[i].cloned = v->cloned[x.sound - 1];
        v->trace[i].request = v->requests[x.sound - 1]; v->trace_count++;
    }
}
static void init_flush_load(init_vm *v)
{
    if (v->pending_reg) v->r[v->pending_reg] = v->pending;
    v->pending_reg = 0;
}
static bool init_import(init_vm *v, u32 slot)
{
    init_value result = {0};
    init_flush_load(v);
    switch (slot) {
    case 0x24: { /* SoundIndex: 12-byte name passed BY VALUE in a0/a1/a2. */
        char name[13]; u32 i, id;
        for (i = 0; i < 12; i++) name[i] = (char)(v->r[4 + i / 4].word >> ((i % 4) * 8));
        name[12] = 0;
        for (i = 0; i < 12 && name[i]; i++)
            if ((u8)name[i] < 32 || (u8)name[i] > 126) return false;
        if (!i) return false;
        for (id = 0; id < v->names_count; id++)
            if (!v->cloned[id] && !strcmp(v->names[id], name)) break;
        if (id == INIT_LIMIT) return false;
        if (id == v->names_count) {
            q2_sfx_request *shared = v->lookup ? v->lookup(name, v->user) : NULL;
            memcpy(v->names[id], name, 13);
            if (shared) v->requests[id] = *shared;
            else q2_sfx_request_init(&v->requests[id]);
            v->names_count++;
        }
        result.sound = (u16)(id + 1);
        if (!v->has || v->has(name, v->user)) result.word = INIT_HANDLE + 4 * (id + 1);
        break;
    }
    case 0x3C: { /* CloneRequest copies state into an independent request. */
        u32 from = v->r[4].sound, id = v->names_count;
        if (!from || from > id || id == INIT_LIMIT) return false;
        memcpy(v->names[id], v->names[from - 1], 13);
        v->requests[id] = v->requests[from - 1]; v->cloned[id] = true;
        v->names_count++;
        result.sound = (u16)(id + 1);
        if (v->r[4].word) result.word = INIT_HANDLE + 4 * (id + 1);
        break;
    }
    case 0x44: { /* SetRequestParams(handle, absolute pitch, volume, parameter). */
        u32 id = v->r[4].sound;
        if (id && id <= v->names_count)
            q2_sfx_request_params(&v->requests[id - 1], (s32)v->r[5].word,
                                   (s32)v->r[6].word, (s32)v->r[7].word);
        break;
    }
    case 0x18: /* printf: diagnostics have no sound-state side effects. */
    case 0x118: /* RegisterClassMethod: module's table is already decoded. */
        break;
    case 0x34: /* Attachment initialization succeeds on the loaded model. */
        result.word = 1; break;
    case 0x74: /* Dummy move record: only non-sound frame fields read it. */
        result.word = INIT_MODEL + 128; break;
    default: return false;
    }
    v->r[2] = result;
    v->pc = v->r[31].word; v->next = v->pc + 4;
    return true;
}
static bool init_run(init_vm *v)
{
    u32 steps;
    for (steps = 0; steps < 100000 && v->pc != INIT_RETURN && !v->failed; steps++) {
        u32 pc=v->pc, w, op,s,t,d,h,f,addr,dst=0,load=0; s32 imm;
        init_value result={0};
        v->fault_pc=pc;
        if (pc >= INIT_IMPORT && pc < INIT_IMPORT + 0x130) {
            if (!init_import(v, pc - INIT_IMPORT)) return false;
            continue;
        }
        if ((pc & 3) || pc < v->base || pc - v->base >= v->size) return false;
        w=init_read(v,pc,4).word; op=w>>26; s=(w>>21)&31; t=(w>>16)&31;
        d=(w>>11)&31; h=(w>>6)&31; f=w&63; imm=(s16)w;
        addr=v->r[s].word+(u32)imm;
        v->pc=v->next; v->next+=4;
#define BRANCH(target) (v->next=(target))
#define R(n) (v->r[n].word)
        if (!op) {
            dst=d;
            switch(f) {
            case 0: result.word=R(t)<<h; if (!h) result.sound=v->r[t].sound; break;
            case 2: result.word=R(t)>>h; break;
            case 3: result.word=(u32)((s32)R(t)>>h); break;
            case 4: result.word=R(t)<<(R(s)&31); break;
            case 6: result.word=R(t)>>(R(s)&31); break;
            case 7: result.word=(u32)((s32)R(t)>>(R(s)&31)); break;
            case 8: dst=0; BRANCH(R(s)); break;
            case 9: result.word=pc+8; BRANCH(R(s)); break;
            case 16: result.word=v->hi; break;
            case 18: result.word=v->lo; break;
            case 24: { s64 x=(s64)(s32)R(s)*(s32)R(t); v->lo=(u32)x; v->hi=(u32)((u64)x>>32); dst=0; break; }
            case 25: { u64 x=(u64)R(s)*R(t); v->lo=(u32)x; v->hi=(u32)(x>>32); dst=0; break; }
            case 26: if (!R(t)) return false; v->lo=(u32)((s64)(s32)R(s)/(s32)R(t)); v->hi=(u32)((s64)(s32)R(s)%(s32)R(t)); dst=0; break;
            case 27: if (!R(t)) return false; v->lo=R(s)/R(t); v->hi=R(s)%R(t); dst=0; break;
            case 33: result.word=R(s)+R(t); if (!R(t)) result.sound=v->r[s].sound; if (!R(s) && !result.sound) result.sound=v->r[t].sound; break;
            case 35: result.word=R(s)-R(t); break;
            case 36: result.word=R(s)&R(t); break;
            case 37: result.word=R(s)|R(t); if (!R(t)) result.sound=v->r[s].sound; if (!R(s) && !result.sound) result.sound=v->r[t].sound; break;
            case 38: result.word=R(s)^R(t); break;
            case 39: result.word=~(R(s)|R(t)); break;
            case 42: result.word=(s32)R(s)<(s32)R(t); break;
            case 43: result.word=R(s)<R(t); break;
            default: return false;
            }
        } else switch(op) {
        case 1: if (t>1) return false; if (((s32)R(s)<0)==(t==0)) BRANCH(pc+4+(u32)(imm*4)); break;
        case 2: BRANCH(((pc+4)&0xF0000000u)|((w&0x3FFFFFFu)<<2)); break;
        case 3: dst=31; result.word=pc+8; BRANCH(((pc+4)&0xF0000000u)|((w&0x3FFFFFFu)<<2)); break;
        case 4: if(R(s)==R(t)) BRANCH(pc+4+(u32)(imm*4)); break;
        case 5: if(R(s)!=R(t)) BRANCH(pc+4+(u32)(imm*4)); break;
        case 6: if((s32)R(s)<=0) BRANCH(pc+4+(u32)(imm*4)); break;
        case 7: if((s32)R(s)>0) BRANCH(pc+4+(u32)(imm*4)); break;
        case 9: dst=t; result.word=addr; if (!imm) result.sound=v->r[s].sound; break;
        case 10: dst=t; result.word=(s32)R(s)<imm; break;
        case 11: dst=t; result.word=R(s)<(u32)imm; break;
        case 12: dst=t; result.word=R(s)&(w&65535); break;
        case 13: dst=t; result.word=R(s)|(w&65535); break;
        case 14: dst=t; result.word=R(s)^(w&65535); break;
        case 15: dst=t; result.word=w<<16; break;
        case 32: load=t; result=init_read(v,addr,1); result.word=(u32)(s32)(s8)result.word; break;
        case 33: if(addr&1) return false; load=t; result=init_read(v,addr,2); result.word=(u32)(s32)(s16)result.word; break;
        case 35: if(addr&3) return false; load=t; result=init_read(v,addr,4); break;
        case 36: load=t; result=init_read(v,addr,1); break;
        case 37: if(addr&1) return false; load=t; result=init_read(v,addr,2); break;
        case 40: init_write(v,addr,v->r[t],1); break;
        case 41: if(addr&1) return false; init_write(v,addr,v->r[t],2); break;
        case 43: if(addr&3) return false; init_write(v,addr,v->r[t],4); break;
        default: return false;
        }
        init_flush_load(v);
        if (dst) v->r[dst]=result;
        if (load) { v->pending_reg=load; v->pending=result; }
        v->r[0].word=0; v->r[0].sound=0;
#undef R
#undef BRANCH
    }
    return !v->failed && v->pc==INIT_RETURN;
}
static q2_result init_sounds(const u8 *image, size_t size, u32 base,
                              bool (*has)(const char *,void *), void *user,
                              q2_cre_sound_bind *out, u32 max, u32 *count,
                              u32 *fault_pc, bool trace,
                              q2_sfx_request *(*lookup)(const char *, void *))
{
    init_vm *v; u32 i,n=0; bool ok;
    if (count) *count=0;
    if (fault_pc) *fault_pc=0;
    if (!image || size<0x130 || size>0x100000 || !count || (max && !out) ||
        (base&3) || base>0xFFFFFFFFu-size ||
        (base<INIT_RETURN+4ull && (u64)base+size>INIT_MODEL)) return Q2_ERR_INVALID_ARG;
    v=calloc(1,sizeof(*v)); if (!v) return Q2_ERR_NO_MEMORY;
    v->image=malloc(size); v->tags=calloc((size+3)/4,sizeof(u16));
    if (!v->image || !v->tags) { free(v->image); free(v->tags); free(v); return Q2_ERR_NO_MEMORY; }
    memcpy(v->image,image,size); v->size=size; v->base=base; v->has=has; v->user=user;
    v->lookup = lookup;
    init_write(v,base+16,(init_value){0x00010130u,0},4);
    for(i=0x14;i<0x130;i+=4) init_write(v,base+i,(init_value){INIT_IMPORT+i,0},4);
    v->r[4].word=INIT_MODEL; v->r[29].word=INIT_STACK+sizeof(v->stack)-16;
    v->r[31].word=INIT_RETURN; v->pc=q2_rd_u32(image+8); v->next=v->pc+4;
    ok=init_run(v);
    if (!ok && fault_pc) *fault_pc=v->fault_pc;
    if (ok) for(i=0;i<(trace?v->trace_count:(u32)(size/4));i++) {
        q2_cre_sound_bind b = {0};
        if (trace) b=v->trace[i];
        else {
            u16 tag=v->tags[i];
            if (!tag || !q2_rd_u32(v->image+4*i)) continue;
            b.addr=base+4*i; memcpy(b.name,v->names[tag-1],13);
            b.request_id=tag; b.request=v->requests[tag-1];
            b.cloned=v->cloned[tag-1];
        }
        if (n<max) out[n]=b;
        n++;
    }
    if (ok && lookup) for (i=0;i<v->names_count;i++) {
        q2_sfx_request *shared;
        if (v->cloned[i]) continue;
        shared=lookup(v->names[i],user);
        if (shared) *shared=v->requests[i];
    }
    *count=n; free(v->image); free(v->tags); free(v);
    return ok ? Q2_OK : Q2_ERR_BAD_FORMAT;
}
q2_result q2_creature_init_sounds(const u8 *image, size_t size, u32 base,
                                  bool (*has)(const char *,void *), void *user,
                                  q2_cre_sound_bind *out, u32 max, u32 *count,
                                  u32 *fault_pc)
{ return init_sounds(image,size,base,has,user,out,max,count,fault_pc,false,NULL); }

q2_result q2_creature_init_sound_requests(const u8 *image, size_t size, u32 base,
    bool (*has)(const char *,void *), void *user,
    q2_sfx_request *(*lookup)(const char *,void *),
    q2_cre_sound_bind *out, u32 max, u32 *count, u32 *fault_pc)
{ return init_sounds(image,size,base,has,user,out,max,count,fault_pc,false,lookup); }

static bool init_missing(const char *name, void *user) { (void)name; (void)user; return false; }
u32 q2_creature_sound_bindings(const u8 *image, size_t size, u32 base,
                               q2_cre_sound_bind *out, u32 max)
{
    q2_cre_sound_bind all[INIT_LIMIT*2]; u32 n=0,n2=0,i,j,total=0;
    if (init_sounds(image,size,base,NULL,NULL,all,INIT_LIMIT,&n,NULL,true,NULL)!=Q2_OK ||
        init_sounds(image,size,base,init_missing,NULL,all+n,INIT_LIMIT,&n2,NULL,true,NULL)!=Q2_OK) return 0;
    for(i=0;i<n+n2;i++) {
        for(j=0;j<i;j++) if(all[j].addr==all[i].addr && !strcmp(all[j].name,all[i].name)) break;
        if(j<i) continue;
        if(total<max && out) out[total]=all[i];
        total++;
    }
    return total;
}

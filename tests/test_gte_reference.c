/* Hardware results from ps1-tests, MIT licensed. See fixtures/README.md. */
#include "gte.h"
#include <stdio.h>
#include <string.h>
#include "fixtures/gte_reference.h"

static gte_rgbc rgb(u32 value)
{
    gte_rgbc c={(u8)value,(u8)(value>>8),(u8)(value>>16),(u8)(value>>24)};
    return c;
}
static u32 pack_rgb(gte_rgbc c)
{
    return c.r | ((u32)c.g<<8) | ((u32)c.b<<16) | ((u32)c.c<<24);
}
static void matrix(gte_matrix *m,const u32 *r)
{
    int i;
    for(i=0;i<9;++i) m->m[i/3][i%3]=(s16)(r[i/2]>>((i&1)*16));
}
static void load_registers(gte_state *g,const u32 *r)
{
    int i;
    memset(g,0,sizeof(*g));
    for(i=0;i<3;++i) {
        g->v[i]=(gte_vec16){(s16)r[i*2],(s16)(r[i*2]>>16),(s16)r[i*2+1],0};
        /* Writes to SXYP (15) push the screen FIFO. */
        g->sxy[i]=(gte_sxy){(s16)r[13+i],(s16)(r[13+i]>>16)};
        g->rgb_fifo[i]=rgb(r[20+i]);
        g->mac[i]=(s32)r[25+i];
        /* IRGB (28), written after IR1..3, expands its 5-bit components. */
        g->ir[i]=(s16)(((r[28]>>(5*i))&31)<<7);
    }
    g->rgbc=rgb(r[6]); g->otz=(u16)r[7]; g->ir0=(s16)r[8];
    for(i=0;i<4;++i) g->sz[i]=(u16)r[16+i];
    g->res1=(s32)r[23]; g->mac0=(s32)r[24];
    g->lzcs=(s32)r[30];
    g->lzcr=q2_count_leading_zeros_u32(g->lzcs<0?~r[30]:r[30]);
    matrix(&g->rot,r+32); matrix(&g->light,r+40); matrix(&g->colour,r+48);
    g->tr=(gte_vec32){(s32)r[37],(s32)r[38],(s32)r[39]};
    g->bk=(gte_vec32){(s32)r[45],(s32)r[46],(s32)r[47]};
    g->fc=(gte_vec32){(s32)r[53],(s32)r[54],(s32)r[55]};
    g->ofx=(s32)r[56];g->ofy=(s32)r[57];g->h=(u16)r[58];
    g->dqa=(s16)r[59];g->dqb=(s32)r[60];
    g->zsf3=(s16)r[61];g->zsf4=(s16)r[62];
    g->flag=r[63]&0x7ffff000u;
    if(g->flag&GTE_FLAG_ERROR_MASK)g->flag|=GTE_FLAG_ERROR;
}
static u32 read_register(const gte_state *g,int r)
{
    int i;
    u32 packed=0;
    if(r==7)return g->otz;
    if(r==8)return (u32)(s32)g->ir0;
    if(r>=9&&r<=11)return (u32)(s32)g->ir[r-9];
    if(r>=12&&r<=15) {
        int n=r==15?2:r-12;
        return (u16)g->sxy[n].x|((u32)(u16)g->sxy[n].y<<16);
    }
    if(r>=16&&r<=19)return g->sz[r-16];
    if(r>=20&&r<=22)return pack_rgb(g->rgb_fifo[r-20]);
    if(r==23)return (u32)g->res1;
    if(r==24)return (u32)g->mac0;
    if(r>=25&&r<=27)return (u32)g->mac[r-25];
    if(r==28||r==29) {
        for(i=0;i<3;++i)packed|=(u32)q2_clamp_s32(g->ir[i]>>7,0,31)<<(i*5);
        return packed;
    }
    if(r==30)return (u32)g->lzcs;
    if(r==31)return (u32)g->lzcr;
    return g->flag;
}
int main(void)
{
    unsigned i,failures=0,count=(unsigned)(sizeof(gte_vectors)/sizeof(gte_vectors[0]));
    for(i=0;i<count;++i) {
        const struct gte_reference *v=&gte_vectors[i];
        gte_state g;
        int j,failed=0,sf=(v->opcode>>19)&1;
        load_registers(&g,v->input);
        switch(v->opcode&63) {
        case 1:gte_rtps(&g,true);break;
        case 6:gte_nclip(&g);break;
        case 18:gte_mvmva(&g,sf,(v->opcode>>17)&3,(v->opcode>>15)&3,
                         (v->opcode>>13)&3,(v->opcode>>10)&1);break;
        case 19:gte_ncds(&g);break;
        case 20:gte_cdp(&g);break;
        case 22:gte_ncdt(&g);break;
        case 27:gte_nccs(&g);break;
        case 28:gte_cc(&g);break;
        case 30:gte_ncs(&g);break;
        case 32:gte_nct(&g);break;
        case 40:gte_sqr(&g,sf);break;
        case 45:gte_avsz3(&g);break;
        case 46:gte_avsz4(&g);break;
        case 48:gte_rtpt(&g);break;
        case 63:gte_ncct(&g);break;
        default:return 2;
        }
        for(j=0;j<26;++j) {
            int reg=j==25?63:j+7;
            u32 actual=read_register(&g,reg);
            if(actual!=v->output[j]) {
                if(failures<12) printf("vector %u opcode %08x r%d: %08x != %08x\n",
                    v->id,v->opcode,reg,actual,v->output[j]);
                failed=1;
            }
        }
        failures+=failed;
    }
    printf("GTE hardware vectors: %u/%u passed (all affected registers and FLAG)\n",count-failures,count);
    return failures?1:0;
}

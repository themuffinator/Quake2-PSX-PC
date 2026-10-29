/* QFRONT +35C8/+36A0: sixteen scrolling text objects and a fixed title. */
#ifndef Q2PSX_CREDITS_H
#define Q2PSX_CREDITS_H

#include "menudraw.h"

#define Q2_CREDITS_MAX 256
#define Q2_CREDITS_SLOTS 16

typedef struct q2_credit_row {
    const char *text;                 /* borrowed from the level module */
    s16 x, y;
} q2_credit_row;

typedef struct q2_credits {
    const q2_credit_row *rows;
    u32 count, next;
    q2_credit_row slot[Q2_CREDITS_SLOTS];
    u8 frame;
    bool active;
    double carry;
} q2_credits;

void q2_credits_start(q2_credits *roll, const q2_credit_row *rows, u32 count);
/* Advance at the disc's video rate, independently of host render frequency. */
bool q2_credits_advance(q2_credits *roll, double dt, int tick_rate);
void q2_credits_build_ot(const q2_credits *roll, psx_ot *ot,
                          const q2_menu_draw_opts *opts, int screen_h);

#endif

/*
 * menumouse.h — what is under the pointer, in the console's own menu space.
 *
 * ---------------------------------------------------------------------------
 * Why this is a module and not four lines in the client
 * ---------------------------------------------------------------------------
 * The console had no pointer, so nothing in the executable answers this
 * question and there is nothing here to transcribe. What there IS, and what
 * this module is bound to, is menudraw.c: every rectangle below is the exact
 * inverse of a rectangle that module emits, so a page that draws its slider
 * somewhere is a page whose slider can be grabbed there. Written inline in the
 * client the two would drift the first time a face metric changed, and the
 * failure would be silent — a control that looks right and cannot be hit.
 *
 * It is also the only part of mouse support that can be tested without a
 * window, which is why it takes coordinates rather than events.
 *
 * ---------------------------------------------------------------------------
 * The space
 * ---------------------------------------------------------------------------
 * `x` and `y` are in the console's 512x248 framebuffer — the same space every
 * item record's x/y is in (menu.h). A host works in window pixels and has two
 * transforms to undo first: the window-to-framebuffer fit, and the origin the
 * 512x248 block is centred at inside a larger buffer. Both belong to the
 * caller, because both are the caller's own choices.
 *
 * ---------------------------------------------------------------------------
 * The rows are hit at their WIDEST
 * ---------------------------------------------------------------------------
 * A toggle draws "LABEL ON OFF" under the cursor and "LABEL ON" — one word,
 * whichever is current — when it is not (0x8001B670). Hit-testing what is
 * currently drawn would make a row's box grow as the pointer entered it: you
 * would aim at OFF, the row would not be under the pointer yet, and the word
 * would only appear once you had already hit something else. So the box is
 * always the selected form's, which is the form the row is in by the time
 * anyone clicks it. Colour codes are not printable and cannot change a width,
 * so this agrees with `q2_menu_item_display` on every other widget exactly.
 */
#ifndef Q2PSX_MENUMOUSE_H
#define Q2PSX_MENUMOUSE_H

#include "menu.h"

/* Which part of a row the pointer is over. The three widget parts exist
 * because a pad reaches them with LEFT and RIGHT and a pointer reaches them
 * by aiming, and a host that could only ever say "this row" would leave the
 * toggles and sliders unusable with the mouse alone. */
typedef enum q2_menu_hit_part {
    Q2_MENU_HIT_NONE = 0,
    Q2_MENU_HIT_LABEL,    /* the row itself — a click activates it          */
    Q2_MENU_HIT_ON,       /* a toggle's ON word                             */
    Q2_MENU_HIT_OFF,      /* a toggle's OFF word                            */
    Q2_MENU_HIT_PREV,     /* a choice's left half — the pad's LEFT          */
    Q2_MENU_HIT_NEXT,     /* its right half — the pad's RIGHT               */
    Q2_MENU_HIT_SLIDER,   /* inside a slider's 133x8 track                  */
    Q2_MENU_HIT_CHAR      /* player-name character; click cycles upward    */
} q2_menu_hit_part;

typedef struct q2_menu_hit {
    int              index;   /* item index, or -1 when nothing is under it */
    q2_menu_hit_part part;
    int              value;   /* Q2_MENU_HIT_SLIDER: the value at that x    */
} q2_menu_hit;

/*
 * The row's rectangle, corners INCLUSIVE, in menu space.
 *
 * Vertically this is the selection bar's own extent rather than the glyph
 * cell's — the band that lights up IS the row as far as anyone aiming at it is
 * concerned, and it is two pixels taller top and bottom (0x8001A7A8).
 * Horizontally it is the text, extended over the track on a slider row.
 *
 * False for an index that is out of range or draws nothing.
 */
bool q2_menu_item_rect(const q2_menu *m, int index,
                       int *x0, int *y0, int *x1, int *y1);

/*
 * What is at (x, y). Fills `out` and returns true only when the point lands on
 * a SELECTABLE item: a grey row and the static text above a page's navigable
 * group are drawn but cannot be aimed at, exactly as they cannot be reached
 * with the d-pad (0x80019CC4).
 */
bool q2_menu_hit_test(const q2_menu *m, int x, int y, q2_menu_hit *out);

/*
 * The value a point along a slider's track names, whether or not the point is
 * on the track at all — it pins to 0 and 127 outside it. False unless `index`
 * really is a slider.
 *
 * A drag needs this rather than a hit test: once the button is down the row
 * belongs to the pointer, and letting the value stop moving because the pointer
 * wandered a few pixels above the track is exactly the behaviour that makes a
 * slider feel broken.
 */
bool q2_menu_slider_at(const q2_menu *m, int index, int x, int *value);

#endif /* Q2PSX_MENUMOUSE_H */

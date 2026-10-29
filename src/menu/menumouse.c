/*
 * menumouse.c — the inverse of menudraw.c.
 *
 * Every constant here has a twin in that file, and the twin is named at each
 * one. Nothing is authored: if a row can be clicked somewhere it is not drawn,
 * this file and that one disagree and one of them is wrong.
 */
#include "menumouse.h"

#include "menufont.h"

#include <stdio.h>
#include <string.h>

/* menudraw.c's slider — 0x8001BBB4 for the box, 0x8001BD28 for the fill. */
#define HIT_BAR_W 133
#define HIT_BAR_H   8

/*
 * The row's text at its WIDEST — see the header. Only the printable WIDTH is
 * wanted, so the colour codes `q2_menu_item_display` decorates with are left
 * off: they are skipped by `q2_menu_text_length` and cannot change one.
 */
static const char *hit_line(const q2_menu *m, int index, char *out, u32 n)
{
    const q2_menu_item *it = &m->page->items[index];
    const char *label = q2_menu_item_text(m, index);
    s16 value = 0;

    if (m->set && it->setting > Q2_SET_NONE && it->setting < Q2_SET_COUNT)
        value = m->set->v[it->setting];

    switch (it->widget) {
    case Q2_WIDGET_TOGGLE:
        /* The selected form, which draws both words (0x8001B670). */
        snprintf(out, n, "%s ON OFF", label);
        break;
    case Q2_WIDGET_CHOICE:
        snprintf(out, n, "%s", q2_menu_pad_style_name(value));
        break;
    default:
        snprintf(out, n, "%s", label);
        break;
    }

    return out;
}

/* menudraw.c skips a record whose label is empty unless it is a choice, and a
 * row that is not drawn cannot be aimed at. */
static bool row_drawn(const q2_menu *m, int index)
{
    return q2_menu_item_text(m, index)[0] != '\0' ||
           m->page->items[index].widget == Q2_WIDGET_CHOICE;
}

/* Where a slider's track starts: the label is centred on the record's x, so
 * the bar begins half its printable width further on (menudraw.c). */
static int slider_x(const q2_menu_item *it, const char *line)
{
    return it->x + q2_menu_font_width(Q2_MENU_FACE_ITEM, line) / 2;
}

bool q2_menu_slider_at(const q2_menu *m, int index, int x, int *value)
{
    const q2_menu_item *it;
    char line[80];
    int v;

    if (!m || !m->page || index < 0 || index >= (int)m->page->count)
        return false;

    it = &m->page->items[index];
    if (it->widget != Q2_WIDGET_SLIDER)
        return false;

    /*
     * 0x8001BD28 draws the fill out to `bar_x + value + 3`, so the value at a
     * point is that read backwards. The clamp is the adjust loop's own
     * (0x8001C1B8), which is also what pins a drag that has run off an end.
     */
    hit_line(m, index, line, (u32)sizeof(line));
    v = x - slider_x(it, line) - 3;

    if (v < 0)
        v = 0;
    if (v > Q2_MENU_SLIDER_MAX)
        v = Q2_MENU_SLIDER_MAX;

    if (value)
        *value = v;
    return true;
}

bool q2_menu_item_rect(const q2_menu *m, int index,
                       int *x0, int *y0, int *x1, int *y1)
{
    const q2_menu_item *it;
    const q2_menu_face *face;
    char line[80];
    int w, cell_h, left, right;

    if (!m || !m->page || index < 0 || index >= (int)m->page->count)
        return false;
    if (!row_drawn(m, index))
        return false;

    it   = &m->page->items[index];
    face = q2_menu_face_get(Q2_MENU_FACE_ITEM);
    cell_h = face ? face->cell_h : 11;

    hit_line(m, index, line, (u32)sizeof(line));
    w = q2_menu_font_width(Q2_MENU_FACE_ITEM, line);

    /* 0x8001AE30: the run is centred on the record's x. */
    left  = it->x - w / 2;
    right = left + w - 1;

    /* A slider's track is part of the row as far as aiming is concerned, and
     * it is the only part of one worth aiming at. */
    if (it->widget == Q2_WIDGET_SLIDER)
        right = slider_x(it, line) + HIT_BAR_W - 1;

    /* An empty run has no width; give it none rather than a backwards box. */
    if (right < left)
        right = left;

    if (x0) *x0 = left;
    if (x1) *x1 = right;

    /*
     * The selection bar's own extent — 0x8001A7A8, which spans
     * y - cell_h/2 - 2 to y - cell_h/2 + cell_h + 1. The band that lights up
     * is the row, so that is what can be hit.
     */
    if (y0) *y0 = q2_menu_item_y(m, index) - cell_h / 2 - 2;
    if (y1) *y1 = q2_menu_item_y(m, index) - cell_h / 2 + cell_h + 1;

    return true;
}

static bool player_hit_test(const q2_menu *m, int x, int y, q2_menu_hit *out)
{
    q2_menu_player_row rows[7];
    const q2_menu_player_panel *v = &m->mp_panel[0];
    int i, ox, oy, count = q2_menu_player_rows(m, 0, rows);
    /* The one host mouse belongs to player one, even over another panel. */
    q2_menu_player_origin(m, 0, &ox, &oy);
    x -= ox; y -= oy;
    for (i = 0; i < count; i++) {
        q2_menu_hit hit = {i, Q2_MENU_HIT_LABEL, 0};
        const q2_menu_face *face = q2_menu_face_get(rows[i].size);
        int len = q2_menu_text_length(rows[i].text);
        int left = 128 - q2_menu_font_width(rows[i].size, rows[i].text) / 2;
        int selectable = v->page == Q2_MP_PLAYER_MAIN ? 2 :
                         v->page == Q2_MP_PLAYER_OPTIONS ? 3 :
                         v->page == Q2_MP_PLAYER_CONTROLLER ? 5 :
                         v->page == Q2_MP_PLAYER_NAME || v->page == Q2_MP_PLAYER_COLOUR ? 1 : 0;
        int col;
        if (!face || i >= selectable || rows[i].disabled ||
            x < left || x >= left + len * face->advance ||
            y < rows[i].y - face->cell_h / 2 - 2 ||
            y > rows[i].y - face->cell_h / 2 + face->cell_h + 1) continue;
        col = (x - left) / face->advance;
        if (v->page == Q2_MP_PLAYER_NAME) {
            hit.index = col;
            hit.part = Q2_MENU_HIT_CHAR;
        } else if (v->page == Q2_MP_PLAYER_COLOUR ||
                   (v->page == Q2_MP_PLAYER_CONTROLLER && (i == 0 || i == 4)))
            hit.part = x < 128 ? Q2_MENU_HIT_PREV : Q2_MENU_HIT_NEXT;
        else if (v->page == Q2_MP_PLAYER_CONTROLLER) {
            if (col >= len - 3) hit.part = Q2_MENU_HIT_OFF;
            else if (col >= len - 6 && col < len - 4) hit.part = Q2_MENU_HIT_ON;
        }
        if (out) *out = hit;
        return true;
    }
    return false;
}

bool q2_menu_hit_test(const q2_menu *m, int x, int y, q2_menu_hit *out)
{
    q2_menu_hit hit;
    int i;

    if (out) {
        out->index = -1;
        out->part  = Q2_MENU_HIT_NONE;
        out->value = 0;
    }

    if (!m || !m->open || !m->page)
        return false;
    if (m->page_id == Q2_PAGE_FRONT_PLAYERS)
        return player_hit_test(m, x, y, out);

    hit.index = -1;
    hit.part  = Q2_MENU_HIT_NONE;
    hit.value = 0;

    for (i = (int)m->page->first; i < (int)m->page->count; i++) {
        const q2_menu_item *it = &m->page->items[i];
        const q2_menu_face *face;
        char line[80];
        int x0, y0, x1, y1, advance, col, len;

        /* Grey rows and disabled ones are drawn and not navigable; the d-pad
         * skips them and so does the pointer. */
        if (!q2_menu_item_selectable(m, i))
            continue;
        if (!q2_menu_item_rect(m, i, &x0, &y0, &x1, &y1))
            continue;
        if (x < x0 || x > x1 || y < y0 || y > y1)
            continue;

        hit.index = i;
        hit.part  = Q2_MENU_HIT_LABEL;

        face    = q2_menu_face_get(Q2_MENU_FACE_ITEM);
        advance = face ? face->advance : Q2_MENU_FACE_ITEM;
        hit_line(m, i, line, (u32)sizeof(line));
        len = q2_menu_text_length(line);

        switch (it->widget) {
        case Q2_WIDGET_SLIDER: {
            int bar_x = slider_x(it, line);

            if (x >= bar_x && x < bar_x + HIT_BAR_W) {
                hit.part = Q2_MENU_HIT_SLIDER;
                q2_menu_slider_at(m, i, x, &hit.value);
            }
            break;
        }

        case Q2_WIDGET_TOGGLE:
            /*
             * The row reads "LABEL ON OFF", so counting back from the end
             * finds the two words wherever the label ends: OFF is the last
             * three columns and ON the two before the space before them.
             */
            col = (advance > 0) ? (x - x0) / advance : 0;
            if (col >= len - 3)
                hit.part = Q2_MENU_HIT_OFF;
            else if (col >= len - 6 && col < len - 4)
                hit.part = Q2_MENU_HIT_ON;
            break;

        case Q2_WIDGET_CHOICE:
            /*
             * A choice has no words to aim at — it draws one value and LEFT
             * and RIGHT step it — so the row itself is the control, split
             * down the record's own centre.
             */
            hit.part = (x < it->x) ? Q2_MENU_HIT_PREV : Q2_MENU_HIT_NEXT;
            break;

        default:
            break;
        }

        break;   /* rows do not overlap; the first that contains it wins */
    }

    if (out)
        *out = hit;

    return hit.index >= 0;
}

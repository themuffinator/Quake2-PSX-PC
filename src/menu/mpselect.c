/* QFRONT's per-player setup, +0x59CC / +0x5D94 / +0x75C8.
 * Each pad owns a state, cursor and eight-character name. Rendering is a
 * reader of that state; it never consumes input or changes configuration. */
#include "menu.h"
#include <stdio.h>
#include <string.h>

void q2_menu_reset_identities(q2_menu *m)
{
    int p;
    if (!m) return;
    for (p = 0; p < 4; p++) {
        snprintf(m->mp_name[p], sizeof(m->mp_name[p]), "PLAYER %d", p + 1);
        m->mp_team[p] = (s16)(p & 1);
        m->mp_colour[p] = (s16)p;
    }
}

void q2_menu_players_begin(q2_menu *m)
{
    int p;
    memset(m->mp_panel, 0, sizeof(m->mp_panel));
    for (p = 0; p < 4; p++) m->mp_panel[p].cursor = 1;
    m->mp_panel_settle = true;
}

bool q2_menu_players_two_teams(const q2_menu *m)
{
    int p;
    unsigned teams = 0;
    if (m->mp_setup.mode != Q2_MENU_MP_TEAM_DEATHMATCH) return true;
    for (p = 0; p < m->mp_setup.players && p < 4; p++)
        teams |= 1u << (m->mp_team[p] & 1);
    return teams == 3;
}

static s16 *setting(q2_menu *m, int p, q2_menu_setting id)
{
    return &m->set->v[q2_menu_player_setting(id, p)];
}

static bool controller_disabled(const q2_menu *m, int p, int row)
{
    int cls = m->set->v[q2_menu_player_setting(Q2_SET_PAD_CLASS, p)];
    bool mouse = m->set->v[q2_menu_player_setting(Q2_SET_USE_MOUSE, p)] != 0;
    if (row == 1) return !m->pad_shockable[p];
    if (row == 2) return cls != 0 && cls != 1;
    if (row == 3) return m->mp_host_mouse ? p != 0 : cls != 1;
    if (row == 4) return !mouse || (m->mp_host_mouse && p != 0);
    return false;
}

void q2_menu_player_origin(const q2_menu *m, int p, int *x, int *y)
{
    /* +0x5B08 uses engine+0x168, the stacked layout (0x80077900).
     * +0x9BB8 moves player two's text to x=384 within its wide viewport.
     * Three/four players use the ordinary quadrant offsets. */
    bool two = m->mp_setup.players == 2;
    if (x) *x = (p & 1) * 256 + (two ? 0 : 1);
    if (y) *y = two ? 1 + p * 120 :
                  p < 2 ? 1 : (m->fb_h == 240 ? 120 : 124);
}

bool q2_menu_player_point_at(q2_menu *m, int index)
{
    q2_menu_player_panel *v = &m->mp_panel[0];
    int count = v->page == Q2_MP_PLAYER_MAIN ? (q2_menu_players_two_teams(m) ? 2 : 1) :
                v->page == Q2_MP_PLAYER_OPTIONS ? 3 :
                v->page == Q2_MP_PLAYER_CONTROLLER ? 5 : 0;
    if (v->page == Q2_MP_PLAYER_NAME) {
        size_t len = strlen(m->mp_name[0]);
        if (index < 0 || index >= 8 || index == v->letter) return false;
        while (len <= (size_t)index) m->mp_name[0][len++] = ' ';
        m->mp_name[0][len] = 0;
        v->letter = (u8)index;
    } else {
        if (index < 0 || index >= count || index == v->cursor ||
            (v->page == Q2_MP_PLAYER_CONTROLLER && controller_disabled(m, 0, index)))
            return false;
        v->cursor = (u8)index;
    }
    m->sound = Q2_MSND_MOVE;
    return true;
}

static void controller_step(q2_menu *m, int p, u16 press)
{
    q2_menu_player_panel *v = &m->mp_panel[p];
    static const q2_menu_setting fields[] = {
        Q2_SET_PAD_STYLE, Q2_SET_VIBRATION, Q2_SET_SWAP_Y,
        Q2_SET_USE_MOUSE, Q2_SET_MOUSE_SPEED
    };
    s16 *value = setting(m, p, fields[v->cursor]);
    int before = *value;
    if (v->cursor == 0) {
        int cls = *setting(m, p, Q2_SET_PAD_CLASS);
        int lo = cls == 0 ? 0 : cls == 1 ? 3 : 6;
        if (press & Q2_PAD_LEFT) --*value;
        if (press & Q2_PAD_RIGHT) ++*value;
        if (*value < lo) *value = (s16)(lo + 2);
        if (*value >= lo + 3) *value = (s16)lo;
    } else if (v->cursor == 4) {
        /* This page's numeric speed changes by one per press, unlike the
         * full-screen controller page's held, two-unit slider (+0x8F5C). */
        if (press & Q2_PAD_LEFT) --*value;
        else if (press & Q2_PAD_RIGHT) ++*value;
        if (*value < 0) *value = 0;
        if (*value > 127) *value = 127;
    } else {
        if (press & Q2_PAD_LEFT) *value = 1;
        if (press & Q2_PAD_RIGHT) *value = 0;
    }
    if (*value != before) m->sound = Q2_MSND_TOGGLE;
}

static void name_step(q2_menu *m, int p, u16 press)
{
    /* +0x6654..+0x6ABC: A..Z, 0..9 and space, with a movable cursor.
     * Right extends a short name; left removes a trailing blank. */
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    q2_menu_player_panel *v = &m->mp_panel[p];
    char *name = m->mp_name[p];
    size_t len = strlen(name);
    int at = v->letter, i;
    if (!len) { name[0] = ' '; name[1] = 0; len = 1; }
    if (at >= (int)len) at = (int)len - 1;
    if (press & (Q2_PAD_UP | Q2_PAD_DOWN)) {
        const char *found = strchr(alphabet, name[at]);
        i = found ? (int)(found - alphabet) : 0;
        i = (i + ((press & Q2_PAD_UP) ? 1 : 36)) % 37;
        name[at] = alphabet[i];
        m->sound = Q2_MSND_SELECT;
    }
    if (press & Q2_PAD_LEFT) {
        if (at == (int)len - 1 && at > 0 && name[at] == ' ')
            name[at] = 0;
        at = at ? at - 1 : (int)len - 1;
        m->sound = Q2_MSND_TOGGLE;
    } else if (press & Q2_PAD_RIGHT) {
        if (++at == (int)len && at < 8) {
            name[at] = ' ';
            name[at + 1] = 0;
        }
        if (at == 8) at = 0;
        m->sound = Q2_MSND_TOGGLE;
    }
    v->letter = (u8)at;
}

int q2_menu_players_step(q2_menu *m, const u16 pad[4], u32 connected)
{
    int p, ready = 0;
    bool settle = m->mp_panel_settle;
    m->mp_panel_settle = false;
    for (p = 0; p < m->mp_setup.players && p < 4; p++) {
        q2_menu_player_panel *v = &m->mp_panel[p];
        u16 press = (u16)(pad[p] & ~v->previous);
        int n = 0, move;
        v->previous = pad[p];
        if (!(connected & (1u << p))) {
            if (v->page != Q2_MP_PLAYER_DISCONNECTED) {
                v->resume_page = v->page;
                v->resume_cursor = v->cursor;
                v->page = Q2_MP_PLAYER_DISCONNECTED;
            }
            continue;
        }
        if (v->page == Q2_MP_PLAYER_DISCONNECTED) {
            v->page = v->resume_page;
            v->cursor = v->resume_cursor;
            press = 0; /* A reinserted controller's held button is no press. */
        }
        if (settle) continue;
        if (v->page == Q2_MP_PLAYER_MAIN) n = q2_menu_players_two_teams(m) ? 2 : 1;
        if (v->page == Q2_MP_PLAYER_OPTIONS) n = 3;
        if (v->page == Q2_MP_PLAYER_CONTROLLER) n = 5;
        if (n) {
            if (v->cursor >= n) v->cursor = 0;
            move = (press & Q2_PAD_UP) ? -1 : (press & Q2_PAD_DOWN) ? 1 : 0;
            if (move) {
                do { v->cursor = (u8)((v->cursor + n + move) % n); }
                while (v->page == Q2_MP_PLAYER_CONTROLLER &&
                       controller_disabled(m, p, v->cursor));
                m->sound = Q2_MSND_MOVE;
            }
            if (v->page == Q2_MP_PLAYER_CONTROLLER &&
                controller_disabled(m, p, v->cursor)) v->cursor = 0;
        }
        if (press & Q2_PAD_TRIANGLE) {
            m->sound = Q2_MSND_BACK;
            switch (v->page) {
            case Q2_MP_PLAYER_MAIN: return -1;
            case Q2_MP_PLAYER_OPTIONS: v->page = Q2_MP_PLAYER_MAIN; v->cursor = 0; break;
            case Q2_MP_PLAYER_READY: v->page = Q2_MP_PLAYER_MAIN; v->cursor = 1; break;
            default:
                v->cursor = v->page == Q2_MP_PLAYER_NAME ? 0 :
                            v->page == Q2_MP_PLAYER_COLOUR ? 1 : 2;
                v->page = Q2_MP_PLAYER_OPTIONS;
                break;
            }
        } else {
            switch (v->page) {
            case Q2_MP_PLAYER_MAIN:
                if (press & Q2_PAD_CROSS) {
                    v->page = v->cursor ? Q2_MP_PLAYER_READY : Q2_MP_PLAYER_OPTIONS;
                    v->cursor = 0;
                    m->sound = Q2_MSND_SELECT;
                }
                break;
            case Q2_MP_PLAYER_OPTIONS:
                if (press & Q2_PAD_CROSS) {
                    v->page = (u8)(Q2_MP_PLAYER_NAME + v->cursor);
                    v->cursor = 0;
                    m->sound = Q2_MSND_SELECT;
                }
                break;
            case Q2_MP_PLAYER_NAME: name_step(m, p, press); break;
            case Q2_MP_PLAYER_COLOUR: {
                bool team = m->mp_setup.mode == Q2_MENU_MP_TEAM_DEATHMATCH;
                s16 *colour = team ? &m->mp_team[p] : &m->mp_colour[p];
                int count = team ? 2 : 4;
                move = (press & Q2_PAD_LEFT) ? -1 : (press & Q2_PAD_RIGHT) ? 1 : 0;
                if (move) {
                    *colour = (s16)((*colour + count + move) % count);
                    m->sound = Q2_MSND_TOGGLE;
                }
                break;
            }
            case Q2_MP_PLAYER_CONTROLLER: controller_step(m, p, press); break;
            default: break;
            }
        }
        if (v->page == Q2_MP_PLAYER_READY) ready++;
    }
    return ready == m->mp_setup.players && q2_menu_players_two_teams(m) ? 1 : 0;
}

static void row(q2_menu_player_row *r, int y, int size, bool selected,
                bool disabled, const char *text)
{
    memset(r, 0, sizeof(*r));
    r->y = (s16)y; r->size = (s16)size;
    r->selected = selected; r->disabled = disabled;
    snprintf(r->text, sizeof(r->text), "%s%s", disabled ? "g" : "", text);
}

static const char *display_name(const q2_menu *m, int p, char text[64])
{
    size_t len;
    snprintf(text, 64, "%.8s", m->mp_name[p]);
    len = strlen(text);
    while (len > 1 && text[len - 1] == ' ') text[--len] = 0;
    return text;
}

int q2_menu_player_rows(const q2_menu *m, int p, q2_menu_player_row out[7])
{
    static const char *const colours[] = { "BLUE", "RED", "PURPLE", "GREEN" };
    const q2_menu_player_panel *v = &m->mp_panel[p];
    bool team = m->mp_setup.mode == Q2_MENU_MP_TEAM_DEATHMATCH;
    const char *label = team ? "TEAM" : "COLOR";
    char text[64];
    int i;
    switch (v->page) {
    case Q2_MP_PLAYER_MAIN:
        row(&out[0], 62, 16, v->cursor == 0, false, "OPTIONS");
        row(&out[1], 88, 16, v->cursor == 1 && q2_menu_players_two_teams(m),
            !q2_menu_players_two_teams(m), "START");
        row(&out[2], 36, 16, false, false, display_name(m, p, text));
        if (q2_menu_players_two_teams(m)) return 3;
        row(&out[3], 100, 8, false, false, "THIS MODE REQUIRES 2 TEAMS"); return 4;
    case Q2_MP_PLAYER_OPTIONS:
        row(&out[0], 36, 16, v->cursor == 0, false, "NAME");
        row(&out[1], 62, 16, v->cursor == 1, false, label);
        row(&out[2], 88, 16, v->cursor == 2, false, "CONTROLLER"); return 3;
    case Q2_MP_PLAYER_NAME:
        for (i = 0; i < 8; i++) {
            text[2*i] = v->letter == i ? 'u' : 'b';
            text[2*i+1] = (size_t)i < strlen(m->mp_name[p]) ? m->mp_name[p][i] : ' ';
        }
        text[16] = 0;
        row(&out[0], 75, 16, true, false, text);
        row(&out[1], 49, 16, false, false, "NAME");
        row(&out[2], 90, 8, false, false, "USE DIRECTIONAL BUTTONS");
        row(&out[3], 100, 8, false, false, "TO CHANGE NAME"); return 4;
    case Q2_MP_PLAYER_COLOUR:
        row(&out[0], 75, 16, true, false, colours[(team ? m->mp_team[p] : m->mp_colour[p]) & 3]);
        row(&out[1], 49, 16, false, false, label);
        snprintf(text, sizeof(text), "LEFT AND RIGHT CHANGES %sS", label);
        row(&out[2], 100, 8, false, false, text); return 3;
    case Q2_MP_PLAYER_CONTROLLER: {
        static const q2_menu_setting fields[] = { Q2_SET_PAD_STYLE,
            Q2_SET_VIBRATION, Q2_SET_SWAP_Y, Q2_SET_USE_MOUSE, Q2_SET_MOUSE_SPEED };
        static const char *const labels[] = { "", "VIBRATE", "SWAP Y", "MOUSE" };
        for (i = 0; i < 5; i++) {
            int value = m->set->v[q2_menu_player_setting(fields[i], p)];
            if (i == 0) snprintf(text, sizeof(text), "%s", q2_menu_pad_style_name(value));
            else if (i == 4) snprintf(text, sizeof(text), "MOUSE SPEED %d", value * 99 / 127);
            else if (controller_disabled(m, p, i))
                snprintf(text, sizeof(text), "%s", labels[i]);
            else snprintf(text, sizeof(text), "%s %sON %sOFF", labels[i],
                          value ? "b" : "d", value ? "d" : "b");
            row(&out[i], 28 + 17*i, 16, v->cursor == i,
                controller_disabled(m, p, i), text);
        }
        return 5;
    }
    case Q2_MP_PLAYER_READY:
        row(&out[0], 49, 16, false, false, display_name(m, p, text));
        row(&out[1], 75, 16, false, false, "READY!"); return 2;
    default:
        row(&out[0], 36, 16, false, false, "PLEASE RE-INSERT");
        snprintf(text, sizeof(text), "CONTROLLER %d", p + 1);
        row(&out[1], 62, 16, false, false, text); return 2;
    }
}

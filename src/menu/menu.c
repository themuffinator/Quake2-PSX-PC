/*
 * menu.c — the menu engine.
 *
 * This is the behaviour half of the reconstruction: the navigation rules, the
 * widget semantics and the page transitions, all read out of the original's
 * code rather than designed here. The interesting ones, with where they came
 * from:
 *
 *   0x80019BFC  "just pressed" is `cur & ~prev`, per player
 *   0x80019CC4  an item is skipped when its disable field is set or its text
 *               (either buffer) starts with 'g'
 *   0x80019E8C  when there is nothing below the cursor it wraps to the first
 *               selectable item, and vice versa
 *   0x80019F60  UP is 0x10 and DOWN is 0x40, taken from the just-pressed mask
 *   0x8001A0D8  an item whose bit 13 is set fires on button *release*; the
 *               rest fire on press
 *   0x8001B720  a toggle takes LEFT to mean ON and RIGHT to mean OFF, because
 *               the row reads "LABEL  ON  OFF" and you are moving along it
 *   0x8001C018  a slider moves two units per frame while the direction is
 *               *held*, and clamps to 0..127
 *
 * What is deliberately not reproduced is the object list: the original builds
 * one array of 92-byte drawables per page and derives everything from it,
 * which a port does not need because it can walk the page table directly. The
 * two places that mattered — a hook overwriting an item's text, and a hook
 * disabling one — are kept as explicit per-item overrides.
 */
#include "menu.h"

#include "worldscale.h"   /* Q2_DT_NOMINAL: the arm countdown's fallback step */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Text */

bool q2_menu_is_code(char c)
{
    return c == Q2_MENU_CODE_BRIGHT || c == Q2_MENU_CODE_DIM ||
           c == Q2_MENU_CODE_GREY   || c == Q2_MENU_CODE_UNDER;
}

int q2_menu_text_length(const char *s)
{
    int n = 0;

    if (!s)
        return 0;
    for (; *s; s++)
        if (!q2_menu_is_code(*s))
            n++;
    return n;
}

int q2_menu_title_y(int screen_h)
{
    return (screen_h - 188) / 2 + 10;
}

const char *q2_menu_sound_name(q2_menu_sound s)
{
    switch (s) {
    case Q2_MSND_MOVE:   return "msc_menu2";
    case Q2_MSND_SELECT: return "msc_menu1";
    case Q2_MSND_BACK:   return "msc_menu3";
    case Q2_MSND_TOGGLE: return "itm_pkup";
    case Q2_MSND_SLIDE:  return "msc_comp_up";
    default:             return "";
    }
}

/* ------------------------------------------------------------------------- */
/* Settings and their defaults */

void q2_menu_reset_video(q2_menu_settings *s)          /* 0x8001FA18 */
{
    if (!s) return;
    s->v[Q2_SET_HORIZONTAL_SPLIT] = 1;
    s->v[Q2_SET_SCREEN_X]         = 0;
    s->v[Q2_SET_SCREEN_Y]         = 24;
}

void q2_menu_reset_sound(q2_menu_settings *s)          /* 0x8001FA50 */
{
    if (!s) return;
    s->v[Q2_SET_STEREO] = 1;
    s->v[Q2_SET_MUSIC]  = 48;
    s->v[Q2_SET_SFX]    = 96;
}

void q2_menu_reset_player(q2_menu_settings *s)         /* 0x8001BDA8 */
{
    if (!s) return;
    s->v[Q2_SET_CROSSHAIR]   = 1;
    s->v[Q2_SET_AUTOCENTRE]  = 1;
    /* Per player in the original: style 6 (STANDARD A) out of the standard
     * class, and a mouse speed of 64. */
    s->v[Q2_SET_PAD_CLASS]   = 2;
    s->v[Q2_SET_PAD_STYLE]   = 6;
    s->v[Q2_SET_MOUSE_SPEED] = 64;
    s->v[Q2_SET_VIBRATION]   = 0;
    s->v[Q2_SET_SWAP_Y]      = 0;
    s->v[Q2_SET_USE_MOUSE]   = 0;
}

void q2_menu_reset_variables(q2_menu_settings *s)      /* 0x8002048C */
{
    if (!s) return;
    s->v[Q2_SET_BLAST_FORCE]    = 64;
    s->v[Q2_SET_GRAVITY]        = 64;
    s->v[Q2_SET_GAME_SPEED]     = 64;
    s->v[Q2_SET_WEAPON_STAY]    = 0;
    s->v[Q2_SET_INFINITE_AMMO]  = 0;
    s->v[Q2_SET_ALL_WEAPONS]    = 0;
    s->v[Q2_SET_FALLING_DAMAGE] = 1;
    s->v[Q2_SET_ONE_SHOT_KILL]  = 0;
}

void q2_menu_settings_defaults(q2_menu_settings *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    q2_menu_reset_video(s);
    q2_menu_reset_sound(s);
    q2_menu_reset_player(s);
    q2_menu_reset_variables(s);
}

/* 0x8001C698: what the GAME VARIABLES page is for. */
void q2_menu_apply_variables(const q2_menu_settings *s, bool enabled,
                             s32 base_tick_rate, q2_menu_rules *out)
{
    if (!out)
        return;

    if (!s || !enabled) {
        /* 0x8001C7E4 — the "off" path writes the same constants. */
        out->gravity   = 32;
        out->tick_rate = base_tick_rate;
        out->cheats    = 0;
        return;
    }

    out->gravity   = ((s32)s->v[Q2_SET_GRAVITY] + 64) >> 2;
    out->tick_rate = (base_tick_rate * ((s32)s->v[Q2_SET_GAME_SPEED] + 64)) >> 7;

    out->cheats = 0;
    if (!s->v[Q2_SET_FALLING_DAMAGE]) out->cheats |= Q2_CHEAT_NO_FALL_DAMAGE;
    if (s->v[Q2_SET_INFINITE_AMMO])   out->cheats |= Q2_CHEAT_INFINITE_AMMO;
    if (s->v[Q2_SET_ALL_WEAPONS])     out->cheats |= Q2_CHEAT_ALL_WEAPONS;
    if (s->v[Q2_SET_ONE_SHOT_KILL])   out->cheats |= Q2_CHEAT_ONE_SHOT_KILL;
}

/* ------------------------------------------------------------------------- */
/* Items */

const char *q2_menu_item_text(const q2_menu *m, int index)
{
    if (!m || !m->page || index < 0 || index >= (int)m->page->count)
        return "";
    if (index < Q2_MENU_MAX_ITEMS && m->text[index][0])
        return m->text[index];
    return m->page->items[index].label ? m->page->items[index].label : "";
}

/*
 * 0x8001B670 rewrites a toggle's text every frame as
 *
 *     selected:   sprintf(buf, "b%s %sON %sOFF", label, on ? "b":"d",
 *                                                       on ? "d":"b")
 *     otherwise:  sprintf(buf, "%s %s", label, on ? "ON" : "OFF")
 *
 * and 0x8001A75C copies the plain label back afterwards, which is why the
 * decoration has to be recomposed rather than stored. Doing it here rather
 * than in the renderer keeps the offline tools able to print exactly what the
 * screen shows.
 */
const char *q2_menu_item_display(const q2_menu *m, int index,
                                 char *out, u32 out_size)
{
    const q2_menu_item *it;
    const char *label;
    bool selected;
    s16 value = 0;

    if (!out || out_size == 0)
        return "";
    out[0] = '\0';

    if (!m || !m->page || index < 0 || index >= (int)m->page->count)
        return out;

    it       = &m->page->items[index];
    label    = q2_menu_item_text(m, index);
    selected = (index == m->cursor) && q2_menu_item_selectable(m, index);

    if (m->set && it->setting > Q2_SET_NONE && it->setting < Q2_SET_COUNT)
        value = m->set->v[it->setting];

    switch (it->widget) {
    case Q2_WIDGET_TOGGLE:
        if (selected)
            snprintf(out, out_size, "b%s %sON %sOFF", label,
                     value ? "b" : "d", value ? "d" : "b");
        else
            snprintf(out, out_size, "%s %s", label, value ? "ON" : "OFF");
        break;

    case Q2_WIDGET_CHOICE:
        snprintf(out, out_size, "%s%s", selected ? "b" : "",
                 q2_menu_pad_style_name(value));
        break;

    default:
        snprintf(out, out_size, "%s%s", selected ? "b" : "", label);
        break;
    }

    return out;
}

bool q2_menu_item_selectable(const q2_menu *m, int index)
{
    const char *t;

    if (!m || !m->page)
        return false;
    if (index < (int)m->page->first || index >= (int)m->page->count)
        return false;
    if (index < Q2_MENU_MAX_ITEMS && m->disabled[index])
        return false;

    /* 0x80019CD4: the object's *displayed* text and its stored label are both
     * tested, so a grey prefix on either takes the item out. */
    t = q2_menu_item_text(m, index);
    if (t[0] == Q2_MENU_CODE_GREY)
        return false;
    if (m->page->items[index].label &&
        m->page->items[index].label[0] == Q2_MENU_CODE_GREY)
        return false;

    return true;
}

static int first_selectable(const q2_menu *m)
{
    int i;

    if (!m->page)
        return 0;
    for (i = (int)m->page->first; i < (int)m->page->count; i++)
        if (q2_menu_item_selectable(m, i))
            return i;
    return (int)m->page->first;
}

static int last_selectable(const q2_menu *m)
{
    int i, found = (int)m->page->first;

    for (i = (int)m->page->first; i < (int)m->page->count; i++)
        if (q2_menu_item_selectable(m, i))
            found = i;
    return found;
}

static bool any_selectable(const q2_menu *m)
{
    int i;

    if (!m->page)
        return false;
    for (i = (int)m->page->first; i < (int)m->page->count; i++)
        if (q2_menu_item_selectable(m, i))
            return true;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Page entry */

static void page_entered(q2_menu *m);
static void front_setup_refresh(q2_menu *m);

static const q2_menu_page *resolve(const q2_menu *m, int id)
{
    if (id == Q2_PAGE_VARIABLES)
        return q2_menu_variables_page(m->cheat_level);
    if (id == Q2_PAGE_VIDEO)
        return q2_menu_video_page(m->multiplayer);
    if (id == Q2_PAGE_FRONT_DMSETUP)
        return q2_menu_front_setup_page(m->mp_setup.mode);
    if (id == Q2_PAGE_FRONT_VARIABLES)
        return q2_menu_front_variables_page(m->cheat_level);
    return q2_menu_page_find(id);
}

void q2_menu_goto(q2_menu *m, int page_id)
{
    const q2_menu_page *p;

    if (!m)
        return;

    p = resolve(m, page_id);
    if (!p)
        return;

    /* 0x8001A3B0: the outgoing page's cursor is saved before the switch. */
    if (m->page_id >= 0 && m->page_id < (int)sizeof(m->cursor_save))
        m->cursor_save[m->page_id] = (u8)m->cursor;

    m->prev_page_id = m->page_id;
    m->page_id      = page_id;
    m->page         = p;

    memset(m->text, 0, sizeof(m->text));
    memset(m->disabled, 0, sizeof(m->disabled));
    m->status[0]    = '\0';
    m->arm_ticks    = 0;
    m->slide_repeat = 0;

    page_entered(m);

    /* Restore where the cursor was, then pull it onto something selectable. */
    m->cursor = (page_id < (int)sizeof(m->cursor_save))
                    ? (int)m->cursor_save[page_id] : 0;
    if (m->cursor < (int)p->first || m->cursor >= (int)p->count ||
        !q2_menu_item_selectable(m, m->cursor))
        m->cursor = first_selectable(m);
}

static void push(q2_menu *m, int page_id)
{
    if (m->depth < (int)sizeof(m->stack))
        m->stack[m->depth++] = (u8)m->page_id;
    q2_menu_goto(m, page_id);
}

static void pop(q2_menu *m)
{
    int target;

    if (m->depth <= 0) {
        /* The root pause page has no parent: TRIANGLE there does nothing,
         * which is what a null back handler means (0x8001D824). */
        return;
    }
    target = (int)m->stack[--m->depth];
    q2_menu_goto(m, target);
}

/* The page an action opens, or 0 when it does something else. */
static int action_page(int action)
{
    switch (action) {
    case Q2_ACT_PAGE_VIDEO:      return Q2_PAGE_VIDEO;
    case Q2_ACT_PAGE_SOUND:      return Q2_PAGE_SOUND;
    case Q2_ACT_PAGE_VARIABLES:  return Q2_PAGE_VARIABLES;
    case Q2_ACT_PAGE_CONTROLLER: return Q2_PAGE_CONTROLLER;
    case Q2_ACT_PAGE_PLAYER:     return Q2_PAGE_PLAYER;
    case Q2_ACT_PAGE_OPTIONS:    return Q2_PAGE_OPTIONS;
    case Q2_ACT_PAGE_POSITION:   return Q2_PAGE_SCREEN_POSITION;
    default:                     return 0;
    }
}

/*
 * TRIANGLE.
 *
 * The original has no stack: a page's back handler is the *function of the
 * parent page*, so going back is re-entering it. Some pages name their parent
 * that way (PLAYER's handler at 0x800201C4 is the OPTIONS page's own), and
 * some go to whichever page called them (0x800200E8 patches the QUIT
 * confirmation's NO to point at the caller). Both are "go back", so both
 * unwind rather than descend — a stack that grew on a back would never come
 * out again.
 */
static void go_back(q2_menu *m)
{
    int act = (int)m->page->back;
    int page;

    if (act == Q2_ACT_NONE)
        return;

    if (m->depth > 0)
        m->depth--;

    page = action_page(act);
    if (page)
        q2_menu_goto(m, page);
    else
        q2_menu_goto(m, (int)m->stack[m->depth]);
}

/*
 * Page entry hooks — the per-page work the original does after installing the
 * item table.
 */
static void page_entered(q2_menu *m)
{
    switch (m->page_id) {
    case Q2_PAGE_DEATH:
        /* 0x8001D774: the middle line is written with the resupply count and
         * greyed out when there are none, which also stops the cursor landing
         * on it. The whole page is inert until the countdown expires. */
        if (m->resupplies <= 0)
            snprintf(m->text[1], Q2_MENU_TEXT_MAX,
                     "gRESUPPLY AND RESTART (0 LEFT)");
        else
            snprintf(m->text[1], Q2_MENU_TEXT_MAX,
                     "RESUPPLY AND RESTART (%d LEFT)", m->resupplies);
        m->arm_ticks = 600;   /* 0x800205B0 */
        break;

    case Q2_PAGE_CONTROLLER: {
        /* 0x8001CA28 / 0x8001CAA8: the two items that need analogue sticks
         * grey themselves out when the pad has none. */
        bool sticks = (m->set && m->set->v[Q2_SET_PAD_CLASS] == 1);
        if (!sticks) {
            m->disabled[2] = 1;
            m->disabled[3] = 1;
        }
        break;
    }

    case Q2_PAGE_FRONT_DMSETUP:
        front_setup_refresh(m);
        break;

    case Q2_PAGE_RESTARTING:
        m->request = Q2_MREQ_RESTART;
        break;
    case Q2_PAGE_RESUPPLYING:
        m->request = Q2_MREQ_RESUPPLY;
        break;
    case Q2_PAGE_QUITTING:
        m->request = Q2_MREQ_QUIT;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */

void q2_menu_init(q2_menu *m, q2_menu_settings *settings, int screen_h)
{
    if (!m)
        return;

    memset(m, 0, sizeof(*m));
    m->set      = settings;
    m->screen_h = screen_h > 0 ? screen_h : Q2_MENU_SCREEN_H;
    m->page_id  = Q2_PAGE_NONE;
    m->controller_count       = Q2_MENU_MP_MAX_PLAYERS;
    m->mp_setup.mode          = Q2_MENU_MP_DEATHMATCH;
    m->mp_setup.players       = 2;
    m->mp_setup.arena         = 0;
    m->mp_setup.time_option   = Q2_MENU_MP_TIME_DEFAULT;
    m->mp_setup.frag_option   = Q2_MENU_MP_FRAG_DEFAULT;
    m->mp_setup.round_option  = Q2_MENU_MP_ROUND_DEFAULT;
}

void q2_menu_set_multiplayer(q2_menu *m, bool on)  { if (m) m->multiplayer = on; }
void q2_menu_set_controller_count(q2_menu *m, int count)
{
    if (!m)
        return;
    if (count < 2) count = 2;
    if (count > Q2_MENU_MP_MAX_PLAYERS) count = Q2_MENU_MP_MAX_PLAYERS;
    m->controller_count = count;
    if (m->mp_setup.players > count)
        m->mp_setup.players = (s16)count;
}
void q2_menu_set_cheat_level(q2_menu *m, int level)
{
    if (!m) return;
    m->cheat_level = level < 0 ? 0 : (level > 3 ? 3 : level);
}
void q2_menu_set_resupplies(q2_menu *m, int n)     { if (m) m->resupplies = n; }

void q2_menu_set_stats(q2_menu *m, int kills, int total_kills,
                       int secrets, int total_secrets)
{
    if (!m)
        return;
    /* "KILLS %d/%d    %d/%d SECRETS" — 0x800AB30C. */
    snprintf(m->status, sizeof(m->status), "KILLS %d/%d    %d/%d SECRETS",
             kills, total_kills, secrets, total_secrets);
}

void q2_menu_open(q2_menu *m)
{
    if (!m || m->open)
        return;

    m->open  = true;
    m->depth = 0;
    /* 0x8001D5C8 picks the page; both are titled PAUSED. */
    m->page_id = Q2_PAGE_NONE;
    q2_menu_goto(m, m->multiplayer ? Q2_PAGE_PAUSE_MP : Q2_PAGE_PAUSE_SP);
    /* Latch one frame before acting, so the button that opened the menu
     * cannot also select the item under the cursor. */
    m->settle = 1;
}

void q2_menu_close(q2_menu *m)
{
    if (!m)
        return;
    m->open    = false;
    m->page    = NULL;
    m->page_id = Q2_PAGE_NONE;
    m->depth   = 0;
}

q2_menu_sound q2_menu_take_sound(q2_menu *m)
{
    q2_menu_sound s;

    if (!m)
        return Q2_MSND_NONE;
    s = m->sound;
    m->sound = Q2_MSND_NONE;
    return s;
}

q2_menu_request q2_menu_take_request(q2_menu *m)
{
    q2_menu_request r;

    if (!m)
        return Q2_MREQ_NONE;
    r = m->request;
    m->request = Q2_MREQ_NONE;
    return r;
}

/* ------------------------------------------------------------------------- */
/* Actions */

static void run_action(q2_menu *m, int action)
{
    switch (action) {
    case Q2_ACT_NONE:
        break;

    case Q2_ACT_RESUME:
        m->request = Q2_MREQ_RESUME;
        q2_menu_close(m);
        break;

    case Q2_ACT_MISSION:
        m->request = Q2_MREQ_MISSION;
        q2_menu_close(m);
        break;

    case Q2_ACT_PAGE_VIDEO:      push(m, Q2_PAGE_VIDEO);            break;
    case Q2_ACT_PAGE_SOUND:      push(m, Q2_PAGE_SOUND);            break;
    case Q2_ACT_PAGE_VARIABLES:  push(m, Q2_PAGE_VARIABLES);        break;
    case Q2_ACT_PAGE_CONTROLLER: push(m, Q2_PAGE_CONTROLLER);       break;
    case Q2_ACT_PAGE_PLAYER:     push(m, Q2_PAGE_PLAYER);           break;
    case Q2_ACT_PAGE_OPTIONS:    push(m, Q2_PAGE_OPTIONS);          break;
    case Q2_ACT_PAGE_POSITION:   push(m, Q2_PAGE_SCREEN_POSITION);  break;

    /* The front end's two sub-pages. They push like any other, which is what
     * lets TRIANGLE walk back to the title without a special case. */
    case Q2_ACT_PAGE_FRONT_START:   push(m, Q2_PAGE_FRONT_START);   break;
    case Q2_ACT_PAGE_FRONT_OPTIONS: push(m, Q2_PAGE_FRONT_OPTIONS); break;
    case Q2_ACT_PAGE_FRONT_NEWLOAD: push(m, Q2_PAGE_FRONT_NEWLOAD); break;
    case Q2_ACT_PAGE_FRONT_SKILL:   push(m, Q2_PAGE_FRONT_SKILL);   break;

    /*
     * And its three leaves. Each raises a request and closes, exactly as
     * RESUME and MISSION do: the menu does not know how to start a game, and
     * the caller does not need to know which row was on.
     */
    /*
     * SINGLE PLAYER opens NEW GAME / LOAD GAME rather than starting anything —
     * the console's flow is START -> SINGLE PLAYER -> NEW GAME -> a difficulty,
     * and only the difficulty begins the game.
     */
    case Q2_ACT_NEW_GAME:
        push(m, Q2_PAGE_FRONT_NEWLOAD);
        break;

    /* The three difficulties are one request with the skill attached, because
     * the only thing that differs between them is a number the AI reads. */
    case Q2_ACT_SKILL_EASY:
    case Q2_ACT_SKILL_MEDIUM:
    case Q2_ACT_SKILL_HARD:
        m->skill = (action == Q2_ACT_SKILL_EASY)   ? 0
                 : (action == Q2_ACT_SKILL_MEDIUM) ? 1 : 2;
        m->request = Q2_MREQ_NEW_GAME;
        q2_menu_close(m);
        break;

    /*
     * The rest of the front end's leaves. Each is a page the port has not built
     * yet, and each closes rather than pretending to be one — which is visible,
     * where a page that silently did nothing would not be.
     */
    /* Picking a mode opens the setup, which is the page the capture shows
     * next: a player count, a map, the two limits, the variables and PROCEED. */
    case Q2_ACT_DM_MODE:
        m->mp_setup.mode = (m->cursor == 1) ? Q2_MENU_MP_TEAM_DEATHMATCH
                         : (m->cursor == 2) ? Q2_MENU_MP_VERSUS
                                            : Q2_MENU_MP_DEATHMATCH;
        push(m, Q2_PAGE_FRONT_DMSETUP);
        break;

    case Q2_ACT_LOAD_GAME:
        m->request = Q2_MREQ_LOAD_GAME;
        q2_menu_close(m);
        break;
    case Q2_ACT_MP_SETTINGS:
        m->request = (m->cursor == 4) ? Q2_MREQ_MP_SAVE_SETTINGS
                                      : Q2_MREQ_MP_LOAD_SETTINGS;
        q2_menu_close(m);
        break;
    case Q2_ACT_GAME_VARIABLES:
        push(m, Q2_PAGE_FRONT_VARIABLES);
        break;
    case Q2_ACT_PROCEED:
        m->request = Q2_MREQ_MP_PROCEED;
        q2_menu_close(m);
        break;
    case Q2_ACT_MULTIPLAYER:
        push(m, Q2_PAGE_FRONT_MULTI);
        break;
    case Q2_ACT_CREDITS:
        m->request = Q2_MREQ_CREDITS;
        q2_menu_close(m);
        break;

    case Q2_ACT_ASK_RESTART:     push(m, Q2_PAGE_RESTART_CONFIRM);  break;
    case Q2_ACT_ASK_QUIT:        push(m, Q2_PAGE_QUIT_CONFIRM);     break;
    case Q2_ACT_ASK_RESUPPLY:    push(m, Q2_PAGE_RESUPPLY_CONFIRM); break;

    /* The terminal pages raise their request on entry, so the transition and
     * the side effect stay one thing. */
    case Q2_ACT_DO_RESTART:      q2_menu_goto(m, Q2_PAGE_RESTARTING);  break;
    case Q2_ACT_DO_QUIT:         q2_menu_goto(m, Q2_PAGE_QUITTING);    break;
    case Q2_ACT_DO_RESUPPLY:     q2_menu_goto(m, Q2_PAGE_RESUPPLYING); break;

    case Q2_ACT_BACK:            pop(m);                            break;

    case Q2_ACT_RESET_VIDEO:     q2_menu_reset_video(m->set);       break;
    case Q2_ACT_RESET_SOUND:     q2_menu_reset_sound(m->set);       break;
    case Q2_ACT_RESET_PLAYER:    q2_menu_reset_player(m->set);      break;
    case Q2_ACT_RESET_VARIABLES: q2_menu_reset_variables(m->set);   break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Widgets */

static s16 *binding(q2_menu *m, int index)
{
    int id;

    if (!m->set || !m->page || index < 0 || index >= (int)m->page->count)
        return NULL;
    id = m->page->items[index].setting;
    if (id <= Q2_SET_NONE || id >= Q2_SET_COUNT)
        return NULL;
    return &m->set->v[id];
}

/* The style list the connected controller class offers — 0x8001C8A8. */
static void choice_range(const q2_menu *m, int *lo, int *hi)
{
    int cls = (m->set ? m->set->v[Q2_SET_PAD_CLASS] : 2);

    switch (cls) {
    case 0:  *lo = 0; *hi = 3; break;   /* a mouse            */
    case 1:  *lo = 3; *hi = 6; break;   /* an analogue pad    */
    default: *lo = 6; *hi = 9; break;   /* everything else    */
    }
}

/* 0x8001B670: toggles react to the *just pressed* mask. */
static void update_toggle(q2_menu *m, int index)
{
    s16 *v = binding(m, index);

    if (!v)
        return;

    if (m->pad_new & Q2_PAD_LEFT) {
        if (*v != 1)
            m->sound = Q2_MSND_TOGGLE;
        *v = 1;
    }
    if (m->pad_new & Q2_PAD_RIGHT) {
        if (*v != 0)
            m->sound = Q2_MSND_TOGGLE;
        *v = 0;
    }
}

/* 0x8001BF1C: sliders react to the *held* mask and move two units a frame. */
static void update_slider(q2_menu *m, int index)
{
    s16 *v = binding(m, index);
    bool moved = false;

    if (!v)
        return;

    /* The repeat gate is reset for every slider but the sound-effects one,
     * which is why moving that slider is the only one that clicks. */
    if (m->page->items[index].setting != Q2_SET_SFX)
        m->slide_repeat = 0;

    if (m->pad_cur & Q2_PAD_LEFT)  { *v -= Q2_MENU_SLIDER_STEP; moved = true; }
    if (m->pad_cur & Q2_PAD_RIGHT) { *v += Q2_MENU_SLIDER_STEP; moved = true; }

    if (*v < 0)
        *v = 0;
    if (*v >= Q2_MENU_SLIDER_MAX + 1)
        *v = Q2_MENU_SLIDER_MAX;

    if (moved) {
        if (m->slide_repeat < 0) {
            m->slide_repeat = 8;
            m->sound = Q2_MSND_SLIDE;
        }
        m->slide_repeat--;
    }
}

/* 0x8001C944: the controller style cycles, wrapping at both ends. */
static void update_choice(q2_menu *m, int index)
{
    s16 *v = binding(m, index);
    int lo, hi;

    if (!v)
        return;

    choice_range(m, &lo, &hi);

    if (m->pad_new & Q2_PAD_LEFT)  { (*v)--; m->sound = Q2_MSND_TOGGLE; }
    if (m->pad_new & Q2_PAD_RIGHT) { (*v)++; m->sound = Q2_MSND_TOGGLE; }

    if (*v < lo)  *v = (s16)(hi - 1);
    if (*v >= hi) *v = (s16)lo;
}

/* ------------------------------------------------------------------------- */
/* QFRONT's match-setup page hook — module+0x50D0.                           */

static const s16 k_front_time_options[Q2_MENU_MP_TIME_OPTIONS] = {
    1, 2, 3, 4, 5, 10, 15, 20, 25, 30, 60, -1
};
static const s16 k_front_frag_options[Q2_MENU_MP_FRAG_OPTIONS] = {
    1, 2, 3, 4, 5, 10, 15, 20, -1
};
static const s16 k_front_round_options[Q2_MENU_MP_ROUND_OPTIONS] = {
    1, 2, 3, 5, 10
};

static void front_setup_refresh(q2_menu *m)
{
    int v;

    if (!m || m->page_id != Q2_PAGE_FRONT_DMSETUP)
        return;

    snprintf(m->text[0], Q2_MENU_TEXT_MAX, "%d PLAYERS",
             (int)m->mp_setup.players);
    snprintf(m->text[1], Q2_MENU_TEXT_MAX, "%s",
             q2_menu_mp_arena_name(m->mp_setup.arena));

    if (m->mp_setup.mode == Q2_MENU_MP_VERSUS) {
        v = k_front_round_options[m->mp_setup.round_option];
        snprintf(m->text[2], Q2_MENU_TEXT_MAX, "ROUNDS %2d", v);
    } else {
        v = k_front_time_options[m->mp_setup.time_option];
        if (v < 0)
            snprintf(m->text[2], Q2_MENU_TEXT_MAX, "TIME LIMIT  i");
        else
            snprintf(m->text[2], Q2_MENU_TEXT_MAX, "TIME LIMIT %2d", v);

        v = k_front_frag_options[m->mp_setup.frag_option];
        if (v < 0)
            snprintf(m->text[3], Q2_MENU_TEXT_MAX, "FRAG LIMIT  i");
        else
            snprintf(m->text[3], Q2_MENU_TEXT_MAX, "FRAG LIMIT %2d", v);
    }
}

static int wrap_option(int value, int count)
{
    if (value < 0) return count - 1;
    if (value >= count) return 0;
    return value;
}

static void update_front_setup(q2_menu *m)
{
    int before, after;
    int step = ((m->pad_new & Q2_PAD_RIGHT) ? 1 : 0)
             - ((m->pad_new & Q2_PAD_LEFT)  ? 1 : 0);

    if (!step)
        return;

    switch (m->cursor) {
    case 0:
        before = m->mp_setup.players;
        after  = before + step;
        if (after < 2) after = 2;
        if (after > m->controller_count) after = m->controller_count;
        m->mp_setup.players = (s16)after;
        break;
    case 1:
        before = m->mp_setup.arena;
        after = wrap_option(before + step, Q2_MENU_MP_ARENA_COUNT);
        m->mp_setup.arena = (s16)after;
        break;
    case 2:
        if (m->mp_setup.mode == Q2_MENU_MP_VERSUS) {
            before = m->mp_setup.round_option;
            after = wrap_option(before + step, Q2_MENU_MP_ROUND_OPTIONS);
            m->mp_setup.round_option = (s16)after;
        } else {
            before = m->mp_setup.time_option;
            after = wrap_option(before + step, Q2_MENU_MP_TIME_OPTIONS);
            m->mp_setup.time_option = (s16)after;
        }
        break;
    case 3:
        if (m->mp_setup.mode == Q2_MENU_MP_VERSUS)
            return;
        before = m->mp_setup.frag_option;
        after = wrap_option(before + step, Q2_MENU_MP_FRAG_OPTIONS);
        m->mp_setup.frag_option = (s16)after;
        break;
    default:
        return;
    }

    if (after != before) {
        m->sound = Q2_MSND_TOGGLE;
        front_setup_refresh(m);
    }
}

/* ------------------------------------------------------------------------- */
/* Navigation */

static void move_cursor(q2_menu *m)
{
    int down = -1, up = -1, i;

    if (!any_selectable(m))
        return;

    for (i = m->cursor + 1; i < (int)m->page->count; i++)
        if (q2_menu_item_selectable(m, i)) { down = i; break; }
    for (i = m->cursor - 1; i >= (int)m->page->first; i--)
        if (q2_menu_item_selectable(m, i)) { up = i; break; }

    /* 0x80019E8C: nothing below wraps to the top, nothing above to the
     * bottom. The original gates both on a flag that its own item loader
     * always clears, so wrapping is unconditional here. */
    if (down < 0) down = first_selectable(m);
    if (up   < 0) up   = last_selectable(m);

    if ((m->pad_new & Q2_PAD_UP) && up >= 0 && up != m->cursor) {
        m->cursor = up;
        m->sound  = Q2_MSND_MOVE;
    } else if ((m->pad_new & Q2_PAD_DOWN) && down >= 0 && down != m->cursor) {
        m->cursor = down;
        m->sound  = Q2_MSND_MOVE;
    }
}

/* ------------------------------------------------------------------------- */
/* Pointing at it — see menu.h                                                */
/* ------------------------------------------------------------------------- */
bool q2_menu_point_at(q2_menu *m, int index)
{
    if (!m || !m->open || !m->page)
        return false;
    if (index == m->cursor)
        return false;
    if (!q2_menu_item_selectable(m, index))
        return false;

    /*
     * The SCREEN POSITION page has nothing to point at — it is two lines of
     * static text and the d-pad drives the offset — so the cursor there is not
     * a thing a pointer may move.
     */
    if (m->page_id == Q2_PAGE_SCREEN_POSITION)
        return false;

    m->cursor = index;
    m->sound  = Q2_MSND_MOVE;
    return true;
}

bool q2_menu_set_slider(q2_menu *m, int index, int value)
{
    s16 *v;

    if (!m || !m->open || !m->page)
        return false;
    if (index < 0 || index >= (int)m->page->count)
        return false;
    if (m->page->items[index].widget != Q2_WIDGET_SLIDER)
        return false;

    v = binding(m, index);
    if (!v)
        return false;

    if (value < 0)
        value = 0;
    if (value > Q2_MENU_SLIDER_MAX)
        value = Q2_MENU_SLIDER_MAX;

    if (*v != (s16)value) {
        *v = (s16)value;

        /*
         * The same repeat gate the held-direction path uses, so a drag ticks
         * at the rate a held press does instead of once per frame. Dragging
         * the SOUND FX slider is the one that is meant to be audible, which is
         * why that gate is not reset for it (0x8001BF1C).
         */
        if (m->page->items[index].setting != Q2_SET_SFX)
            m->slide_repeat = 0;
        if (m->slide_repeat < 0) {
            m->slide_repeat = 8;
            m->sound = Q2_MSND_SLIDE;
        }
        m->slide_repeat--;
    }

    return true;
}

/*
 * The SCREEN POSITION page has no items at all: the d-pad moves the display
 * offset directly and × or △ leaves. 0x8001CD64 is the hook that applies it;
 * the range is the one the video reset writes back into (0, 24).
 */
static void update_position_page(q2_menu *m)
{
    s16 *x, *y;

    if (!m->set)
        return;

    x = &m->set->v[Q2_SET_SCREEN_X];
    y = &m->set->v[Q2_SET_SCREEN_Y];

    if (m->pad_new & Q2_PAD_LEFT)  (*x)--;
    if (m->pad_new & Q2_PAD_RIGHT) (*x)++;
    if (m->pad_new & Q2_PAD_UP)    (*y)--;
    if (m->pad_new & Q2_PAD_DOWN)  (*y)++;

    if (*x < -32) *x = -32;
    if (*x >  32) *x =  32;
    if (*y < 0)   *y = 0;
    if (*y >  48) *y = 48;

    if (m->pad_new & Q2_PAD_CROSS) {
        m->sound = Q2_MSND_SELECT;
        go_back(m);
    }
}

void q2_menu_advance(q2_menu *m, u16 buttons)
{
    const q2_menu_item *item;
    u16  prev;
    bool fire;

    if (!m)
        return;

    prev        = m->pad_prev;
    m->pad_new  = (u16)(buttons & ~prev);
    m->pad_rel  = (u16)(prev & ~buttons);
    m->pad_cur  = buttons;
    m->pad_prev = buttons;

    if (!m->open || !m->page)
        return;

    if (m->settle) {
        m->settle = 0;
        return;
    }

    if (m->arm_ticks > 0) {
        /* The death screen ignores everything until it arms itself, and the
         * countdown is spent in LEVEL CLOCK units — 0x80020544 subtracts the
         * frame's dt from it, not one. See q2_menu.frame_dt. */
        m->arm_ticks -= (m->frame_dt > 0) ? m->frame_dt : Q2_DT_NOMINAL;
        if (m->arm_ticks < 0)
            m->arm_ticks = 0;
        return;
    }

    if (m->page_id == Q2_PAGE_SCREEN_POSITION) {
        update_position_page(m);
        return;
    }

    move_cursor(m);

    if (m->page_id == Q2_PAGE_FRONT_DMSETUP)
        update_front_setup(m);

    /* The widget under the cursor, adjusted before the activation test so a
     * left/right press cannot also count as a select. */
    if (m->cursor >= (int)m->page->first && m->cursor < (int)m->page->count) {
        switch (m->page->items[m->cursor].widget) {
        case Q2_WIDGET_TOGGLE: update_toggle(m, m->cursor); break;
        case Q2_WIDGET_SLIDER: update_slider(m, m->cursor); break;
        case Q2_WIDGET_CHOICE: update_choice(m, m->cursor); break;
        default: break;
        }
    }

    /* TRIANGLE — 0x8001A06C. */
    if (m->pad_new & Q2_PAD_TRIANGLE) {
        if (m->page->back != Q2_ACT_NONE) {
            m->sound = Q2_MSND_BACK;
            go_back(m);
        }
        return;
    }

    if (m->cursor < (int)m->page->first || m->cursor >= (int)m->page->count)
        return;

    item = &m->page->items[m->cursor];
    if (item->action == Q2_ACT_NONE)
        return;

    /* 0x8001A0D8: press, or release for the items that ask for it — the
     * original tests "held last frame and not this one" explicitly. */
    if (item->on_release)
        fire = (m->pad_rel & Q2_PAD_CROSS) != 0;
    else
        fire = (m->pad_new & Q2_PAD_CROSS) != 0;

    if (fire) {
        m->sound = Q2_MSND_SELECT;
        run_action(m, item->action);
    }
}

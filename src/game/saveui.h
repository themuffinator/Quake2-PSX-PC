/*
 * saveui.h — the flow behind the memory-card front end.
 *
 * ---------------------------------------------------------------------------
 * What this is, and what it deliberately is not
 * ---------------------------------------------------------------------------
 * memcard.h reconstructs the console's front END: nine screens, their text and
 * coordinates, the on-release rule, and three function pointers the arms call —
 * `poll` (what is the card doing), `request` (go to this state) and `choose`
 * (act on this row). Behind those pointers on the console is `libmcrd` talking
 * to hardware this port does not have, and memcard.h says plainly that
 * reconstructing them would be inventing them.
 *
 * This is the port's own implementation of exactly those three, over host
 * files. The SHAPE is the original's — the same three entry points, the same
 * state numbers, the same "the row is chosen positionally" contract — and the
 * behaviour behind them is this port's and says so.
 *
 * It lives in the game layer rather than next to memcard.c because it is about
 * savegames, not about menus: it needs save.h and nothing from the menu at all.
 * The client is what binds the two together, by filling a `q2_mcard_host` with
 * the three functions below. That keeps the dependency pointing one way and
 * makes this testable without a screen.
 *
 * ---------------------------------------------------------------------------
 * The states, and which of them are the console's
 * ---------------------------------------------------------------------------
 * The numbers are the original's (memcard.h §"The states"). What each one MEANS
 * here is a mixture, and the mixture is worth being precise about:
 *
 *    3  LIST         the console's. It hands a chosen row to the card code, and
 *                    SAVE FILE is the only screen with rows — that mapping is
 *                    established, not guessed.
 *    5  CHOICE       the console's arm (row 1 -> 6, row 0 -> 1). WHICH screen
 *                    it belongs to was not established; three have that shape.
 *                    The port uses it for OVERWRITE?, which is the only one of
 *                    the three a filesystem needs, and that choice is the
 *                    PORT'S rather than a reading.
 *    6  BUSY         the console's arm target. The port does its file I/O here,
 *                    which is what the SAVING screen is for.
 *    13 REPORT       the console's. It composes text, and LOAD MESSAGE is the
 *                    only screen whose text is runtime-composed.
 *    14 ACCEPT       the console's: apply what was loaded and leave.
 *    19 CHOICE       the console's other two-way choice. The port does not use
 *                    it: its remaining candidate is NOT FORMATTED, and there is
 *                    no card to format. Left unclaimed rather than repurposed.
 *
 * The SAVE? prompt is NOT in here. On the console it is a page, and its YES is
 * what installs the state machine in the first place (0x80020428) — so it
 * happens before any of this runs, and the caller shows it.
 */
#ifndef Q2PSX_SAVEUI_H
#define Q2PSX_SAVEUI_H

#include "save.h"

/* The port's use of the state numbers above. The console's own names are in
 * memcard.h; these are duplicated rather than included because the game layer
 * does not depend on the menu. */
#define Q2_SAVEUI_STATE_START    1    /* the choice's "no" arm — back to 3   */
#define Q2_SAVEUI_STATE_LIST     3
#define Q2_SAVEUI_STATE_CHOICE   5    /* OVERWRITE?                          */
#define Q2_SAVEUI_STATE_BUSY     6    /* SAVING / LOADING                    */
#define Q2_SAVEUI_STATE_REPORT  13
#define Q2_SAVEUI_STATE_ACCEPT  14
#define Q2_SAVEUI_STATE_ACCEPT2 16

typedef enum q2_save_ui_mode {
    Q2_SAVE_UI_SAVE = 0,
    Q2_SAVE_UI_LOAD,
    Q2_SAVE_UI_SETTINGS_SAVE,
    Q2_SAVE_UI_SETTINGS_LOAD
} q2_save_ui_mode;

typedef enum q2_save_ui_status {
    Q2_SAVE_UI_IDLE = 0,      /* never opened, or already collected          */
    Q2_SAVE_UI_RUNNING,
    Q2_SAVE_UI_SAVED,
    Q2_SAVE_UI_LOADED,        /* the snapshot is waiting; take it            */
    Q2_SAVE_UI_FAILED,
    Q2_SAVE_UI_CANCELLED
} q2_save_ui_status;

#define Q2_SAVE_UI_MSG_MAX 48
#define Q2_SAVE_UI_ROW_MAX (Q2_SAVE_LABEL_LEN + 8)

typedef struct q2_save_ui {
    bool              open;
    q2_save_ui_mode   mode;
    int               state;      /* one of the numbers above                */
    int               slot;       /* the chosen row, -1 until one is chosen  */
    bool              pending_io; /* state 6 has work for the next update    */
    q2_save_ui_status status;
    q2_result         last_error;

    /* The four slots, and the text SAVE FILE's four rows show. An unused slot
     * is the empty string, which is what makes the row draw nothing and take no
     * selection bar (memcard.h). */
    q2_save_info info[Q2_SAVE_SLOTS];
    char         row[Q2_SAVE_SLOTS][Q2_SAVE_UI_ROW_MAX];

    /*
     * What the REPORT state shows, in TWO lines — because the screen it maps
     * to has two, and both are placeholders the runtime overwrites
     * (`LOAD MESSAGE` and `HERE` at 0x8009B06C). Leaving the second one alone
     * does not blank it: the menu engine falls back to the table's own label
     * when an override is empty, so the screen would say `HERE`.
     */
    char         message[Q2_SAVE_UI_MSG_MAX];
    char         detail[Q2_SAVE_UI_MSG_MAX];

    /* SAVE: what to write. Borrowed — the caller owns it and must keep it
     * alive until the front end closes. */
    const q2_save *source;

    /* LOAD: what came back. Owned; q2_save_ui_take_loaded moves it out and
     * q2_save_ui_free releases whatever is left. */
    q2_save loaded;
    bool    have_loaded;

    /* LOAD/SAVE SETTINGS: an opaque copy, because this layer deliberately
     * does not know menu.h's field layout. */
    q2_settings_blob settings_source;
    q2_settings_blob settings_loaded;
    bool             have_settings_loaded;
} q2_save_ui;

void q2_save_ui_init(q2_save_ui *ui);
void q2_save_ui_free(q2_save_ui *ui);

/* Re-read the four slot headers. Called by both opens; exposed so a caller can
 * refresh the rows after writing one from somewhere else. */
void q2_save_ui_rescan(q2_save_ui *ui);

/* Open on the list. `snapshot` is what a chosen row will be written with and
 * must outlive the session. */
void q2_save_ui_open_save(q2_save_ui *ui, const q2_save *snapshot);
void q2_save_ui_open_load(q2_save_ui *ui);
void q2_save_ui_open_settings_save(q2_save_ui *ui,
                                   const q2_settings_blob *settings);
void q2_save_ui_open_settings_load(q2_save_ui *ui);

/* Abandon the session. The status becomes CANCELLED. */
void q2_save_ui_close(q2_save_ui *ui);

/* ------------------------------------------------------------------------- */
/* The three the front end calls                                              */
/*                                                                            */
/* These have the signatures of `q2_mcard_host`'s members exactly, so a caller */
/* fills one with { q2_save_ui_poll, q2_save_ui_request, q2_save_ui_choose,    */
/* &ui } and the reconstruction drives this without either side including the  */
/* other's header.                                                            */
/* ------------------------------------------------------------------------- */
int  q2_save_ui_poll(void *user);
void q2_save_ui_request(void *user, int state);
void q2_save_ui_choose(void *user, int row);

/*
 * One frame, after the front end has advanced. Performs whatever state 6 asked
 * for — the actual read or write — and moves to the report.
 *
 * The I/O is deferred to here rather than done inside `choose` for the reason
 * the console has a busy screen at all: the transition has to be visible for a
 * frame, or the player never sees that anything happened.
 */
q2_save_ui_status q2_save_ui_update(q2_save_ui *ui);

/*
 * Dismiss the report and finish.
 *
 * State 13's arm on the console composes text and does not transition; what
 * moves it on was not followed, so the press that dismisses it is the PORT's.
 * It lands on state 14, which is the console's accept arm, so the shape is
 * right even where the trigger is not.
 */
void q2_save_ui_acknowledge(q2_save_ui *ui);

/* The text of row `slot`, or "" for an unused one. Never NULL. */
const char *q2_save_ui_row(const q2_save_ui *ui, int slot);

/* Move the loaded snapshot out. Returns false when there is nothing to take;
 * on success `out` owns it and the ui no longer does. */
bool q2_save_ui_take_loaded(q2_save_ui *ui, q2_save *out);
bool q2_save_ui_take_settings(q2_save_ui *ui, q2_settings_blob *out);

const char *q2_save_ui_status_name(q2_save_ui_status s);

#endif /* Q2PSX_SAVEUI_H */

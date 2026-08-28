#include "saveui.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
static void set_message(q2_save_ui *ui, const char *msg, const char *detail)
{
    snprintf(ui->message, sizeof(ui->message), "%s", msg ? msg : "");
    snprintf(ui->detail,  sizeof(ui->detail),  "%s", detail ? detail : "");
}

void q2_save_ui_init(q2_save_ui *ui)
{
    if (!ui)
        return;
    memset(ui, 0, sizeof(*ui));
    ui->slot   = -1;
    ui->status = Q2_SAVE_UI_IDLE;
}

void q2_save_ui_free(q2_save_ui *ui)
{
    if (!ui)
        return;
    q2_save_free(&ui->loaded);
    memset(ui, 0, sizeof(*ui));
    ui->slot = -1;
}

void q2_save_ui_rescan(q2_save_ui *ui)
{
    int i;

    if (!ui)
        return;

    if (ui->mode == Q2_SAVE_UI_SETTINGS_SAVE ||
        ui->mode == Q2_SAVE_UI_SETTINGS_LOAD) {
        bool used[Q2_SAVE_SLOTS];

        q2_settings_slots_scan(used, Q2_SAVE_SLOTS);
        for (i = 0; i < Q2_SAVE_SLOTS; i++) {
            memset(&ui->info[i], 0, sizeof(ui->info[i]));
            ui->info[i].used = used[i];
            if (used[i])
                snprintf(ui->row[i], sizeof(ui->row[i]), "%d MULTIPLAYER",
                         i + 1);
            else
                ui->row[i][0] = '\0';
        }
    } else {
        q2_save_slots_scan(ui->info, Q2_SAVE_SLOTS);
        for (i = 0; i < Q2_SAVE_SLOTS; i++)
            q2_save_slot_row(&ui->info[i], i, ui->row[i],
                             (u32)sizeof(ui->row[i]));
    }
}

static void open_common(q2_save_ui *ui, q2_save_ui_mode mode)
{
    q2_save_free(&ui->loaded);
    ui->have_loaded = false;
    ui->have_settings_loaded = false;

    ui->open       = true;
    ui->mode       = mode;
    ui->state      = Q2_SAVEUI_STATE_LIST;
    ui->slot       = -1;
    ui->pending_io = false;
    ui->status     = Q2_SAVE_UI_RUNNING;
    ui->last_error = Q2_OK;
    ui->message[0] = '\0';
    ui->detail[0]  = '\0';

    q2_save_ui_rescan(ui);
}

void q2_save_ui_open_save(q2_save_ui *ui, const q2_save *snapshot)
{
    if (!ui)
        return;
    open_common(ui, Q2_SAVE_UI_SAVE);
    ui->source = snapshot;
}

void q2_save_ui_open_load(q2_save_ui *ui)
{
    if (!ui)
        return;
    open_common(ui, Q2_SAVE_UI_LOAD);
    ui->source = NULL;
}

void q2_save_ui_open_settings_save(q2_save_ui *ui,
                                   const q2_settings_blob *settings)
{
    if (!ui || !settings)
        return;
    open_common(ui, Q2_SAVE_UI_SETTINGS_SAVE);
    ui->source = NULL;
    ui->settings_source = *settings;
}

void q2_save_ui_open_settings_load(q2_save_ui *ui)
{
    if (!ui)
        return;
    open_common(ui, Q2_SAVE_UI_SETTINGS_LOAD);
    ui->source = NULL;
    memset(&ui->settings_source, 0, sizeof(ui->settings_source));
}

void q2_save_ui_close(q2_save_ui *ui)
{
    if (!ui || !ui->open)
        return;
    ui->open       = false;
    ui->pending_io = false;
    ui->status     = Q2_SAVE_UI_CANCELLED;
}

/* ------------------------------------------------------------------------- */
/* The host contract                                                          */
/* ------------------------------------------------------------------------- */
int q2_save_ui_poll(void *user)
{
    const q2_save_ui *ui = (const q2_save_ui *)user;

    /* A closed front end reports a state with no arm, so the reconstruction
     * runs its "not live" path and does nothing. */
    if (!ui || !ui->open)
        return 0;
    return ui->state;
}

void q2_save_ui_choose(void *user, int row)
{
    q2_save_ui *ui = (q2_save_ui *)user;

    if (!ui || !ui->open)
        return;

    /* The row is already relative — the front end hands over `cursor - first`
     * (memcard.h), which is the slot index. */
    if (row < 0 || row >= Q2_SAVE_SLOTS)
        return;

    ui->slot = row;
}

/*
 * Where the list goes next, which is the one decision that differs between the
 * two modes:
 *
 *   SAVE  an occupied slot needs the OVERWRITE? question first; an empty one
 *         goes straight to the write.
 *   LOAD  an empty slot has nothing to load. The row draws as the empty string
 *         and takes no selection bar, so it cannot normally be picked at all —
 *         this is the belt to that brace.
 */
static void advance_from_list(q2_save_ui *ui)
{
    if (ui->slot < 0 || ui->slot >= Q2_SAVE_SLOTS)
        return;

    if (ui->mode == Q2_SAVE_UI_LOAD ||
        ui->mode == Q2_SAVE_UI_SETTINGS_LOAD) {
        if (!ui->info[ui->slot].used) {
            set_message(ui, "NO SAVE", "IN THIS SLOT");
            return;
        }
        ui->state      = Q2_SAVEUI_STATE_BUSY;
        ui->pending_io = true;
        return;
    }

    if (ui->info[ui->slot].used) {
        ui->state = Q2_SAVEUI_STATE_CHOICE;
        return;
    }

    ui->state      = Q2_SAVEUI_STATE_BUSY;
    ui->pending_io = true;
}

/* State 14/16: apply and leave. What "apply" means is the caller's — on the
 * console it is 0x8001C698, the same game-variable application the GAME
 * VARIABLES page uses, and the port's caller does that from the loaded
 * snapshot's settings. */
static void finish(q2_save_ui *ui)
{
    ui->open       = false;
    ui->pending_io = false;

    if (ui->status == Q2_SAVE_UI_RUNNING)
        ui->status = Q2_SAVE_UI_CANCELLED;
}

void q2_save_ui_request(void *user, int state)
{
    q2_save_ui *ui = (q2_save_ui *)user;

    if (!ui || !ui->open)
        return;

    switch (state) {
    case Q2_SAVEUI_STATE_LIST:
        /*
         * 0x8001F140 hands over the row and then asks for a transition with its
         * OWN state number, so "request(3)" means "the list has chosen", not
         * "go to the list".
         */
        advance_from_list(ui);
        break;

    case Q2_SAVEUI_STATE_START:
        /*
         * The choice's "no" arm. State 1 has no arm of its own, so what it
         * SHOWS was never established; returning to the list is the only thing
         * the port can do with it that is not a dead end, and it is what "back
         * to the start" means when the list is where a session starts.
         */
        ui->state = Q2_SAVEUI_STATE_LIST;
        break;

    case Q2_SAVEUI_STATE_BUSY:
        ui->state      = Q2_SAVEUI_STATE_BUSY;
        ui->pending_io = true;
        break;

    case Q2_SAVEUI_STATE_ACCEPT:
    case Q2_SAVEUI_STATE_ACCEPT2:
        finish(ui);
        break;

    default:
        /* Any other number the front end asks for is honoured verbatim, so a
         * caller experimenting with the console's remaining states sees them
         * rather than having them quietly dropped. */
        ui->state = state;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* The work                                                                   */
/* ------------------------------------------------------------------------- */
static void do_save(q2_save_ui *ui)
{
    q2_result rc;

    if (ui->mode == Q2_SAVE_UI_SETTINGS_SAVE) {
        char where[Q2_SAVE_UI_MSG_MAX];

        rc = q2_settings_slot_write(&ui->settings_source, ui->slot);
        ui->last_error = rc;
        if (rc != Q2_OK) {
            char why[Q2_SAVE_UI_MSG_MAX];
            ui->status = Q2_SAVE_UI_FAILED;
            snprintf(why, sizeof(why), "%s", q2_result_str(rc));
            set_message(ui, "SAVE FAILED", why);
            return;
        }
        ui->status = Q2_SAVE_UI_SAVED;
        snprintf(where, sizeof(where), "TO SLOT %d", ui->slot + 1);
        set_message(ui, "SETTINGS SAVED", where);
        q2_save_ui_rescan(ui);
        return;
    }

    if (!ui->source) {
        ui->last_error = Q2_ERR_INVALID_ARG;
        ui->status     = Q2_SAVE_UI_FAILED;
        set_message(ui, "SAVE FAILED", "NOTHING TO SAVE");
        return;
    }

    rc = q2_save_slot_write(ui->source, ui->slot);
    ui->last_error = rc;

    if (rc != Q2_OK) {
        char why[Q2_SAVE_UI_MSG_MAX];
        ui->status = Q2_SAVE_UI_FAILED;
        snprintf(why, sizeof(why), "%s", q2_result_str(rc));
        set_message(ui, "SAVE FAILED", why);
        return;
    }

    {
        char where[Q2_SAVE_UI_MSG_MAX];
        ui->status = Q2_SAVE_UI_SAVED;
        snprintf(where, sizeof(where), "TO SLOT %d", ui->slot + 1);
        set_message(ui, "GAME SAVED", where);
    }

    /* The row the player just wrote has to say so before the screen is looked
     * at again. */
    q2_save_ui_rescan(ui);
}

static void do_load(q2_save_ui *ui)
{
    q2_result rc;

    if (ui->mode == Q2_SAVE_UI_SETTINGS_LOAD) {
        rc = q2_settings_slot_read(&ui->settings_loaded, ui->slot);
        ui->last_error = rc;
        ui->have_settings_loaded = false;
        if (rc != Q2_OK) {
            char why[Q2_SAVE_UI_MSG_MAX];
            ui->status = Q2_SAVE_UI_FAILED;
            snprintf(why, sizeof(why), "%s", q2_result_str(rc));
            set_message(ui, "LOAD FAILED", why);
            return;
        }
        ui->have_settings_loaded = true;
        ui->status = Q2_SAVE_UI_LOADED;
        set_message(ui, "SETTINGS LOADED", "MULTIPLAYER");
        return;
    }

    q2_save_free(&ui->loaded);
    ui->have_loaded = false;

    rc = q2_save_slot_read(&ui->loaded, ui->slot);
    ui->last_error = rc;

    if (rc != Q2_OK) {
        char why[Q2_SAVE_UI_MSG_MAX];
        ui->status = Q2_SAVE_UI_FAILED;
        snprintf(why, sizeof(why), "%s", q2_result_str(rc));
        set_message(ui, "LOAD FAILED", why);
        return;
    }

    ui->have_loaded = true;
    ui->status      = Q2_SAVE_UI_LOADED;
    set_message(ui, "GAME LOADED",
                ui->loaded.label[0] ? ui->loaded.label : ui->loaded.map);
}

q2_save_ui_status q2_save_ui_update(q2_save_ui *ui)
{
    if (!ui)
        return Q2_SAVE_UI_IDLE;

    if (!ui->open || !ui->pending_io)
        return ui->status;

    ui->pending_io = false;

    if (ui->slot < 0 || ui->slot >= Q2_SAVE_SLOTS) {
        ui->status = Q2_SAVE_UI_FAILED;
        set_message(ui, "FAILED", "NO SLOT CHOSEN");
    } else if (ui->mode == Q2_SAVE_UI_SAVE ||
               ui->mode == Q2_SAVE_UI_SETTINGS_SAVE) {
        do_save(ui);
    } else {
        do_load(ui);
    }

    /* Whatever happened, the player is told. That is what the report state is
     * for, and it is why a failure does not simply close the screen. */
    ui->state = Q2_SAVEUI_STATE_REPORT;
    return ui->status;
}

void q2_save_ui_acknowledge(q2_save_ui *ui)
{
    if (!ui || !ui->open)
        return;
    if (ui->state != Q2_SAVEUI_STATE_REPORT)
        return;

    ui->state = Q2_SAVEUI_STATE_ACCEPT;
    finish(ui);
}

/* ------------------------------------------------------------------------- */
const char *q2_save_ui_row(const q2_save_ui *ui, int slot)
{
    if (!ui || slot < 0 || slot >= Q2_SAVE_SLOTS)
        return "";
    return ui->row[slot];
}

bool q2_save_ui_take_loaded(q2_save_ui *ui, q2_save *out)
{
    if (!ui || !out || !ui->have_loaded)
        return false;

    *out = ui->loaded;
    memset(&ui->loaded, 0, sizeof(ui->loaded));
    ui->have_loaded = false;
    return true;
}

bool q2_save_ui_take_settings(q2_save_ui *ui, q2_settings_blob *out)
{
    if (!ui || !out || !ui->have_settings_loaded)
        return false;

    *out = ui->settings_loaded;
    memset(&ui->settings_loaded, 0, sizeof(ui->settings_loaded));
    ui->have_settings_loaded = false;
    return true;
}

const char *q2_save_ui_status_name(q2_save_ui_status s)
{
    switch (s) {
    case Q2_SAVE_UI_IDLE:      return "idle";
    case Q2_SAVE_UI_RUNNING:   return "running";
    case Q2_SAVE_UI_SAVED:     return "saved";
    case Q2_SAVE_UI_LOADED:    return "loaded";
    case Q2_SAVE_UI_FAILED:    return "failed";
    case Q2_SAVE_UI_CANCELLED: return "cancelled";
    default:                   return "?";
    }
}

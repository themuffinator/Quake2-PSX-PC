#ifndef Q2PSX_CLIENT_GAMEPAD_H
#define Q2PSX_CLIENT_GAMEPAD_H

#include <SDL3/SDL.h>
#include "pad.h"

/* Host device assignment is separate from retail pad decoding. Disconnecting
 * a device vacates its slot; it never moves another player's controller. */
typedef struct q2_gamepads {
    SDL_Gamepad *device[Q2_SIM_MAX_PLAYERS];
    SDL_JoystickID id[Q2_SIM_MAX_PLAYERS];
    u32 held[Q2_SIM_MAX_PLAYERS];
    u32 pressed[Q2_SIM_MAX_PLAYERS];
    u32 menu_pressed[Q2_SIM_MAX_PLAYERS];
    int menu_axis[Q2_SIM_MAX_PLAYERS][2];
    SDL_JoystickID prompt_id;
    s16 axis[Q2_SIM_MAX_PLAYERS][SDL_GAMEPAD_AXIS_COUNT];
    bool changed[Q2_SIM_MAX_PLAYERS];
    int first_slot; /* 1 reserves player one for keyboard/mouse; 0 uses four pads. */
} q2_gamepads;

void q2_gamepads_open(q2_gamepads *pads, int first_slot);
void q2_gamepads_close(q2_gamepads *pads);
int q2_gamepads_event(q2_gamepads *pads, const SDL_Event *event);
/* Current buttons plus taps since the last read. Axes are centred signed bytes.
 * The caller rolls buttons once per simulation tick and clears pressed. */
u32 q2_gamepads_read(const q2_gamepads *pads, int player, int style,
                     q2_pad_state *axes);
/* Menu input is independent of gameplay style, with taps latched until read. */
u16 q2_gamepads_menu(q2_gamepads *pads);
u16 q2_gamepads_menu_player(q2_gamepads *pads, int player);
SDL_Gamepad *q2_gamepads_prompt(const q2_gamepads *pads);
/* Single-player uses the first connected pad without a command-line flag;
 * multiplayer retains the established device/player assignments. */
int q2_gamepads_primary(const q2_gamepads *pads, bool multiplayer);

#endif

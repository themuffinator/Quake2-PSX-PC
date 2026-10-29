/* Shared host button names and stick navigation for the two front ends.
 * SDL's label API respects PlayStation / Xbox / Nintendo mappings. */
#ifndef Q2PSX_CONTROLLER_UI_H
#define Q2PSX_CONTROLLER_UI_H
#include <SDL3/SDL.h>

static inline const char *q2_ui_button_name(SDL_GamepadButtonLabel label,
                                           SDL_GamepadButton button)
{
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "CROSS";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "CIRCLE";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "SQUARE";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "TRIANGLE";
    default:
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "SOUTH";
        case SDL_GAMEPAD_BUTTON_EAST: return "EAST";
        case SDL_GAMEPAD_BUTTON_WEST: return "WEST";
        case SDL_GAMEPAD_BUTTON_NORTH: return "NORTH";
        default: return "START";
        }
    }
}

static inline const char *q2_ui_button(SDL_Gamepad *pad, SDL_GamepadButton button)
{
    return q2_ui_button_name(SDL_GetGamepadButtonLabel(pad, button), button);
}

/* Hysteresis prevents a resting stick from chattering between menu rows. */
static inline int q2_ui_axis(Sint16 value, int previous)
{
    if (value < -16000) return -1;
    if (value > 16000) return 1;
    if (previous < 0 && value < -8000) return -1;
    if (previous > 0 && value > 8000) return 1;
    return 0;
}
#endif

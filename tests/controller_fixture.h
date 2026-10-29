/* SDL-local virtual controllers. No OS input injection or physical input. */
#ifndef Q2_TEST_CONTROLLER_FIXTURE_H
#define Q2_TEST_CONTROLLER_FIXTURE_H
#include <SDL3/SDL.h>
static SDL_JoystickID test_controller(SDL_GamepadType type)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID id;
    char guid[33], mapping[768];
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x1209;
    desc.product_id = (Uint16)(0x7000 + type);
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name = "Q2 menu regression controller";
    id = SDL_AttachVirtualJoystick(&desc);
    if (!id) return 0;
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    SDL_snprintf(mapping, sizeof(mapping),
        "%s,Q2 menu regression controller,a:b0,b:b1,x:b2,y:b3,back:b4,"
        "start:b6,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,"
        "dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,"
        "lefttrigger:a4,righttrigger:a5,type:%s,", guid, SDL_GetGamepadStringForType(type));
    if (!SDL_SetGamepadMapping(id, mapping)) { SDL_DetachVirtualJoystick(id); return 0; }
    return id;
}
#endif

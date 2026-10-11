// Motion controllers as an N64 pad. The actual mapping lives in game/vr_game.c.

#include <ultra64.h>

#include "controller_api.h"
#include "../vr/vr.h"
#include "game/vr_game.h"

static void vr_ctrl_init(void) {
}

static void vr_ctrl_read(OSContPad *pad) {
    vr_game_read_pad(pad);
}

static u32 vr_ctrl_rawkey(void) {
    return VK_INVALID;
}

static void vr_ctrl_rumble_play(float strength, float length) {
    vr_haptic(VR_HAND_LEFT, strength, length);
    vr_haptic(VR_HAND_RIGHT, strength, length);
}

static void vr_ctrl_rumble_stop(void) {
    vr_haptic(VR_HAND_LEFT, 0.0f, 0.0f);
    vr_haptic(VR_HAND_RIGHT, 0.0f, 0.0f);
}

struct ControllerAPI controller_vr = {
    0x3000,
    vr_ctrl_init,
    vr_ctrl_read,
    vr_ctrl_rawkey,
    vr_ctrl_rumble_play,
    vr_ctrl_rumble_stop,
    NULL,
    NULL,
};

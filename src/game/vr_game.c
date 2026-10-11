// Game-side VR logic: first-person camera at Mario's eyes, head hiding, arm IK from the
// motion controllers, body turning, snap turn, punch gestures and controller mapping.
//
// Coordinates: the game world is right-handed Y-up like OpenXR's tracking space, so tracking
// space maps into the world with a scale, a rotation about +Y and a translation:
//     world = anchor + RotY(yaw) * scale * (tracked - gVrHeadCenter)

#include <PR/ultratypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "sm64.h"
#include "types.h"
#include "game_init.h"
#include "camera.h"
#include "level_update.h"
#include "object_list_processor.h"
#include "rendering_graph_node.h"
#include "engine/math_util.h"
#include "engine/graph_node.h"

#include "vr_game.h"
#include "pc/vr/vr_math.h"

extern s16 gMatStackIndex;
extern Mat4 gMatStack[32];

#define SM64_TO_RAD(a) ((float) (a) * (float) M_PI / 32768.0f)

// Tunables (environment variables so they can be changed without rebuilding)
static float sWorldScale = 100.0f; // world units per meter
static float sEyeUp = 40.0f;       // along the head bone, from the neck joint
static float sEyeFwd = 22.0f;      // along Mario's facing
static float sLeash = 0.30f;       // meters of physical lean before the view pulls Mario's body along
static bool sIkEnabled = true;
static bool sTunablesLoaded;

static bool sFpWanted = true;
static float sFpYaw;
static float sYawOffset;
static bool sNeedRecenter = true;
static enum VrViewMode sLastMode = VR_VIEW_NONE;

static float sAnchorLast[3];
static bool sAnchorValid;
static float sEyeOffset[3];
static bool sEyeOffsetValid;
static bool sHeadSeen;

static struct Object *sCurObj;
static int sPartIdx = -1;
static const u8 *sMtxPoolStart;
static bool sHideHead;
static struct GraphNodeRotation *sHeadNode;

static bool sIkHand[2];
static float sIkTargetView[2][3];
static float sIkPoleView[2][3];
static float sIkHandDirView[2][3];
static bool sLoggedSkeleton;

static bool sLatchJump, sLatchAttack, sLatchStart, sLatchZ;
static float sPunchCooldown;
static int sPunchTicks;
static bool sTurnArmed = true;
static bool sPrevView, sPrevRecenter;
static bool sMarioDrawn;

static float env_or(const char *name, float def);
static float hmd_yaw(void);

// floating panel + START button
static enum VrViewMode sPanelMode = VR_VIEW_NONE;
static bool sPanelFollowing;
static bool sPrevRecenterPanel;
static bool sUiOwnsTrigger[2];
static bool sTrigWasDown[2];
static bool sTouchWasIn[2];
static bool sHoverPrev[2];
static int sUiStartTicks;

static void place_panel(float dist) {
    const struct VrTracking *t = &gVrTrack;
    float yaw = hmd_yaw();
    gVrPanel.yaw = yaw;
    gVrPanel.pos[0] = t->head.pos[0] - sinf(yaw) * dist;
    gVrPanel.pos[1] = t->head.pos[1] - 0.05f;
    gVrPanel.pos[2] = t->head.pos[2] - cosf(yaw) * dist;
    gVrPanel.valid = true;
}

// Menus: a big screen that stays exactly where it was put (until Recenter).
// In game: the HUD/pause panel stays in place too, and only swings back in front of the player
// once they've turned well away from it.
static void update_panel(double dt, bool recenter) {
    const struct VrTracking *t = &gVrTrack;
    enum VrViewMode mode = gVrFrameCommitted.mode;
    bool screen = mode == VR_VIEW_SCREEN;
    float dist = screen ? env_or("VR_SCREEN_DIST", 2.2f) : env_or("VR_HUD_DIST", 1.6f);

    if (screen) {
        gVrPanel.halfW = env_or("VR_SCREEN_WIDTH", 2.0f) * 0.5f;
    } else {
        gVrPanel.halfW = dist * 0.62f * env_or("VR_HUD_SCALE", 1.0f);
    }
    gVrPanel.halfH = gVrPanel.halfW * 0.75f;

    bool modeChanged = screen != (sPanelMode == VR_VIEW_SCREEN);
    if (!gVrPanel.valid || modeChanged || recenter) {
        place_panel(dist);
        sPanelFollowing = false;
    } else if (!screen) {
        float diff = vr_wrap_angle(hmd_yaw() - gVrPanel.yaw);
        if (fabsf(diff) > 0.70f) sPanelFollowing = true;  // ~40 degrees
        if (sPanelFollowing) {
            float step = (float) dt * 2.5f;
            gVrPanel.yaw = vr_wrap_angle(gVrPanel.yaw + (fabsf(diff) < step ? diff : (diff > 0 ? step : -step)));
            if (fabsf(diff) < 0.05f) sPanelFollowing = false;
        }
        // keep the panel at its distance as the player moves around the room
        float k = fminf(1.0f, (float) dt * 3.0f);
        float target[3] = {
            t->head.pos[0] - sinf(gVrPanel.yaw) * dist,
            t->head.pos[1] - 0.05f,
            t->head.pos[2] - cosf(gVrPanel.yaw) * dist,
        };
        for (int i = 0; i < 3; i++) gVrPanel.pos[i] += (target[i] - gVrPanel.pos[i]) * k;
    }
    sPanelMode = screen ? VR_VIEW_SCREEN : mode;

    // START button: under the screen in menus, bottom-right corner of the HUD in game
    struct VrButton *b = &gVrStartButton;
    b->visible = true;
    if (screen) {
        b->hw = 0.24f;
        b->hh = 0.09f;
        b->cx = 0;
        b->cy = -gVrPanel.halfH - 0.16f;
    } else {
        b->hw = 0.13f;
        b->hh = 0.05f;
        b->cx = gVrPanel.halfW - b->hw - 0.03f;
        b->cy = -gVrPanel.halfH + b->hh + 0.03f;
    }
}

static void press_start(int hand) {
    sUiStartTicks = 2;
    vr_haptic(hand, 0.8f, 0.06f);
}

// Laser pointers: aim at the START button and pull the trigger (or just poke it with a hand).
static void update_pointer(void) {
    const struct VrTracking *t = &gVrTrack;
    struct VrButton *b = &gVrStartButton;
    bool screen = gVrFrameCommitted.mode == VR_VIEW_SCREEN;
    VrMat panel, inv;
    vr_panel_matrix(panel);
    vrm_rigid_inverse(inv, panel);
    b->hovered = false;

    for (int h = 0; h < 2; h++) {
        bool hover = false;
        gVrLaserActive[h] = false;
        if (t->aimValid[h] && gVrPanel.valid) {
            VrMat am;
            vrm_from_pose(am, t->aim[h].quat, t->aim[h].pos);
            float dirT[3] = { -am[2][0], -am[2][1], -am[2][2] };
            float o[3], d[3];
            vrm_point(o, t->aim[h].pos, inv);
            vrm_dir(d, dirT, inv);
            if (d[2] < -1e-4f && o[2] > 0) {
                float s = -o[2] / d[2];
                float x = o[0] + d[0] * s, y = o[1] + d[1] * s;
                bool onButton = b->visible && fabsf(x - b->cx) < b->hw + 0.02f && fabsf(y - b->cy) < b->hh + 0.02f;
                bool onScreen = screen && fabsf(x) < gVrPanel.halfW && fabsf(y) < gVrPanel.halfH;
                if (s < 6.0f && (onButton || onScreen)) {
                    gVrLaserActive[h] = true;
                    for (int i = 0; i < 3; i++) gVrLaserHit[h][i] = t->aim[h].pos[i] + dirT[i] * s;
                }
                hover = onButton && s < 6.0f;
            }
        }
        if (hover && !sHoverPrev[h]) vr_haptic(h, 0.25f, 0.02f);
        sHoverPrev[h] = hover;

        bool down = t->trigger[h] > 0.6f;
        if (hover && down && !sTrigWasDown[h]) press_start(h);
        // a trigger pulled on the button belongs to the UI until it's released
        sUiOwnsTrigger[h] = hover || (sUiOwnsTrigger[h] && t->trigger[h] > 0.2f);
        sTrigWasDown[h] = down;
        if (hover) b->hovered = true;

        // poke: the hand pushes through the button
        bool in = false;
        if (t->handValid[h] && b->visible) {
            float p[3];
            vrm_point(p, t->grip[h].pos, inv);
            in = fabsf(p[0] - b->cx) < b->hw && fabsf(p[1] - b->cy) < b->hh && fabsf(p[2]) < 0.06f;
        }
        if (in && !sTouchWasIn[h]) press_start(h);
        sTouchWasIn[h] = in;
    }
}

static float env_or(const char *name, float def) {
    const char *v = getenv(name);
    return v ? (float) atof(v) : def;
}

static void load_tunables(void) {
    if (sTunablesLoaded) return;
    sTunablesLoaded = true;
    sWorldScale = env_or("VR_WORLD_SCALE", sWorldScale);
    sEyeUp = env_or("VR_EYE_UP", sEyeUp);
    sEyeFwd = env_or("VR_EYE_FWD", sEyeFwd);
    sLeash = env_or("VR_LEASH", sLeash);
    sIkEnabled = env_or("VR_ARM_IK", 1.0f) != 0.0f;
    sFpWanted = env_or("VR_FIRST_PERSON", 1.0f) != 0.0f;
}

// ---- tracking helpers -------------------------------------------------------------------------

static float hmd_yaw(void) {
    return vrq_yaw(gVrTrack.head.quat);
}

// Head forward direction in world space for a given tracking->world yaw.
static void head_forward_world(float out[3], float yaw) {
    VrMat hm, ry;
    const float zero[3] = { 0, 0, 0 };
    vrm_from_pose(hm, gVrTrack.head.quat, zero);
    float f[3] = { -hm[2][0], -hm[2][1], -hm[2][2] };
    vrm_rot_y(ry, yaw);
    vrm_dir(out, f, ry);
}

static void tracking_to_world(float out[3], const float p[3], const float anchor[3], float yaw) {
    VrMat ry;
    float d[3] = {
        (p[0] - gVrHeadCenter[0]) * sWorldScale,
        (p[1] - gVrHeadCenter[1]) * sWorldScale,
        (p[2] - gVrHeadCenter[2]) * sWorldScale,
    };
    vrm_rot_y(ry, yaw);
    vrm_dir(out, d, ry);
    out[0] += anchor[0];
    out[1] += anchor[1];
    out[2] += anchor[2];
}

static s16 head_sm64_yaw(void) {
    float f[3];
    head_forward_world(f, sLastMode == VR_VIEW_FIRST ? sFpYaw : gVrFrameCommitted.yaw);
    return atan2s(f[2], f[0]);
}

static void recenter_position(void) {
    memcpy(gVrHeadCenter, gVrTrack.head.pos, sizeof(gVrHeadCenter));
}

static void recenter_fp(void) {
    recenter_position();
    // Look direction at recenter becomes Mario's facing. SM64 yaw psi faces (sin psi, cos psi)
    // in (x, z); tracking yaw theta faces (-sin theta, -cos theta), so theta = psi + pi.
    float marioTheta = SM64_TO_RAD(gMarioState->faceAngle[1]) + (float) M_PI;
    sFpYaw = vr_wrap_angle(marioTheta - hmd_yaw());
}

static void recenter_basic(void) {
    recenter_position();
    sYawOffset = -hmd_yaw();
}

// ---- view mode -----------------------------------------------------------------------------

static bool fp_allowed(void) {
    struct MarioState *m = gMarioState;
    if (!sFpWanted || gMarioObject == NULL || m == NULL || m->marioObj == NULL) return false;
    switch (m->action) {
        case ACT_IN_CANNON:
        case ACT_INTRO_CUTSCENE:
        case ACT_END_PEACH_CUTSCENE:
        case ACT_CREDITS_CUTSCENE:
        case ACT_END_WAVING_CUTSCENE:
            return false;
    }
    return true;
}

static bool ik_allowed(void) {
    struct MarioState *m = gMarioState;
    u32 group = m->action & ACT_GROUP_MASK;
    if (!sIkEnabled) return false;
    if (group != ACT_GROUP_STATIONARY && group != ACT_GROUP_MOVING && group != ACT_GROUP_AIRBORNE) return false;
    if (m->action & (ACT_FLAG_SWIMMING | ACT_FLAG_ON_POLE | ACT_FLAG_HANGING | ACT_FLAG_INTANGIBLE)) return false;
    if (m->heldObj != NULL) return false;
    return true;
}

bool vr_fp_active(void) {
    return vr_enabled() && sLastMode == VR_VIEW_FIRST;
}

s16 vr_fp_camera_yaw(void) {
    // SM64 camera yaw points from the focus back to the camera
    return (s16) (head_sm64_yaw() + 0x8000);
}

static void default_eye(float out[3]) {
    struct Object *o = gMarioObject;
    s16 yaw = o->header.gfx.angle[1];
    out[0] = o->header.gfx.pos[0] + sins(yaw) * 10.0f;
    out[1] = o->header.gfx.pos[1] + 120.0f;
    out[2] = o->header.gfx.pos[2] + coss(yaw) * 10.0f;
}

void vr_camera_override(struct GraphNodeCamera *gc) {
    if (!vr_enabled()) return;
    struct VrFrameInfo *b = &gVrFrameBuild;
    gc->rollScreen = 0;
    b->scale = sWorldScale;

    if (fp_allowed()) {
        b->mode = VR_VIEW_FIRST;
        if (sLastMode != VR_VIEW_FIRST || sNeedRecenter) {
            recenter_fp();
            sNeedRecenter = false;
        }
        b->yaw = sFpYaw;
        float pos[3], f[3];
        if (sAnchorValid && sLastMode == VR_VIEW_FIRST) {
            memcpy(pos, sAnchorLast, sizeof(pos));
        } else {
            default_eye(pos);
        }
        head_forward_world(f, sFpYaw);
        // keep lookat well-defined when looking straight up/down
        if (f[1] > 0.98f) f[1] = 0.98f;
        if (f[1] < -0.98f) f[1] = -0.98f;
        vec3f_copy(gc->pos, pos);
        gc->focus[0] = pos[0] + f[0] * 100.0f;
        gc->focus[1] = pos[1] + f[1] * 100.0f;
        gc->focus[2] = pos[2] + f[2] * 100.0f;
        // first guess for the anchor; refined from the head bone when Mario is drawn
        memcpy(b->anchorCur, pos, sizeof(pos));
    } else {
        b->mode = VR_VIEW_THIRD;
        if (sNeedRecenter) {
            recenter_basic();
            sNeedRecenter = false;
        }
        float dx = gc->focus[0] - gc->pos[0];
        float dz = gc->focus[2] - gc->pos[2];
        float camTheta = (dx * dx + dz * dz > 1e-4f) ? vr_atan2(-dx, -dz) : 0.0f;
        b->yaw = vr_wrap_angle(camTheta + sYawOffset);
        memcpy(b->anchorCur, gc->pos, sizeof(b->anchorCur));
    }
}

void vr_capture_camera(Mat4 view) {
    if (!vr_enabled()) return;
    memcpy(gVrFrameBuild.viewGame, view, sizeof(gVrFrameBuild.viewGame));
    gVrFrameBuild.haveCamera = true;
}

// ---- Mario rendering ------------------------------------------------------------------------

void vr_object_begin(struct Object *obj) {
    if (!vr_enabled() || obj != gMarioObject) return;
    sMarioDrawn = true;
    sCurObj = obj;
    sPartIdx = 0;
    sMtxPoolStart = gGfxPoolEnd;
    sIkHand[0] = sIkHand[1] = false;
}

void vr_object_end(struct Object *obj) {
    if (obj == NULL || obj != sCurObj) return;
    // display list memory is allocated downwards from gGfxPoolEnd
    gVrFrameBuild.marioMtxLo = gGfxPoolEnd;
    gVrFrameBuild.marioMtxHi = sMtxPoolStart;
    sCurObj = NULL;
    sPartIdx = -1;
}

static bool drawing_player(void) {
    return sCurObj != NULL && gCurGraphNodeObject == &sCurObj->header.gfx;
}

void vr_mario_head(struct GraphNode *node, UNUSED Mat4 unused) {
    sHeadNode = (struct GraphNodeRotation *) node->next;
    sHideHead = false;
    if (!drawing_player() || gVrFrameBuild.mode != VR_VIEW_FIRST || !gVrFrameBuild.haveCamera) return;
    sHideHead = true;

    struct VrFrameInfo *b = &gVrFrameBuild;
    float (*m)[4] = gMatStack[gMatStackIndex];
    VrMat invView;
    vrm_inverse(invView, b->viewGame);

    float neck[3], up[3];
    float neckView[3] = { m[3][0], m[3][1], m[3][2] };
    float upView[3] = { m[0][0], m[0][1], m[0][2] };
    vrm_point(neck, neckView, invView);
    vrm_dir(up, upView, invView);
    vrv_normalize(up);

    struct Object *o = sCurObj;
    s16 yaw = o->header.gfx.angle[1];
    float eye[3] = {
        neck[0] + up[0] * sEyeUp + sins(yaw) * sEyeFwd,
        neck[1] + up[1] * sEyeUp,
        neck[2] + up[2] * sEyeUp + coss(yaw) * sEyeFwd,
    };
    // Follow Mario's position exactly, but smooth out the head bob from his animations.
    float target[3] = { eye[0] - o->header.gfx.pos[0], eye[1] - o->header.gfx.pos[1], eye[2] - o->header.gfx.pos[2] };
    if (!sEyeOffsetValid) {
        memcpy(sEyeOffset, target, sizeof(target));
        sEyeOffsetValid = true;
    } else {
        for (int i = 0; i < 3; i++) {
            sEyeOffset[i] += (target[i] - sEyeOffset[i]) * 0.35f;
        }
    }
    for (int i = 0; i < 3; i++) {
        b->anchorCur[i] = o->header.gfx.pos[i] + sEyeOffset[i];
    }
    sHeadSeen = true;

    if (!sLoggedSkeleton) {
        printf("[vr] head bone: neck (%.0f %.0f %.0f) up (%.2f %.2f %.2f), mario pos (%.0f %.0f %.0f)\n",
               neck[0], neck[1], neck[2], up[0], up[1], up[2],
               o->header.gfx.pos[0], o->header.gfx.pos[1], o->header.gfx.pos[2]);
        fflush(stdout);
    }

    // Arm IK targets for this frame, in the game's view space (same space as gMatStack).
    if (!ik_allowed()) return;
    float right[3] = { -coss(yaw), 0, sins(yaw) };
    float fwd[3] = { sins(yaw), 0, coss(yaw) };
    for (int h = 0; h < 2; h++) {
        if (!gVrTrack.handValid[h]) continue;
        VrMat gm;
        const float back[3] = { 0, 0, 0.05f }; // grip center -> wrist
        float wristT[3], world[3];
        vrm_from_pose(gm, gVrTrack.grip[h].quat, gVrTrack.grip[h].pos);
        vrm_point(wristT, back, gm);
        tracking_to_world(world, wristT, b->anchorCur, b->yaw);
        vrm_point(sIkTargetView[h], world, b->viewGame);

        float side = (h == VR_HAND_RIGHT) ? 1.0f : -1.0f;
        float pole[3] = {
            right[0] * side * 0.7f - fwd[0] * 0.5f,
            -1.0f,
            right[2] * side * 0.7f - fwd[2] * 0.5f,
        };
        vrm_dir(sIkPoleView[h], pole, b->viewGame);
        vrv_normalize(sIkPoleView[h]);

        // the hand points where the controller points (grip -Z), rotated into the world
        VrMat ry;
        float fwdT[3] = { -gm[2][0], -gm[2][1], -gm[2][2] }, fwdW[3];
        vrm_rot_y(ry, b->yaw);
        vrm_dir(fwdW, fwdT, ry);
        vrm_dir(sIkHandDirView[h], fwdW, b->viewGame);
        sIkHand[h] = true;
    }
}

bool vr_hide_rotation_node(struct GraphNodeRotation *node) {
    return sHideHead && node == sHeadNode;
}

// Rotate the three axis rows of m so that row 0 points along `to` (shortest rotation),
// turning by at most maxAngle radians.
static void swing_rows_limited(float m[4][4], const float to[3], float maxAngle) {
    float a[3] = { m[0][0], m[0][1], m[0][2] };
    float b[3] = { to[0], to[1], to[2] };
    if (vrv_normalize(a) < 1e-6f || vrv_normalize(b) < 1e-6f) return;
    float k[3];
    vrv_cross(k, a, b);
    float s = vrv_len(k), c = vrv_dot(a, b);
    if (s < 1e-6f) return;
    k[0] /= s; k[1] /= s; k[2] /= s;
    if (vr_atan2(s, c) > maxAngle) {
        s = sinf(maxAngle);
        c = cosf(maxAngle);
    }
    for (int r = 0; r < 3; r++) {
        float v[3] = { m[r][0], m[r][1], m[r][2] }, kxv[3];
        vrv_cross(kxv, k, v);
        float kv = vrv_dot(k, v) * (1.0f - c);
        for (int i = 0; i < 3; i++) {
            m[r][i] = v[i] * c + kxv[i] * s + k[i] * kv;
        }
    }
}

static void swing_rows(float m[4][4], const float to[3]) {
    swing_rows_limited(m, to, (float) M_PI);
}

// Re-orthonormalize the axis rows (a stretched parent leaves its children sheared) to length k.
static void ortho_rows(float m[4][4], float k) {
    float r0[3] = { m[0][0], m[0][1], m[0][2] }, r1[3] = { m[1][0], m[1][1], m[1][2] };
    float r2old[3] = { m[2][0], m[2][1], m[2][2] }, r2[3];
    if (vrv_normalize(r0) < 1e-6f) return;
    float d = vrv_dot(r0, r1);
    for (int i = 0; i < 3; i++) r1[i] -= r0[i] * d;
    if (vrv_normalize(r1) < 1e-6f) return;
    vrv_cross(r2, r0, r1);
    if (vrv_dot(r2, r2old) < 0) {
        r2[0] = -r2[0]; r2[1] = -r2[1]; r2[2] = -r2[2];
    }
    for (int i = 0; i < 3; i++) {
        m[0][i] = r0[i] * k;
        m[1][i] = r1[i] * k;
        m[2][i] = r2[i] * k;
    }
}

static float sArmScale[2] = { 1.0f, 1.0f }; // Mario's unscaled bone length factor (object scale)
static float sArmStretch[2] = { 1.0f, 1.0f };

static void ik_upper_arm(float m[4][4], int h) {
    float S[3] = { m[3][0], m[3][1], m[3][2] };
    float scale = vrv_len(m[0]);
    const float *T = sIkTargetView[h];
    float dir[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] };
    float d = vrv_normalize(dir);
    if (d < 1e-3f) return;
    // Mario's arms stretch (cartoon style) when the player's real hand is further away than he
    // can reach, so his gloves always end up where the controllers are.
    float reach = (65.0f + 60.0f) * scale - 0.5f;
    float stretch = d / reach;
    float maxStretch = env_or("VR_ARM_STRETCH_MAX", 3.0f);
    if (stretch < 1.0f) stretch = 1.0f;
    if (stretch > maxStretch) stretch = maxStretch;
    sArmScale[h] = scale;
    sArmStretch[h] = stretch;
    float L1 = 65.0f * scale * stretch, L2 = 60.0f * scale * stretch;
    float dmin = fabsf(L1 - L2) + 1.0f, dmax = L1 + L2 - 0.5f;
    if (d < dmin) d = dmin;
    if (d > dmax) d = dmax;
    float a = (L1 * L1 - L2 * L2 + d * d) / (2.0f * d);
    float hgt = sqrtf(fmaxf(0.0f, L1 * L1 - a * a));
    const float *pole = sIkPoleView[h];
    float pd = vrv_dot(pole, dir);
    float pp[3] = { pole[0] - dir[0] * pd, pole[1] - dir[1] * pd, pole[2] - dir[2] * pd };
    if (vrv_normalize(pp) < 1e-4f) hgt = 0;
    float E[3];
    for (int i = 0; i < 3; i++) {
        E[i] = S[i] + dir[i] * a + pp[i] * hgt;
    }
    float to[3] = { E[0] - S[0], E[1] - S[1], E[2] - S[2] };
    swing_rows(m, to);
    for (int i = 0; i < 3; i++) m[0][i] *= stretch; // lengthen along the bone only
}

static void ik_forearm(float m[4][4], int h) {
    const float *T = sIkTargetView[h];
    ortho_rows(m, sArmScale[h]);
    float to[3] = { T[0] - m[3][0], T[1] - m[3][1], T[2] - m[3][2] };
    swing_rows(m, to);
    for (int i = 0; i < 3; i++) m[0][i] *= sArmStretch[h];
}

static void ik_hand(float m[4][4], int h) {
    ortho_rows(m, sArmScale[h]); // the glove keeps its normal size
    swing_rows_limited(m, sIkHandDirView[h], 1.2f);
}

// Mario's animated parts, in traversal order:
// 0 root, 1 butt, 2 torso, 3 head, 4 L shoulder, 5 L upper arm, 6 L forearm, 7 L hand,
// 8 R shoulder, 9 R upper arm, 10 R forearm, 11 R hand, 12-19 legs.
void vr_anim_part(Mat4 mtx) {
    if (!drawing_player() || sPartIdx < 0) return;
    int idx = sPartIdx++;
    if (!sLoggedSkeleton && idx == 19) {
        sLoggedSkeleton = true;
    }
    switch (idx) {
        case 5:  if (sIkHand[VR_HAND_LEFT]) ik_upper_arm(mtx, VR_HAND_LEFT); break;
        case 6:  if (sIkHand[VR_HAND_LEFT]) ik_forearm(mtx, VR_HAND_LEFT); break;
        case 9:  if (sIkHand[VR_HAND_RIGHT]) ik_upper_arm(mtx, VR_HAND_RIGHT); break;
        case 10: if (sIkHand[VR_HAND_RIGHT]) ik_forearm(mtx, VR_HAND_RIGHT); break;
        case 7:  if (sIkHand[VR_HAND_LEFT]) ik_hand(mtx, VR_HAND_LEFT); break;
        case 11: if (sIkHand[VR_HAND_RIGHT]) ik_hand(mtx, VR_HAND_RIGHT); break;
    }
}

s32 vr_hand_case(s32 rightHand, s32 defaultCase) {
    int h = rightHand ? VR_HAND_RIGHT : VR_HAND_LEFT;
    if (!drawing_player() || !sIkHand[h]) return defaultCase;
    return gVrTrack.squeeze[h] > 0.5f ? 0 /* fist */ : 1 /* open */;
}

// ---- tick / frame ----------------------------------------------------------------------------

// Standing / crouching actions where Mario may simply turn on the spot.
static bool body_can_turn(u32 action) {
    if ((action & ACT_GROUP_MASK) != ACT_GROUP_STATIONARY) return false;
    if (action & ACT_FLAG_IDLE) return action != ACT_START_SLEEPING;
    switch (action) {
        case ACT_CROUCHING:
        case ACT_START_CROUCHING:
        case ACT_STOP_CROUCHING:
        case ACT_BRAKING_STOP:
        case ACT_IN_QUICKSAND:
            return true;
    }
    return false;
}

void vr_game_begin_tick(void) {
    if (!vr_enabled()) return;
    load_tunables();
    struct VrFrameInfo *b = &gVrFrameBuild;
    memset(b, 0, sizeof(*b));
    b->mode = VR_VIEW_NONE;
    b->scale = sWorldScale;
    b->yaw = sYawOffset;
    sHeadSeen = false;
    sHideHead = false;
    sMarioDrawn = false;

    // Not moving: Mario's body turns with the player's real body/head. Starts once the player has
    // turned ~25 degrees away from Mario's facing and keeps turning until they line up again,
    // so small glances don't wobble him around.
    static bool sBodyFollowing;
    struct MarioState *m = gMarioState;
    if (sLastMode == VR_VIEW_FIRST && gMarioObject != NULL && m != NULL && m->marioObj != NULL
        && body_can_turn(m->action)) {
        s16 diff = (s16) (head_sm64_yaw() - m->faceAngle[1]);
        s32 adiff = diff < 0 ? -diff : diff;
        if (adiff > 0x1200) sBodyFollowing = true;
        if (sBodyFollowing) {
            s16 maxStep = 0xC00; // ~17 degrees per tick
            s16 step = diff > maxStep ? maxStep : (diff < -maxStep ? -maxStep : diff);
            m->faceAngle[1] += step;
            m->marioObj->header.gfx.angle[1] = m->faceAngle[1];
            if (adiff < 0x200) sBodyFollowing = false;
        }
    } else {
        sBodyFollowing = false;
    }
}

void vr_game_commit(void) {
    if (!vr_enabled()) return;
    struct VrFrameInfo *b = &gVrFrameBuild;
    if (b->mode == VR_VIEW_FIRST && !sHeadSeen && gMarioObject != NULL) {
        if (sEyeOffsetValid) {
            for (int i = 0; i < 3; i++) b->anchorCur[i] = gMarioObject->header.gfx.pos[i] + sEyeOffset[i];
        } else {
            default_eye(b->anchorCur);
        }
    }
    // No Mario on screen (title, logo, file select, act select): show the game's own 2D picture
    // on a floating screen instead of putting the player inside the scene.
    if (!sMarioDrawn) {
        b->mode = VR_VIEW_SCREEN;
    }
    if (b->mode == VR_VIEW_NONE || b->mode == VR_VIEW_SCREEN) {
        if (sNeedRecenter) {
            recenter_basic();
            sNeedRecenter = false;
        }
        b->anchorCur[0] = b->anchorCur[1] = b->anchorCur[2] = 0;
        b->yaw = sYawOffset;
    }
    if (b->mode != VR_VIEW_FIRST) {
        sEyeOffsetValid = false;
    }

    float dx = b->anchorCur[0] - sAnchorLast[0];
    float dy = b->anchorCur[1] - sAnchorLast[1];
    float dz = b->anchorCur[2] - sAnchorLast[2];
    bool continuous = sAnchorValid && sLastMode == b->mode && dx * dx + dy * dy + dz * dz < 400.0f * 400.0f;
    memcpy(b->anchorPrev, continuous ? sAnchorLast : b->anchorCur, sizeof(b->anchorPrev));

    gVrFrameCommitted = *b;
    memcpy(sAnchorLast, b->anchorCur, sizeof(sAnchorLast));
    sAnchorValid = true;
    sLastMode = b->mode;
}

void vr_game_frame(double dt) {
    struct VrTracking *t = &gVrTrack;
    load_tunables();

    if (t->recenter && !sPrevRecenter) sNeedRecenter = true;
    sPrevRecenter = t->recenter;
    if (t->viewToggle && !sPrevView) sFpWanted = !sFpWanted;
    sPrevView = t->viewToggle;

    // Snap turn (first person). Third person uses the right stick for Lakitu instead.
    if (sLastMode == VR_VIEW_FIRST) {
        float x = t->turn[0];
        if (sTurnArmed && fabsf(x) > 0.7f) {
            sFpYaw = vr_wrap_angle(sFpYaw - (x > 0 ? 1.0f : -1.0f) * (float) M_PI / 6.0f);
            sTurnArmed = false;
        } else if (fabsf(x) < 0.3f) {
            sTurnArmed = true;
        }

        // Physical lean is free up to the leash; beyond it the anchor follows the head.
        float off[3] = { t->head.pos[0] - gVrHeadCenter[0], 0, t->head.pos[2] - gVrHeadCenter[2] };
        float l = vrv_len(off);
        if (l > sLeash) {
            float k = (l - sLeash) / l;
            gVrHeadCenter[0] += off[0] * k;
            gVrHeadCenter[2] += off[2] * k;
        }
        float dy = t->head.pos[1] - gVrHeadCenter[1];
        if (dy > 0.15f) gVrHeadCenter[1] += dy - 0.15f;
        if (dy < -0.8f) gVrHeadCenter[1] += dy + 0.8f;
    }

    update_panel(dt, t->recenter && !sPrevRecenterPanel);
    sPrevRecenterPanel = t->recenter;
    update_pointer();

    if (t->jump) sLatchJump = true;
    if (t->attack || (t->trigger[VR_HAND_RIGHT] > 0.6f && !sUiOwnsTrigger[VR_HAND_RIGHT])) sLatchAttack = true;
    if (t->start) sLatchStart = true;
    if (t->trigger[VR_HAND_LEFT] > 0.5f && !sUiOwnsTrigger[VR_HAND_LEFT]) sLatchZ = true;

    // Punch gesture: a fast jab forward with either hand presses B.
    sPunchCooldown -= (float) dt;
    if (sLastMode == VR_VIEW_FIRST && sPunchCooldown <= 0) {
        VrMat hm;
        const float zero[3] = { 0, 0, 0 };
        vrm_from_pose(hm, t->head.quat, zero);
        float f[3] = { -hm[2][0], 0, -hm[2][2] };
        vrv_normalize(f);
        for (int h = 0; h < 2; h++) {
            if (!t->handValid[h]) continue;
            float speed = vrv_len(t->handVel[h]);
            float fwd = vrv_dot(t->handVel[h], f);
            if (speed > 2.2f && fwd > 1.6f) {
                sPunchTicks = 2;
                sPunchCooldown = 0.35f;
                vr_haptic(h, 0.7f, 0.08f);
                break;
            }
        }
    }
}

// Test autopilot (VR_AUTOPILOT=1): press Start through the title / level select screens,
// then optionally walk forward (VR_AUTOPILOT_WALK=ticks) so a run can be checked hands-free.
static void autopilot(OSContPad *pad) {
    static int ticks, levelTicks;
    if (getenv("VR_AUTOPILOT") == NULL) return;
    if (gMarioObject == NULL) {
        levelTicks = 0;
        if ((ticks++ % 45) < 3) pad->button |= START_BUTTON;
        return;
    }
    levelTicks++;
    int walk = (int) env_or("VR_AUTOPILOT_WALK", 0);
    if (levelTicks > 60 && levelTicks < 60 + walk) pad->stick_y = 64;
}

void vr_game_read_pad(OSContPad *pad) {
    if (!vr_enabled()) return;
    struct VrTracking *t = &gVrTrack;
    autopilot(pad);
    if (sLatchJump || t->jump) pad->button |= A_BUTTON;
    bool rTrig = t->trigger[VR_HAND_RIGHT] > 0.6f && !sUiOwnsTrigger[VR_HAND_RIGHT];
    bool lTrig = t->trigger[VR_HAND_LEFT] > 0.5f && !sUiOwnsTrigger[VR_HAND_LEFT];
    if (sLatchAttack || t->attack || rTrig || sPunchTicks > 0) pad->button |= B_BUTTON;
    if (sLatchZ || lTrig) pad->button |= Z_TRIG;
    if (sLatchStart || t->start || sUiStartTicks > 0) pad->button |= START_BUTTON;
    if (sUiStartTicks > 0) sUiStartTicks--;
    if (sPunchTicks > 0) sPunchTicks--;
    sLatchJump = sLatchAttack = sLatchStart = sLatchZ = false;

    if (sLastMode != VR_VIEW_FIRST) {
        if (t->turn[0] > 0.7f) pad->button |= R_CBUTTONS;
        if (t->turn[0] < -0.7f) pad->button |= L_CBUTTONS;
        if (t->turn[1] > 0.7f) pad->button |= U_CBUTTONS;
        if (t->turn[1] < -0.7f) pad->button |= D_CBUTTONS;
    }

    float x = t->move[0], y = t->move[1];
    if (x * x + y * y > 0.01f) {
        int sx = (int) (x * 80.0f), sy = (int) (y * 80.0f);
        if (sx > 80) sx = 80;
        if (sx < -80) sx = -80;
        if (sy > 80) sy = 80;
        if (sy < -80) sy = -80;
        pad->stick_x = sx;
        pad->stick_y = sy;
    }
}

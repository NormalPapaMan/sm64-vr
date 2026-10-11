// OpenXR runtime glue for SM64 VR: instance/session, per-eye swapchains, controller actions,
// desktop mirror. Also a simulator mode (--vrsim) that renders both eyes into the window with
// synthetic head/hand poses, so the stereo path can be tested without a headset.

#ifdef _WIN32
#include <windows.h>
#endif
#define GLEW_STATIC
#include <GL/glew.h>
#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define XR_NO_PROTOTYPES
#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "vr.h"
#include "vr_math.h"

struct VrTracking gVrTrack;
struct VrFrameInfo gVrFrameBuild;
struct VrFrameInfo gVrFrameCommitted;
float gVrHeadCenter[3] = { 0, 1.6f, 0 };

static float sSkyMarker[4][4];
static float sFullviewProj[4][4];
void *gVrSkyProjMarker = sSkyMarker;
void *gVrFullviewProj = sFullviewProj;

static enum VrMode sMode = VR_MODE_OFF;
static bool sExitRequested;

#define VR_LOG(...) do { fprintf(stderr, "[vr] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

void vr_markers_init(void) {
    // Contents only matter for the fullview matrix (320x240 ortho, same as the game's guOrtho).
    vrm_identity(sSkyMarker);
    memset(sFullviewProj, 0, sizeof(sFullviewProj));
    const float l = 0, r = 320, b = 0, t = 240, n = -10, f = 10;
    sFullviewProj[0][0] = 2.0f / (r - l);
    sFullviewProj[1][1] = 2.0f / (t - b);
    sFullviewProj[2][2] = -2.0f / (f - n);
    sFullviewProj[3][0] = -(r + l) / (r - l);
    sFullviewProj[3][1] = -(t + b) / (t - b);
    sFullviewProj[3][2] = -(f + n) / (f - n);
    sFullviewProj[3][3] = 1.0f;
}

enum VrMode vr_mode(void) {
    return sMode;
}

bool vr_exit_requested(void) {
    return sExitRequested;
}

// ---------------------------------------------------------------------------------------------
// Shared GL targets (swapchain images for OpenXR, plain textures for the simulator)
// ---------------------------------------------------------------------------------------------

static uint32_t sEyeW, sEyeH;
static GLuint sFbo[2], sDepth[2];
static GLuint sSimTex[2];
static GLuint sBoundTex[2];

static void create_eye_targets(void) {
    glGenFramebuffers(2, sFbo);
    glGenRenderbuffers(2, sDepth);
    for (int i = 0; i < 2; i++) {
        glBindRenderbuffer(GL_RENDERBUFFER, sDepth[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, sEyeW, sEyeH);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

static void bind_eye_texture(int eye, GLuint tex) {
    sBoundTex[eye] = tex;
    glBindFramebuffer(GL_FRAMEBUFFER, sFbo[eye]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sDepth[eye]);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        static int warned;
        if (!warned++) VR_LOG("eye framebuffer incomplete: 0x%x", st);
    }
    glViewport(0, 0, sEyeW, sEyeH);
}

// Copies the rendered eye(s) to the desktop window and presents it.
static void mirror_to_window(bool bothEyes) {
    SDL_Window *wnd = SDL_GL_GetCurrentWindow();
    int ww = 0, wh = 0;
    if (wnd == NULL) return;
    SDL_GL_GetDrawableSize(wnd, &ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int eyes = bothEyes ? 2 : 1;
    for (int e = 0; e < eyes; e++) {
        // Fit each eye (square-ish) into its slot, cropping the source to the slot's aspect.
        int slotW = ww / eyes, slotH = wh;
        float slotAspect = (float) slotW / (float) slotH;
        float srcAspect = (float) sEyeW / (float) sEyeH;
        int sx0 = 0, sy0 = 0, sx1 = sEyeW, sy1 = sEyeH;
        if (srcAspect > slotAspect) {
            int cw = (int) (sEyeH * slotAspect);
            sx0 = (sEyeW - cw) / 2; sx1 = sx0 + cw;
        } else {
            int ch = (int) (sEyeW / slotAspect);
            sy0 = (sEyeH - ch) / 2; sy1 = sy0 + ch;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sFbo[e]);
        glBlitFramebuffer(sx0, sy0, sx1, sy1, slotW * e, 0, slotW * (e + 1), slotH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    SDL_GL_SwapWindow(wnd);
}

void vr_eye_size(uint32_t *w, uint32_t *h) {
    *w = sEyeW;
    *h = sEyeH;
}

// ---------------------------------------------------------------------------------------------
// Simulator
// ---------------------------------------------------------------------------------------------

static uint64_t sSimLast;
static double sSimTime;

static float env_float(const char *name, float def) {
    const char *s = getenv(name);
    return s ? (float) atof(s) : def;
}

static void quat_from_yaw_pitch(float q[4], float yaw, float pitch) {
    // q = qYaw * qPitch
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    q[0] = cy * sp;
    q[1] = sy * cp;
    q[2] = -sy * sp;
    q[3] = cy * cp;
}

static bool sim_init(void) {
    sEyeW = 1024;
    sEyeH = 1024;
    create_eye_targets();
    glGenTextures(2, sSimTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, sSimTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sEyeW, sEyeH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    sSimLast = SDL_GetPerformanceCounter();
    VR_LOG("simulator mode: %ux%u per eye", sEyeW, sEyeH);
    return true;
}

static void sim_update_poses(void) {
    // Head: standing, yaw/pitch from env so screenshots are reproducible.
    float yaw = env_float("VR_SIM_YAW", 0) * (float) M_PI / 180.0f;
    // VR_SIM_TURN degrees of extra body turn after VR_SIM_TURN_AT seconds (tests body following)
    if (sSimTime > env_float("VR_SIM_TURN_AT", 1e9f)) yaw += env_float("VR_SIM_TURN", 0) * (float) M_PI / 180.0f;
    float pitch = env_float("VR_SIM_PITCH", 0) * (float) M_PI / 180.0f;
    struct VrTracking *t = &gVrTrack;
    t->hmdValid = true;
    quat_from_yaw_pitch(t->head.quat, yaw, pitch);
    t->head.pos[0] = 0; t->head.pos[1] = 1.6f; t->head.pos[2] = 0;
    VrMat hm;
    vrm_from_pose(hm, t->head.quat, t->head.pos);
    for (int e = 0; e < 2; e++) {
        float off[3] = { e == 0 ? -0.032f : 0.032f, 0, 0 };
        memcpy(t->eye[e].quat, t->head.quat, sizeof(t->head.quat));
        vrm_point(t->eye[e].pos, off, hm);
        t->fov[e][0] = -1.0f; t->fov[e][1] = 1.0f; t->fov[e][2] = 1.0f; t->fov[e][3] = -1.0f;
    }
    // Hands: held out in front, gently waving (relative to body yaw 0).
    float wave = sinf((float) sSimTime * 2.0f) * 0.12f;
    float handPos[2][3] = {
        { -0.22f, 1.25f + wave, -0.40f },
        { 0.24f, 1.30f - wave, -0.45f },
    };
    float reach = env_float("VR_SIM_REACH", 1.0f); // >1 pushes the hands out to full arm's length
    handPos[0][2] *= reach;
    handPos[1][2] *= reach;
    for (int h = 0; h < 2; h++) {
        t->handValid[h] = true;
        quat_from_yaw_pitch(t->grip[h].quat, 0, 0);
        memcpy(t->grip[h].pos, handPos[h], sizeof(handPos[h]));
        t->handVel[h][0] = t->handVel[h][1] = t->handVel[h][2] = 0;
        // aim ray: VR_SIM_AIM_YAW / VR_SIM_AIM_PITCH (degrees) point the laser
        t->aimValid[h] = true;
        quat_from_yaw_pitch(t->aim[h].quat, env_float("VR_SIM_AIM_YAW", 0) * (float) M_PI / 180.0f,
                            env_float("VR_SIM_AIM_PITCH", 0) * (float) M_PI / 180.0f);
        memcpy(t->aim[h].pos, handPos[h], sizeof(handPos[h]));
    }
    // VR_SIM_CLICK=1: pull the right trigger for 0.2 s every 4 s
    t->trigger[VR_HAND_RIGHT] = (getenv("VR_SIM_CLICK") && fmod(sSimTime, 4.0) > 3.8) ? 1.0f : 0.0f;
}

// ---------------------------------------------------------------------------------------------
// OpenXR
// ---------------------------------------------------------------------------------------------

#define XR_FUNCS(X) \
    X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem) \
    X(xrGetInstanceProperties) X(xrPollEvent) X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) \
    X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) \
    X(xrEnumerateViewConfigurationViews) X(xrCreateReferenceSpace) X(xrCreateActionSpace) X(xrLocateSpace) \
    X(xrCreateSwapchain) X(xrEnumerateSwapchainFormats) X(xrEnumerateSwapchainImages) \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrStringToPath) \
    X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) \
    X(xrGetActionStateVector2f) X(xrGetActionStatePose) X(xrApplyHapticFeedback) X(xrResultToString)

#define DECLARE_FN(f) static PFN_##f p##f;
XR_FUNCS(DECLARE_FN)
static PFN_xrGetInstanceProcAddr pxrGetInstanceProcAddr;
static PFN_xrGetOpenGLGraphicsRequirementsKHR pxrGetOpenGLGraphicsRequirementsKHR;

static XrInstance sInst;
static XrSystemId sSystem;
static XrSession sSession;
static XrSpace sLocalSpace, sViewSpace;
static XrSessionState sState = XR_SESSION_STATE_UNKNOWN;
static bool sRunning;
static XrSwapchain sSwapchain[2];
static XrSwapchainImageOpenGLKHR *sSwapImages[2];
static uint32_t sSwapImageCount[2];
static bool sAcquired[2];
static XrView sViews[2];
static XrFrameState sFrameState;
static XrTime sLastDisplayTime;

static XrActionSet sActionSet;
static XrAction aAim, aPose, aTrigger, aSqueeze, aMove, aTurn, aJump, aAttack, aStart, aView, aRecenter, aHaptic;
static XrPath sHandPath[2];
static XrSpace sGripSpace[2], sAimSpace[2];

static bool xr_check(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (pxrResultToString && sInst != XR_NULL_HANDLE) pxrResultToString(sInst, r, buf);
    VR_LOG("%s failed: %s (%d)", what, buf, (int) r);
    return false;
}

static XrPath xr_path(const char *s) {
    XrPath p = XR_NULL_PATH;
    pxrStringToPath(sInst, s, &p);
    return p;
}

static XrAction make_action(XrActionType type, const char *name, const char *loc, bool perHand) {
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    ci.actionType = type;
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, loc);
    if (perHand) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = sHandPath;
    }
    XrAction a = XR_NULL_HANDLE;
    xr_check(pxrCreateAction(sActionSet, &ci, &a), name);
    return a;
}

struct Bind { XrAction *action; const char *path; };

static void suggest(const char *profile, const struct Bind *b, int n) {
    XrActionSuggestedBinding sb[32];
    int count = 0;
    for (int i = 0; i < n && count < 32; i++) {
        XrPath p = xr_path(b[i].path);
        if (p == XR_NULL_PATH || *b[i].action == XR_NULL_HANDLE) continue;
        sb[count].action = *b[i].action;
        sb[count].binding = p;
        count++;
    }
    XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    s.interactionProfile = xr_path(profile);
    s.countSuggestedBindings = count;
    s.suggestedBindings = sb;
    XrResult r = pxrSuggestInteractionProfileBindings(sInst, &s);
    if (XR_FAILED(r)) VR_LOG("bindings for %s rejected (%d)", profile, (int) r);
}

#define L "/user/hand/left"
#define R "/user/hand/right"

static void setup_actions(void) {
    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(asci.actionSetName, "gameplay");
    strcpy(asci.localizedActionSetName, "Gameplay");
    xr_check(pxrCreateActionSet(sInst, &asci, &sActionSet), "xrCreateActionSet");
    sHandPath[0] = xr_path(L);
    sHandPath[1] = xr_path(R);

    aPose = make_action(XR_ACTION_TYPE_POSE_INPUT, "hand_pose", "Hand pose", true);
    aAim = make_action(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Pointer", true);
    aTrigger = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger (L: crouch / Z, R: attack / B)", true);
    aSqueeze = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip (make a fist)", true);
    aMove = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move", false);
    aTurn = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Snap turn", false);
    aJump = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "jump", "Jump (A)", false);
    aAttack = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "attack", "Attack (B)", false);
    aStart = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "start", "Pause (Start)", false);
    aView = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "view_toggle", "First / third person", false);
    aRecenter = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "recenter", "Recenter", false);
    aHaptic = make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Rumble", true);

    const struct Bind touch[] = {
        { &aPose, L "/input/grip/pose" }, { &aPose, R "/input/grip/pose" },
        { &aAim, L "/input/aim/pose" }, { &aAim, R "/input/aim/pose" },
        { &aTrigger, L "/input/trigger/value" }, { &aTrigger, R "/input/trigger/value" },
        { &aSqueeze, L "/input/squeeze/value" }, { &aSqueeze, R "/input/squeeze/value" },
        { &aMove, L "/input/thumbstick" }, { &aTurn, R "/input/thumbstick" },
        { &aJump, R "/input/a/click" }, { &aAttack, R "/input/b/click" },
        { &aStart, L "/input/menu/click" }, { &aView, L "/input/y/click" },
        { &aRecenter, L "/input/x/click" }, { &aRecenter, L "/input/thumbstick/click" },
        { &aHaptic, L "/output/haptic" }, { &aHaptic, R "/output/haptic" },
    };
    suggest("/interaction_profiles/oculus/touch_controller", touch, sizeof(touch) / sizeof(touch[0]));

    const struct Bind index[] = {
        { &aPose, L "/input/grip/pose" }, { &aPose, R "/input/grip/pose" },
        { &aAim, L "/input/aim/pose" }, { &aAim, R "/input/aim/pose" },
        { &aTrigger, L "/input/trigger/value" }, { &aTrigger, R "/input/trigger/value" },
        { &aSqueeze, L "/input/squeeze/value" }, { &aSqueeze, R "/input/squeeze/value" },
        { &aMove, L "/input/thumbstick" }, { &aTurn, R "/input/thumbstick" },
        { &aJump, R "/input/a/click" }, { &aAttack, R "/input/b/click" },
        { &aStart, L "/input/b/click" }, { &aView, L "/input/a/click" },
        { &aRecenter, L "/input/thumbstick/click" },
        { &aHaptic, L "/output/haptic" }, { &aHaptic, R "/output/haptic" },
    };
    suggest("/interaction_profiles/valve/index_controller", index, sizeof(index) / sizeof(index[0]));

    const struct Bind vive[] = {
        { &aPose, L "/input/grip/pose" }, { &aPose, R "/input/grip/pose" },
        { &aAim, L "/input/aim/pose" }, { &aAim, R "/input/aim/pose" },
        { &aTrigger, L "/input/trigger/value" }, { &aTrigger, R "/input/trigger/value" },
        { &aSqueeze, L "/input/squeeze/click" }, { &aSqueeze, R "/input/squeeze/click" },
        { &aMove, L "/input/trackpad" }, { &aJump, R "/input/trackpad/click" },
        { &aStart, L "/input/menu/click" }, { &aView, R "/input/menu/click" },
        { &aHaptic, L "/output/haptic" }, { &aHaptic, R "/output/haptic" },
    };
    suggest("/interaction_profiles/htc/vive_controller", vive, sizeof(vive) / sizeof(vive[0]));

    const struct Bind wmr[] = {
        { &aPose, L "/input/grip/pose" }, { &aPose, R "/input/grip/pose" },
        { &aAim, L "/input/aim/pose" }, { &aAim, R "/input/aim/pose" },
        { &aTrigger, L "/input/trigger/value" }, { &aTrigger, R "/input/trigger/value" },
        { &aSqueeze, L "/input/squeeze/click" }, { &aSqueeze, R "/input/squeeze/click" },
        { &aMove, L "/input/thumbstick" }, { &aTurn, R "/input/thumbstick" },
        { &aJump, R "/input/trackpad/click" }, { &aAttack, L "/input/trackpad/click" },
        { &aStart, L "/input/menu/click" }, { &aView, R "/input/menu/click" },
        { &aRecenter, L "/input/thumbstick/click" },
        { &aHaptic, L "/output/haptic" }, { &aHaptic, R "/output/haptic" },
    };
    suggest("/interaction_profiles/microsoft/motion_controller", wmr, sizeof(wmr) / sizeof(wmr[0]));

    const struct Bind simple[] = {
        { &aPose, L "/input/grip/pose" }, { &aPose, R "/input/grip/pose" },
        { &aAim, L "/input/aim/pose" }, { &aAim, R "/input/aim/pose" },
        { &aJump, R "/input/select/click" }, { &aAttack, L "/input/select/click" },
        { &aStart, L "/input/menu/click" }, { &aView, R "/input/menu/click" },
        { &aHaptic, L "/output/haptic" }, { &aHaptic, R "/output/haptic" },
    };
    suggest("/interaction_profiles/khr/simple_controller", simple, sizeof(simple) / sizeof(simple[0]));

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.action = aPose;
        sci.subactionPath = sHandPath[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        xr_check(pxrCreateActionSpace(sSession, &sci, &sGripSpace[h]), "xrCreateActionSpace");
        sci.action = aAim;
        xr_check(pxrCreateActionSpace(sSession, &sci, &sAimSpace[h]), "xrCreateActionSpace (aim)");
    }

    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &sActionSet;
    xr_check(pxrAttachSessionActionSets(sSession, &ai), "xrAttachSessionActionSets");
}

#undef L
#undef R

static bool xr_load_loader(void) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("openxr_loader.dll");
    if (lib == NULL) {
        VR_LOG("openxr_loader.dll not found next to the executable");
        return false;
    }
    pxrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr) (void *) GetProcAddress(lib, "xrGetInstanceProcAddr");
#endif
    if (pxrGetInstanceProcAddr == NULL) return false;
    pxrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                           (PFN_xrVoidFunction *) &pxrEnumerateInstanceExtensionProperties);
    pxrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *) &pxrCreateInstance);
    return pxrEnumerateInstanceExtensionProperties && pxrCreateInstance;
}

static bool xr_init(void) {
    if (!xr_load_loader()) return false;

    uint32_t extCount = 0;
    if (!xr_check(pxrEnumerateInstanceExtensionProperties(NULL, 0, &extCount, NULL), "enumerate extensions")) {
        VR_LOG("no OpenXR runtime available (is SteamVR / Oculus / Virtual Desktop running?)");
        return false;
    }
    XrExtensionProperties *exts = calloc(extCount, sizeof(*exts));
    for (uint32_t i = 0; i < extCount; i++) exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    pxrEnumerateInstanceExtensionProperties(NULL, extCount, &extCount, exts);
    bool hasGL = false;
    for (uint32_t i = 0; i < extCount; i++) {
        if (strcmp(exts[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0) hasGL = true;
    }
    free(exts);
    if (!hasGL) {
        VR_LOG("runtime has no " XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
        return false;
    }

    const char *enabled[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "SM64 VR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy(ici.applicationInfo.engineName, "sm64ex");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    if (!xr_check(pxrCreateInstance(&ici, &sInst), "xrCreateInstance")) return false;

#define LOAD_FN(f) pxrGetInstanceProcAddr(sInst, #f, (PFN_xrVoidFunction *) &p##f);
    XR_FUNCS(LOAD_FN)
#undef LOAD_FN
    pxrGetInstanceProcAddr(sInst, "xrGetOpenGLGraphicsRequirementsKHR",
                           (PFN_xrVoidFunction *) &pxrGetOpenGLGraphicsRequirementsKHR);

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(pxrGetInstanceProperties(sInst, &ip))) VR_LOG("runtime: %s", ip.runtimeName);

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xr_check(pxrGetSystem(sInst, &sgi, &sSystem), "xrGetSystem (is the headset connected?)")) return false;

    XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!xr_check(pxrGetOpenGLGraphicsRequirementsKHR(sInst, sSystem, &req), "GL requirements")) return false;

#ifdef _WIN32
    XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    gb.hDC = wglGetCurrentDC();
    gb.hGLRC = wglGetCurrentContext();
#else
#error OpenXR binding only implemented for Windows
#endif
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb;
    sci.systemId = sSystem;
    if (!xr_check(pxrCreateSession(sInst, &sci, &sSession), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo rci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rci.poseInReferenceSpace.orientation.w = 1.0f;
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    xr_check(pxrCreateReferenceSpace(sSession, &rci, &sLocalSpace), "LOCAL space");
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xr_check(pxrCreateReferenceSpace(sSession, &rci, &sViewSpace), "VIEW space");

    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t nViews = 0;
    if (!xr_check(pxrEnumerateViewConfigurationViews(sInst, sSystem, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                    &nViews, vcv), "view configuration")) return false;
    sEyeW = vcv[0].recommendedImageRectWidth;
    sEyeH = vcv[0].recommendedImageRectHeight;
    float ss = env_float("VR_SUPERSAMPLE", 1.0f);
    if (ss > 0.25f && ss <= 2.0f) {
        sEyeW = (uint32_t) (sEyeW * ss);
        sEyeH = (uint32_t) (sEyeH * ss);
    }

    int64_t formats[64];
    uint32_t nFormats = 0;
    pxrEnumerateSwapchainFormats(sSession, 64, &nFormats, formats);
    int64_t fmt = 0;
    for (uint32_t i = 0; i < nFormats && !fmt; i++) if (formats[i] == GL_SRGB8_ALPHA8) fmt = GL_SRGB8_ALPHA8;
    for (uint32_t i = 0; i < nFormats && !fmt; i++) if (formats[i] == GL_RGBA8) fmt = GL_RGBA8;
    if (!fmt && nFormats) fmt = formats[0];
    VR_LOG("eye %ux%u, swapchain format 0x%llx", sEyeW, sEyeH, (unsigned long long) fmt);

    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = fmt;
        ci.sampleCount = 1;
        ci.width = sEyeW;
        ci.height = sEyeH;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!xr_check(pxrCreateSwapchain(sSession, &ci, &sSwapchain[e]), "xrCreateSwapchain")) return false;
        pxrEnumerateSwapchainImages(sSwapchain[e], 0, &sSwapImageCount[e], NULL);
        sSwapImages[e] = calloc(sSwapImageCount[e], sizeof(XrSwapchainImageOpenGLKHR));
        for (uint32_t i = 0; i < sSwapImageCount[e]; i++) sSwapImages[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        pxrEnumerateSwapchainImages(sSwapchain[e], sSwapImageCount[e], &sSwapImageCount[e],
                                    (XrSwapchainImageBaseHeader *) sSwapImages[e]);
    }
    create_eye_targets();
    setup_actions();
    return true;
}

static void xr_poll_events(void) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (pxrPollEvent(sInst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *e = (XrEventDataSessionStateChanged *) &ev;
            sState = e->state;
            VR_LOG("session state %d", (int) sState);
            if (sState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                sRunning = xr_check(pxrBeginSession(sSession, &bi), "xrBeginSession");
            } else if (sState == XR_SESSION_STATE_STOPPING) {
                pxrEndSession(sSession);
                sRunning = false;
            } else if (sState == XR_SESSION_STATE_EXITING || sState == XR_SESSION_STATE_LOSS_PENDING) {
                sRunning = false;
                sExitRequested = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            sExitRequested = true;
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
    }
}

static void copy_pose(VrPose *dst, const XrPosef *src) {
    dst->quat[0] = src->orientation.x;
    dst->quat[1] = src->orientation.y;
    dst->quat[2] = src->orientation.z;
    dst->quat[3] = src->orientation.w;
    dst->pos[0] = src->position.x;
    dst->pos[1] = src->position.y;
    dst->pos[2] = src->position.z;
}

static bool get_bool(XrAction a) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    gi.action = a;
    return XR_SUCCEEDED(pxrGetActionStateBoolean(sSession, &gi, &st)) && st.isActive && st.currentState;
}

static float get_float(XrAction a, int hand) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    gi.action = a;
    gi.subactionPath = sHandPath[hand];
    return (XR_SUCCEEDED(pxrGetActionStateFloat(sSession, &gi, &st)) && st.isActive) ? st.currentState : 0.0f;
}

static void get_vec2(XrAction a, float out[2]) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    gi.action = a;
    out[0] = out[1] = 0;
    if (XR_SUCCEEDED(pxrGetActionStateVector2f(sSession, &gi, &st)) && st.isActive) {
        out[0] = st.currentState.x;
        out[1] = st.currentState.y;
    }
}

static void xr_update_tracking(XrTime t) {
    struct VrTracking *tr = &gVrTrack;

    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = sLocalSpace;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    uint32_t n = 0;
    sViews[0].type = sViews[1].type = XR_TYPE_VIEW;
    if (XR_SUCCEEDED(pxrLocateViews(sSession, &li, &vs, 2, &n, sViews)) &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        for (int e = 0; e < 2; e++) {
            copy_pose(&tr->eye[e], &sViews[e].pose);
            tr->fov[e][0] = tanf(sViews[e].fov.angleLeft);
            tr->fov[e][1] = tanf(sViews[e].fov.angleRight);
            tr->fov[e][2] = tanf(sViews[e].fov.angleUp);
            tr->fov[e][3] = tanf(sViews[e].fov.angleDown);
        }
        tr->hmdValid = true;
    }
    XrSpaceLocation hl = { XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(pxrLocateSpace(sViewSpace, sLocalSpace, t, &hl)) &&
        (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        copy_pose(&tr->head, &hl.pose);
    }

    XrActiveActionSet aas = { sActionSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    pxrSyncActions(sSession, &si);

    for (int h = 0; h < 2; h++) {
        XrSpaceVelocity vel = { XR_TYPE_SPACE_VELOCITY };
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        loc.next = &vel;
        tr->handValid[h] = false;
        if (XR_SUCCEEDED(pxrLocateSpace(sGripSpace[h], sLocalSpace, t, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            copy_pose(&tr->grip[h], &loc.pose);
            tr->handValid[h] = true;
            if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
                tr->handVel[h][0] = vel.linearVelocity.x;
                tr->handVel[h][1] = vel.linearVelocity.y;
                tr->handVel[h][2] = vel.linearVelocity.z;
            }
        }
        XrSpaceLocation aim = { XR_TYPE_SPACE_LOCATION };
        tr->aimValid[h] = false;
        if (XR_SUCCEEDED(pxrLocateSpace(sAimSpace[h], sLocalSpace, t, &aim)) &&
            (aim.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (aim.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            copy_pose(&tr->aim[h], &aim.pose);
            tr->aimValid[h] = true;
        } else if (tr->handValid[h]) {
            tr->aim[h] = tr->grip[h];
            tr->aimValid[h] = true;
        }
        tr->trigger[h] = get_float(aTrigger, h);
        tr->squeeze[h] = get_float(aSqueeze, h);
    }
    get_vec2(aMove, tr->move);
    get_vec2(aTurn, tr->turn);
    tr->jump = get_bool(aJump);
    tr->attack = get_bool(aAttack);
    tr->start = get_bool(aStart);
    tr->viewToggle = get_bool(aView);
    tr->recenter = get_bool(aRecenter);
}

void vr_haptic(int hand, float amplitude, float seconds) {
    if (sMode != VR_MODE_OPENXR || !sRunning) return;
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    v.amplitude = amplitude;
    v.duration = (XrDuration) (seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = aHaptic;
    hi.subactionPath = sHandPath[hand];
    pxrApplyHapticFeedback(sSession, &hi, (XrHapticBaseHeader *) &v);
}

// ---------------------------------------------------------------------------------------------
// Public frame API
// ---------------------------------------------------------------------------------------------

bool vr_init(enum VrMode mode) {
    sMode = VR_MODE_OFF;
    if (mode == VR_MODE_OFF) return false;
    if (glGenFramebuffers == NULL || glBlitFramebuffer == NULL) {
        VR_LOG("GL framebuffer objects unavailable");
        return false;
    }
    bool ok = (mode == VR_MODE_SIM) ? sim_init() : xr_init();
    if (!ok) {
        VR_LOG("VR init failed, running on the desktop");
        return false;
    }
    sMode = mode;
    SDL_GL_SetSwapInterval(0); // the runtime paces frames, not the monitor
    return true;
}

bool vr_frame_begin(double *dt, bool *shouldRender) {
    *dt = 0;
    *shouldRender = false;
    if (sMode == VR_MODE_SIM) {
        // pace to ~72 Hz
        uint64_t freq = SDL_GetPerformanceFrequency();
        uint64_t target = sSimLast + freq / 72;
        uint64_t now = SDL_GetPerformanceCounter();
        if (now < target) SDL_Delay((Uint32) ((target - now) * 1000 / freq));
        now = SDL_GetPerformanceCounter();
        *dt = (double) (now - sSimLast) / (double) freq;
        sSimLast = now;
        sSimTime += *dt;
        sim_update_poses();
        *shouldRender = true;
        return true;
    }

    xr_poll_events();
    if (!sRunning) {
        SDL_Delay(10);
        return false;
    }
    sFrameState.type = XR_TYPE_FRAME_STATE;
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    if (!xr_check(pxrWaitFrame(sSession, &wi, &sFrameState), "xrWaitFrame")) return false;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    if (!xr_check(pxrBeginFrame(sSession, &bi), "xrBeginFrame")) return false;

    if (sLastDisplayTime != 0) *dt = (double) (sFrameState.predictedDisplayTime - sLastDisplayTime) * 1e-9;
    sLastDisplayTime = sFrameState.predictedDisplayTime;
    xr_update_tracking(sFrameState.predictedDisplayTime);
    *shouldRender = sFrameState.shouldRender && gVrTrack.hmdValid;
    return true;
}

void vr_eye_bind(int eye) {
    if (sMode == VR_MODE_SIM) {
        bind_eye_texture(eye, sSimTex[eye]);
        return;
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    pxrAcquireSwapchainImage(sSwapchain[eye], &ai, &idx);
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    pxrWaitSwapchainImage(sSwapchain[eye], &wi);
    sAcquired[eye] = true;
    bind_eye_texture(eye, sSwapImages[eye][idx].image);
}

void vr_frame_end(bool rendered) {
    if (sMode == VR_MODE_SIM) {
        mirror_to_window(true);
        return;
    }
    if (rendered) mirror_to_window(false);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int e = 0; e < 2; e++) {
        if (sAcquired[e]) {
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            pxrReleaseSwapchainImage(sSwapchain[e], &ri);
            sAcquired[e] = false;
        }
    }

    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const XrCompositionLayerBaseHeader *layers[1] = { (const XrCompositionLayerBaseHeader *) &layer };
    for (int e = 0; e < 2; e++) {
        memset(&pv[e], 0, sizeof(pv[e]));
        pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        pv[e].pose = sViews[e].pose;
        pv[e].fov = sViews[e].fov;
        pv[e].subImage.swapchain = sSwapchain[e];
        pv[e].subImage.imageRect.extent.width = sEyeW;
        pv[e].subImage.imageRect.extent.height = sEyeH;
    }
    layer.space = sLocalSpace;
    layer.viewCount = 2;
    layer.views = pv;

    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = sFrameState.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = rendered ? 1 : 0;
    ei.layers = layers;
    xr_check(pxrEndFrame(sSession, &ei), "xrEndFrame");
}

void vr_shutdown(void) {
    if (sMode == VR_MODE_OPENXR && sInst != XR_NULL_HANDLE) {
        if (sSession != XR_NULL_HANDLE) pxrDestroySession(sSession);
        pxrDestroyInstance(sInst);
        sInst = XR_NULL_HANDLE;
        sSession = XR_NULL_HANDLE;
    }
    sMode = VR_MODE_OFF;
}

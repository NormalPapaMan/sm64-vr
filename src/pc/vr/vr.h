#ifndef VR_H
#define VR_H

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------------------------
// Runtime (OpenXR or the desktop simulator). Everything here is in tracking space, meters.
// ---------------------------------------------------------------------------------------------

enum VrMode {
    VR_MODE_OFF,
    VR_MODE_OPENXR,
    VR_MODE_SIM, // no headset: synthetic poses, both eyes shown side by side in the window
};

enum VrHand { VR_HAND_LEFT, VR_HAND_RIGHT };

typedef struct {
    float quat[4]; // x, y, z, w
    float pos[3];
} VrPose;

struct VrTracking {
    bool hmdValid;
    VrPose head;         // center eye
    VrPose eye[2];
    float fov[2][4];     // tan of left, right, up, down half-angles (left/down negative)

    bool handValid[2];
    VrPose grip[2];
    bool aimValid[2];
    VrPose aim[2];       // pointing ray (-Z forward)
    float handVel[2][3];

    // controls (already mapped to game semantics by the runtime's binding profiles)
    float move[2];       // left stick
    float turn[2];       // right stick
    float trigger[2];
    float squeeze[2];
    bool jump;           // A
    bool attack;         // B
    bool start;
    bool viewToggle;
    bool recenter;
};

extern struct VrTracking gVrTrack;

bool vr_init(enum VrMode mode);
enum VrMode vr_mode(void);
static inline bool vr_enabled(void) { return vr_mode() != VR_MODE_OFF; }
void vr_shutdown(void);

// Pumps runtime events and waits for the next frame. Returns false when nothing should be drawn
// this iteration (session not running yet); *dt is the time to advance the game by.
bool vr_frame_begin(double *dt, bool *shouldRender);
void vr_eye_size(uint32_t *w, uint32_t *h);
void vr_eye_bind(int eye);
void vr_frame_end(bool rendered);
bool vr_exit_requested(void);

void vr_haptic(int hand, float amplitude, float seconds);

// ---------------------------------------------------------------------------------------------
// Per-display-list frame info, written by the game while it builds a frame and consumed by
// gfx_pc every time that display list is (re)rendered for an eye.
// ---------------------------------------------------------------------------------------------

enum VrViewMode {
    VR_VIEW_NONE,        // no camera this frame (title, menus): game view space == room
    VR_VIEW_THIRD,       // stand at the game camera, head rotation on top
    VR_VIEW_FIRST,       // Mario's eyes
    VR_VIEW_SCREEN,      // title / menus: the original 2D picture on a floating screen
};

struct VrFrameInfo {
    enum VrViewMode mode;
    bool haveCamera;
    float viewGame[4][4];      // world -> game camera view, as written into the display list
    float anchorPrev[3];       // world position of the tracking origin at the previous tick
    float anchorCur[3];        // ... at this tick
    float yaw;                 // tracking -> world rotation about +Y (radians)
    float scale;               // world units per meter
    // Mario's matrices live in [marioMtxLo, marioMtxHi); they get the same interpolation offset
    // as the camera so the body doesn't stutter against the view.
    const void *marioMtxLo, *marioMtxHi;
};

extern struct VrFrameInfo gVrFrameBuild;     // being filled during the current game tick
extern struct VrFrameInfo gVrFrameCommitted; // belongs to the display list currently on screen

// Tracking-space point that maps onto the anchor (recenter / lean leash). Live, not per tick.
extern float gVrHeadCenter[3];

// Marker matrices the game loads as the projection to request special handling.
extern void *gVrSkyProjMarker;      // vertices are world-space directions around the eye
extern void *gVrFullviewProj;       // 320x240 ortho that must cover the whole view (fades)

// Fill the marker storage (call once, before the game builds its first frame).
void vr_markers_init(void);

// gfx_pc entry: render the committed display list for one eye. alpha = 0..1 between ticks.
void gfx_vr_render_eye(void *displayList, int eye, float alpha);
// gfx_pc entry: render a display list exactly like the desktop game into the bound framebuffer.
void gfx_vr_render_flat(void *displayList, uint32_t w, uint32_t h);

// ---------------------------------------------------------------------------------------------
// Floating panel (HUD / menu screen) and the on-panel START button, all in tracking space.
// The game side places them every frame; the renderer and overlay draw them.
// ---------------------------------------------------------------------------------------------

struct VrPanel {
    bool valid;
    float pos[3];        // center, meters, tracking space
    float yaw;           // panel faces the player along local +Z
    float halfW, halfH;  // meters
};

struct VrButton {
    bool visible;
    bool hovered;
    float cx, cy, hw, hh; // panel-local meters
};

extern struct VrPanel gVrPanel;
extern struct VrButton gVrStartButton;
extern bool gVrLaserActive[2];
extern float gVrLaserHit[2][3];

// Panel-local -> tracking space (row-vector rigid matrix).
void vr_panel_matrix(float out[4][4]);

// Overlay: menu screen capture, panel/button/laser drawing into the currently bound eye.
void vr_screen_capture(void *displayList);
void vr_overlay_draw(int eye, bool screenMode);

#endif

# SM64 VR — developer notes

## Base
- sm64pc/sm64ex @ d7ca2c04 (Dec 2024). The first commit of this repo is that tree, untouched, so
  the commit history shows exactly what the VR mod changes.
- Toolchain: MSYS2 MinGW64 with `make python mingw-w64-x86_64-{gcc,SDL2,glew,pkgconf}`.
- OpenXR: headers 1.1.43 in `openxr/include`, loader `openxr/bin/openxr_loader.dll`
  (Khronos OpenXR-SDK-Source release 1.1.43), copied next to the exe by the Makefile.
  The loader is loaded at runtime (`LoadLibrary`), so there is no link-time dependency.

## Architecture
- The game ticks at 30 Hz; VR renders at the headset's rate by replaying the *last* display list
  per eye with fresh head poses. sm64ex's gfx_pc transforms vertices on the CPU, so a display list
  can be replayed any number of times.
- Stereo without touching game code: gfx_pc classifies the effective projection matrix:
  - perspective → `inv(V_game) * V_eye * P_eye` (asymmetric OpenXR FOV, near 6 units, original far).
    Fog is still computed from the original projection.
  - ortho / identity (HUD, text, pause menu) → mapped onto a floating panel placed in the room.
  - pointer-identified marker matrices → VR skybox (sphere built from the original 32×32 tiles,
    rotation-only view) and full-view fades (screen wipes).
  - full-screen fill rects (background clear) stay full-view.
- Scenes without Mario (title, logo, file select, act select) switch to `VR_VIEW_SCREEN`: the
  display list is rendered exactly like the desktop game into a texture and shown on a
  room-fixed screen (`src/pc/vr/vr_overlay.c`).
- First person: the game camera sits at Mario's eye (head bone + offset, bob smoothed); head yaw
  drives the movement direction (`intendedYaw`). Mario's head is hidden by collapsing its rotation
  node. Standing still, Mario turns to follow the player's real yaw (starts at ~25°, stops at ~3°).
- Arms: two-bone IK on Mario's animated parts. Part order: 0 root, 1 butt, 2 torso, 3 head,
  4 L shoulder, 5 L upper arm, 6 L forearm, 7 L hand, 8 R shoulder, 9 R upper arm, 10 R forearm,
  11 R hand, 12+ legs. Bones run along local +X; upper arm 65 units, forearm 60. Arms stretch
  (scale along +X) when the target is out of reach; the glove is re-orthonormalized so it keeps
  its size; the wrist bends toward where the controller points (max ~70°).
- Smooth motion: the camera anchor and Mario's matrices (found by the gfx-pool address range
  allocated while Mario is traversed) are interpolated between the last two ticks.
- Input: OpenXR actions with suggested bindings for Touch, Index, Vive, WMR and the KHR simple
  controller, exposed to the game as an extra controller backend (`controller_vr.c`). The
  floating START button is hit-tested with the aim pose; a trigger pulled on it is consumed by
  the UI and not passed to the game as B / Z.

## Testing without a headset
- `--vrsim`: synthetic head/hands, both eyes side by side in the window.
  `VR_SIM_PITCH`, `VR_SIM_YAW`, `VR_SIM_TURN` + `VR_SIM_TURN_AT` (delayed turn),
  `VR_SIM_REACH` (hand distance), `VR_SIM_AIM_YAW` / `VR_SIM_AIM_PITCH` (laser), `VR_SIM_CLICK=1`
  (pulls the trigger every 4 s).
- Hands-free into a level: `VR_AUTOPILOT=1 VR_LEVEL_SELECT=1 VR_TEST_LEVEL=9` (Bob-omb
  Battlefield); `VR_AUTOPILOT_WALK=<ticks>` walks forward.
- Force a specific OpenXR runtime: `XR_RUNTIME_JSON=<path to runtime json>`.

## Gotchas
1. Projections are often built as LOAD identity + MUL ortho (`create_dl_ortho_matrix`), and the
   camera MULs a roll matrix into the projection. Classify the *effective* projection, not the op.
2. Level near planes are 50–100 units; in first person they clip Mario's arms. Use ~6 units for
   the eye projection, but compute fog with the game's original matrix or fog density changes.
3. `obj_is_in_view` culls by the game camera's frustum; with a 30 Hz camera and a 90 Hz head,
   objects pop at the edges. Disabled in VR.
4. Mario's matrices are found by address range: `alloc_display_list` grows downward from `gGfxPoolEnd`.
5. GLEW is static in this build: `#define GLEW_STATIC` before `#include <GL/glew.h>` in new files.
6. The game exports its own global `atan2f` (math_util.c, built on `atan2s` with a different
   argument meaning) and it replaces libm's at link time for *every* object file. Never call
   `atan2f` in VR code; use `vr_atan2`.
7. The overlay shares the GL context with gfx_pc: reset gfx_pc's cached shader / texture /
   viewport state before each game render (`gfx_vr_begin_target`) and push/pop GL state in the overlay.
8. The debug level select starts level #1 (a dummy) and loops back to the intro;
   `VR_TEST_LEVEL` preselects a real stage.

## Ideas
- Hand roll (twist) from the controller, optional smooth turn, comfort vignette when moving fast.
- Laser pointer driving the file-select cursor.

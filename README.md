# Super Mario 64 VR — full-body first person

Play Super Mario 64 from Mario's own eyes in VR. Look down and you see Mario's body; his arms
follow your real hands; his gloves make fists when you squeeze; throw a real punch and he punches.
The title screen and menus float in front of you on a big screen.

Built on the [sm64ex](https://github.com/sm64pc/sm64ex) PC port with a native OpenXR renderer —
works with **SteamVR, Meta Quest (Link / Air Link), Virtual Desktop, Pico, Index, Vive, WMR**.
Windows only for now.

> **You need your own Super Mario 64 (USA) ROM.** This repository contains no Nintendo assets and
> no ROM; the game's assets are extracted from your ROM when you build. Don't ask for ROMs here.

## Install (one click)

1. **Download** this repo: green **Code** button → **Download ZIP**, then unzip it somewhere
   (e.g. your Desktop). Or `git clone https://github.com/cooperdmendoza-prog/sm64-vr`.
2. **Double-click `setup.bat`.** It will:
   - install the free MSYS2 build tools if you don't have them (asks first, ~1.5 GB),
   - ask you to pick your ROM (`.z64`, `.n64`, `.v64`, or a `.zip` containing one) and check it's
     the unmodified USA version,
   - build the game (5–10 minutes the first time),
   - put **Super Mario 64 VR** and **Super Mario 64 (desktop)** shortcuts on your desktop.
3. Start SteamVR (or Oculus / Virtual Desktop), put on your headset and open **Super Mario 64 VR**.

No headset connected? It starts as the normal desktop game. `Test without headset.bat` runs the
VR renderer with a simulated headset so you can see both eyes on your monitor.

To update: download the new version (or `git pull`) and run `setup.bat` again.

## Controls

| Action | Control |
|---|---|
| Move (relative to where you look) | Left stick |
| Jump (A) | A |
| Punch / kick / grab (B) | B, right trigger, **or throw a real punch** |
| Crouch / ground pound (Z) | Left trigger |
| Start / pause | Point at the floating **START** button and pull the trigger (or poke it). The left menu button also works |
| Snap turn 30° | Right stick left/right |
| Recenter (also brings the menu screen back) | X or click the left stick |
| First / third person | Y |
| Make a fist | Grip |

Turn your real body and Mario turns with you. An Xbox controller or the keyboard also works
alongside the motion controllers. In third person, the right stick works Lakitu's C-buttons.

## Settings

Set these as environment variables (e.g. in a `.bat` before starting the game) — no rebuild needed.

| Variable | Default | Meaning |
|---|---|---|
| `VR_WORLD_SCALE` | 100 | game units per real meter (bigger = you feel smaller) |
| `VR_EYE_UP`, `VR_EYE_FWD` | 40, 22 | eye position relative to Mario's neck |
| `VR_ARM_IK` | 1 | 0 = keep Mario's animated arms |
| `VR_ARM_STRETCH_MAX` | 3.0 | how far Mario's arms may stretch to reach your hands (1 = never) |
| `VR_LEASH` | 0.30 | meters you can lean before the view pulls Mario along |
| `VR_FIRST_PERSON` | 1 | 0 = start in third person |
| `VR_HUD_DIST`, `VR_HUD_SCALE` | 1.6, 1.0 | distance (m) and size of the in-game HUD panel |
| `VR_SCREEN_DIST`, `VR_SCREEN_WIDTH` | 2.2, 2.0 | distance and width (m) of the menu screen |
| `VR_SUPERSAMPLE` | 1.0 | render resolution multiplier (0.5–2.0) |

Command line: `--novr` (desktop only), `--vrsim` (simulated headset), plus all sm64ex options.

## How it works

- The game runs at its original 30 Hz. Every headset frame (72–120 Hz) re-renders the last game
  frame from your current head pose, so looking around is always smooth; the camera and Mario's
  body are interpolated between game ticks.
- Stereo happens in the renderer: the game's perspective matrices become per-eye OpenXR
  projections, the HUD goes onto a floating panel, the skybox is rebuilt as a sphere from the
  original tiles, and screen wipes become full-view fades. Fog still uses the original projection.
- First person: the camera sits at Mario's eyes (from his head bone), his head is hidden, the
  stick moves him relative to your gaze, and two-bone IK drives his arms to your controllers.
- Scenes without Mario (title, file select, act select) are drawn exactly like the original game
  onto a screen that stays put in the room.

Code: `src/pc/vr/` (OpenXR, overlay, math), `src/game/vr_game.c` (camera, IK, input),
`src/pc/gfx/gfx_pc.c` (stereo). Notes and gotchas: [`docs/DEVLOG.md`](docs/DEVLOG.md).

## Building by hand

With MSYS2 MinGW64 (`make python mingw-w64-x86_64-{gcc,SDL2,glew,pkgconf}`) and
`baserom.us.z64` in the repo root:

```
make -j8 VERSION=us
```

The exe and `openxr_loader.dll` end up in `build/us_pc/`.

## Credits

- [sm64ex](https://github.com/sm64pc/sm64ex) and the [SM64 decompilation](https://github.com/n64decomp/sm64)
  teams — the original README is in [README_sm64ex.md](README_sm64ex.md).
- [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) — headers and loader in `openxr/`
  (Apache-2.0, see `openxr/LICENSE`).
- VR mod by cooperdmendoza-prog, written with Claude (AI).

Super Mario 64 is © Nintendo. This is a non-commercial fan project and is not affiliated with Nintendo.

@echo off
rem Super Mario 64 in VR (uses your active OpenXR runtime: SteamVR, Oculus, Virtual Desktop...).
rem Without a headset it starts the normal desktop game.
if not exist "%~dp0build\us_pc\sm64.us.f3dex2e.exe" (
  echo The game is not built yet. Run setup.bat first.
  pause
  exit /b 1
)
cd /d "%~dp0build\us_pc"
start "" sm64.us.f3dex2e.exe --skip-intro %*

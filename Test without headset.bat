@echo off
rem Runs the VR pipeline with a simulated headset (both eyes side by side) - for testing without a headset.
cd /d "%~dp0build\us_pc"
set VR_SIM_PITCH=-20
start "" sm64.us.f3dex2e.exe --vrsim --skip-intro %*

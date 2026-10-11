@echo off
rem One-click setup for Super Mario 64 VR: installs build tools, asks for your ROM, builds, makes shortcuts.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*

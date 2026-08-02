@echo off
rem Standalone numerical test suite for the VR held-object physics (vr_physics.cpp compiles
rem with no engine dependencies). Run this, then vrphys_suite.exe — a stable sim reads ~0.00mm
rem HF jitter on every held case. See HANDOFF-VR-COMBAT.md at the repo root.
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /std:c++17 /I "%~dp0..\..\libultraship\include" /I "%~dp0..\..\libultraship\src" vrphys_suite.cpp /Fe:vrphys_suite.exe

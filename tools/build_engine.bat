@echo off
rem Engine, its test, and whatever else links it. Run build_master.bat first for f2mc16 and tms57070.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
set CC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set FLAGS=/O2 /clang:-O3 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /w /Isrc\shim /Isrc\f2mc16 /Isrc
%CC% %FLAGS% /FIemu.h /c src\ob12_engine.cpp /Fobuild\ob12_engine.obj
%CC% %FLAGS% /c tools\engine_test.cpp /Fobuild\engine_test.obj
%CC% build\engine_test.obj build\ob12_engine.obj build\f2mc16.obj build\tms57070.obj synchronization.lib /Febuild\engine_test.exe
%CC% %FLAGS% /c src\panel_view.cpp /Fobuild\panel_view.obj
%CC% %FLAGS% /c src\ob12_player.cpp /Fobuild\ob12_player.obj
%CC% %FLAGS% /c src\live_io.cpp /Fobuild\live_io.obj
%CC% build\ob12_player.obj build\panel_view.obj build\live_io.obj build\ob12_engine.obj build\f2mc16.obj build\tms57070.obj synchronization.lib d2d1.lib winmm.lib user32.lib /Febuild\ob12_player.exe
%CC% %FLAGS% tools\live_api_test.cpp build\ob12_engine.obj build\f2mc16.obj build\tms57070.obj synchronization.lib /Febuild\live_api_test.exe
%CC% %FLAGS% tools\pace_test.cpp build\ob12_engine.obj build\f2mc16.obj build\tms57070.obj synchronization.lib winmm.lib /Febuild\pace_test.exe
%CC% %FLAGS% tools\env_test.cpp build\ob12_engine.obj build\f2mc16.obj build\tms57070.obj synchronization.lib /Febuild\env_test.exe
%CC% %FLAGS% tools\seq_test.cpp build\ob12_engine.obj build\f2mc16.obj build\tms57070.obj synchronization.lib /Febuild\seq_test.exe
echo BUILD_EXIT=%ERRORLEVEL%

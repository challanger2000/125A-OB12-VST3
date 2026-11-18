@echo off
rem OB-12 VST3 bundle. Needs build_master.bat and build_engine.bat first.
rem SDK path: a local Steinberg VST3 SDK with pluginterfaces, set below or in OB12_VST3SDK.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
if "%OB12_VST3SDK%"=="" set OB12_VST3SDK=F:\Claude\eii-vst3\vendor\vst3sdk
set SDK=%OB12_VST3SDK%
set CC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set FLAGS=/O2 /clang:-O3 /EHsc /std:c++17 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /w /Isrc /I"%SDK%"
if not exist build\vst3 mkdir build\vst3
rem A running host locks the bundle: set OB12_BUNDLE_ROOT to build beside it.
if "%OB12_BUNDLE_ROOT%"=="" set OB12_BUNDLE_ROOT=build\OB12.vst3
set BUNDLE=%OB12_BUNDLE_ROOT%\Contents\x86_64-win
if not exist %BUNDLE% mkdir %BUNDLE%
%CC% %FLAGS% /c src\ob12_vst3.cpp /Fobuild\vst3\plugin.obj
%CC% %FLAGS% /c "%SDK%\pluginterfaces\base\coreiids.cpp" /Fobuild\vst3\coreiids.obj
%CC% %FLAGS% /c "%SDK%\pluginterfaces\base\funknown.cpp" /Fobuild\vst3\funknown.obj
%CC% %FLAGS% /c "%SDK%\public.sdk\source\vst\vstinitiids.cpp" /Fobuild\vst3\vstinitiids.obj
%CC% %FLAGS% /c tools\vst3_iids.cpp /Fobuild\vst3\iids.obj
%CC% /LD build\vst3\plugin.obj build\vst3\coreiids.obj build\vst3\funknown.obj build\vst3\vstinitiids.obj build\vst3\iids.obj build\panel_view.obj build\ob12_engine.obj build\f2mc16.obj build\tms57070.obj synchronization.lib d2d1.lib user32.lib ole32.lib winmm.lib /Fe%BUNDLE%\OB12.vst3
%CC% %FLAGS% tools\vst3_smoke.cpp build\vst3\coreiids.obj build\vst3\funknown.obj build\vst3\vstinitiids.obj ole32.lib user32.lib /Febuild\vst3_smoke.exe
echo BUILD_EXIT=%ERRORLEVEL%

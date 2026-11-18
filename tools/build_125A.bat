@echo off
setlocal EnableExtensions
rem Portable 125A OB-12 x64 VST3 build. No F:\ paths; no firmware in Git.
cd /d "%~dp0.."
if not defined OB12_VST3SDK (
  echo ERROR: set OB12_VST3SDK to the Steinberg VST3 SDK root.
  exit /b 2
)
if not exist "%OB12_VST3SDK%\pluginterfaces\base\coreiids.cpp" (
  echo ERROR: invalid OB12_VST3SDK: "%OB12_VST3SDK%"
  exit /b 2
)
if not exist "rom\12cpu152-CPU-v1.52-FINAL.bin" (
  echo ERROR: Supply your own original OB-12 firmware in rom\ (not in Git).
  exit /b 2
)
if not exist "rom\dsp" (
  echo ERROR: rom\dsp is missing. Decode the original DSP firmware with tools\ob12_carve.py.
  exit /b 2
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -property installationPath`) do set "VS_HOME=%%I"
if defined VS_HOME if exist "%VS_HOME%\VC\Auxiliary\Build\vcvars64.bat" call "%VS_HOME%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not defined OB12_CC if defined VS_HOME if exist "%VS_HOME%\VC\Tools\Llvm\x64\bin\clang-cl.exe" set "OB12_CC=%VS_HOME%\VC\Tools\Llvm\x64\bin\clang-cl.exe"
if not defined OB12_CC set "OB12_CC=clang-cl.exe"
"%OB12_CC%" --version >nul 2>&1
if errorlevel 1 (
  echo ERROR: clang-cl not found. Install the Visual Studio 2022 LLVM components.
  exit /b 2
)
where python >nul 2>&1
if errorlevel 1 (
  echo ERROR: Python 3 is required for DSP translation.
  exit /b 2
)
if not exist build mkdir build
if not exist build\vst3 mkdir build\vst3
python tools\gen_fast.py rom\dsp build\tms_fast.inc
if errorlevel 1 goto :fail
set "ROOTFLAGS=/O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /Isrc\shim /Isrc\f2mc16 /Isrc /Ibuild"
set "FLAGS=%ROOTFLAGS% /I%OB12_VST3SDK%"
echo [1/9] F2MC16
"%OB12_CC%" %ROOTFLAGS% /FIemu.h /c src\f2mc16\f2mc16.cpp /Fobuild\f2mc16.obj
if errorlevel 1 goto :fail
echo [2/9] TMS57070
"%OB12_CC%" %ROOTFLAGS% /FIemu.h /c src\tms57070\tms57070.cpp /Fobuild\tms57070.obj
if errorlevel 1 goto :fail
echo [3/9] OB-12 Engine
"%OB12_CC%" %ROOTFLAGS% /FIemu.h /c src\ob12_engine.cpp /Fobuild\ob12_engine.obj
if errorlevel 1 goto :fail
echo [4/9] Hardware Panel + Preset Browser
"%OB12_CC%" %ROOTFLAGS% /c src\panel_view.cpp /Fobuild\panel_view.obj
if errorlevel 1 goto :fail
echo [5/9] VST3 Processor + Controller
"%OB12_CC%" %FLAGS% /c src\ob12_vst3.cpp /Fobuild\vst3\plugin.obj
if errorlevel 1 goto :fail
echo [6/9] Steinberg Core
"%OB12_CC%" %FLAGS% /c "%OB12_VST3SDK%\pluginterfaces\base\coreiids.cpp" /Fobuild\vst3\coreiids.obj
if errorlevel 1 goto :fail
"%OB12_CC%" %FLAGS% /c "%OB12_VST3SDK%\pluginterfaces\base\funknown.cpp" /Fobuild\vst3\funknown.obj
if errorlevel 1 goto :fail
echo [7/9] Steinberg VST IDs
"%OB12_CC%" %FLAGS% /c "%OB12_VST3SDK%\public.sdk\source\vst\vstinitiids.cpp" /Fobuild\vst3\vstinitiids.obj
if errorlevel 1 goto :fail
"%OB12_CC%" %FLAGS% /c tools\vst3_iids.cpp /Fobuild\vst3\iids.obj
if errorlevel 1 goto :fail
echo [8/9] Link
set "BUNDLE=build\OB12.vst3\Contents\x86_64-win"
if not exist "%BUNDLE%" mkdir "%BUNDLE%"
"%OB12_CC%" /LD build\vst3\plugin.obj build\vst3\coreiids.obj build\vst3\funknown.obj build\vst3\vstinitiids.obj build\vst3\iids.obj build\panel_view.obj build\ob12_engine.obj build\f2mc16.obj build\tms57070.obj synchronization.lib d2d1.lib dwrite.lib windowscodecs.lib gdi32.lib user32.lib ole32.lib comdlg32.lib winmm.lib /Fe"%BUNDLE%\OB12.vst3"
if errorlevel 1 goto :fail
echo [9/9] Package personal firmware beside the DLL
xcopy "rom" "%BUNDLE%\rom\" /E /I /Q /Y >nul
if errorlevel 2 goto :fail
echo SUCCESS: build\OB12.vst3
exit /b 0
:fail
echo FAILED: VST3 build. See the compiler error above; no success claimed.
exit /b 1

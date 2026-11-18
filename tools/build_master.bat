@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
if not exist build mkdir build
set CC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set FLAGS=/O2 /clang:-O3 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /w /Isrc\shim /Isrc\f2mc16 /Isrc /Ibuild /FIemu.h
rem The DSP programs translated for the core (build\tms_fast.inc); needs rom\dsp from ob12_carve.py.
python tools\gen_fast.py rom\dsp build\tms_fast.inc
%CC% %FLAGS% /c src\f2mc16\f2mc16.cpp /Fobuild\f2mc16.obj
%CC% %FLAGS% /c src\tms57070\tms57070.cpp /Fobuild\tms57070.obj
%CC% %FLAGS% /c src\ob12_master.cpp /Fobuild\ob12_master.obj
%CC% %FLAGS% /c src\ob12_slave.cpp /Fobuild\ob12_slave.obj
%CC% %FLAGS% /c src\ob12_system.cpp /Fobuild\ob12_system_main.obj
%CC% build\ob12_master.obj build\f2mc16.obj /Febuild\ob12_master.exe
%CC% build\ob12_slave.obj build\f2mc16.obj /Febuild\ob12_slave.exe
%CC% %FLAGS% /c src\live_io.cpp /Fobuild\live_io.obj
%CC% build\ob12_system_main.obj build\f2mc16.obj build\tms57070.obj build\live_io.obj winmm.lib /Febuild\ob12_system.exe
echo BUILD_EXIT=%ERRORLEVEL%

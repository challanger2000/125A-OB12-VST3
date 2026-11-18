@echo off
rem Builds core_compare against a local copy of the public emulator.
rem usage: build_compare <folder holding TMS57070.h>. The emulator has no licence file, keep it out of this repo.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
if not exist build\cmp mkdir build\cmp
set CC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set FLAGS=/O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /w
%CC% %FLAGS% /FItools\tms_harness\open_private.h /I"%~1" /c "%~1\TMS57070.cpp" /Fobuild\cmp\e1.obj
%CC% %FLAGS% /FItools\tms_harness\open_private.h /I"%~1" /c "%~1\TMS57070_core.cpp" /Fobuild\cmp\e2.obj
%CC% %FLAGS% /FItools\tms_harness\open_private.h /I"%~1" /c "%~1\TMS57070_MAC.cpp" /Fobuild\cmp\e3.obj
%CC% %FLAGS% /Isrc /c src\tms57070\tms57070.cpp /Fobuild\cmp\own.obj
%CC% %FLAGS% /Isrc /I"%~1" /c tools\tms_harness\core_compare.cpp /Fobuild\cmp\cc.obj
%CC% build\cmp\cc.obj build\cmp\own.obj build\cmp\e1.obj build\cmp\e2.obj build\cmp\e3.obj /Febuild\core_compare_now.exe
echo BUILD_EXIT=%ERRORLEVEL%

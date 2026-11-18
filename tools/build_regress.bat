@echo off
rem Builds core_regress: the core of commit 87d17c3 against the current core.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
if not exist build\oldcore mkdir build\oldcore
git show 87d17c3:src/tms57070/tms57070.h > build\oldcore\tms57070.h
git show 87d17c3:src/tms57070/tms57070.cpp > build\oldcore\tms57070.cpp
set CC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set FLAGS=/O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /w
%CC% %FLAGS% /Dtms=tms_old /Ibuild\oldcore /c build\oldcore\tms57070.cpp /Fobuild\oldcore\old.obj
%CC% %FLAGS% /Isrc /c src\tms57070\tms57070.cpp /Fobuild\oldcore\new.obj
%CC% %FLAGS% /Isrc /Ibuild /c tools\tms_harness\core_regress.cpp /Fobuild\oldcore\regress.obj
%CC% build\oldcore\regress.obj build\oldcore\old.obj build\oldcore\new.obj /Febuild\core_regress.exe
echo BUILD_EXIT=%ERRORLEVEL%

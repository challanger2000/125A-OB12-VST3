@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
set CC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe"
%CC% /O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /w /Isrc /c tools\dasp4_gain.cpp /Fobuild\dasp4_gain.obj
%CC% build\dasp4_gain.obj build\tms57070.obj /Febuild\dasp4_gain.exe
echo BUILD_EXIT=%ERRORLEVEL%

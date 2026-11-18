@echo off
rem Profile guided f2mc16 and tms57070 objects. Run after build_master.bat and build_engine.bat,
rem then run build_engine.bat and build_vst3.bat again to link the new objects.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
set LLVM=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64
set CC="%LLVM%\bin\clang-cl.exe"
set FLAGS=/O2 /clang:-O3 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /w /Isrc\shim /Isrc\f2mc16 /Isrc /Ibuild /FIemu.h
if not exist build\pgo mkdir build\pgo
%CC% %FLAGS% /clang:-fprofile-instr-generate /c src\f2mc16\f2mc16.cpp /Fobuild\pgo\f2mc16.obj
%CC% %FLAGS% /clang:-fprofile-instr-generate /c src\tms57070\tms57070.cpp /Fobuild\pgo\tms57070.obj
%CC% build\engine_test.obj build\ob12_engine.obj build\pgo\f2mc16.obj build\pgo\tms57070.obj synchronization.lib "%LLVM%\lib\clang\19\lib\windows\clang_rt.profile-x86_64.lib" /Febuild\pgo\engine_test.exe
set LLVM_PROFILE_FILE=build\pgo\ob12.profraw
build\pgo\engine_test.exe rom 20 build\pgo\out.wav
"%LLVM%\bin\llvm-profdata.exe" merge -o build\pgo\ob12.profdata build\pgo\ob12.profraw
%CC% %FLAGS% /clang:-fprofile-instr-use=build\pgo\ob12.profdata /c src\f2mc16\f2mc16.cpp /Fobuild\f2mc16.obj
%CC% %FLAGS% /clang:-fprofile-instr-use=build\pgo\ob12.profdata /c src\tms57070\tms57070.cpp /Fobuild\tms57070.obj
echo BUILD_EXIT=%ERRORLEVEL%

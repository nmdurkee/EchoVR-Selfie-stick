@echo off
rem build_render.cmd - builds and runs the SelfieStick render-module tests (WARP GPU, no headset).
rem Output: build\render\render_tests.exe. The DLL itself is built by build.cmd (core),
rem which compiles selfie_render.cpp alongside its own sources.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\render mkdir build\render
cl /nologo /O2 /MT /W3 /c /TC vendor\minhook\src\buffer.c vendor\minhook\src\hook.c vendor\minhook\src\trampoline.c vendor\minhook\src\hde\hde64.c /Fo:build\render\
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /wd4324 render_tests.cpp selfie_render.cpp build\render\buffer.obj build\render\hook.obj build\render\trampoline.obj build\render\hde64.obj /Fo:build\render\ /Fe:build\render\render_tests.exe /link d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
build\render\render_tests.exe
exit /b %errorlevel%

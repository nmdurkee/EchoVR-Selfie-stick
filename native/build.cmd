@echo off
rem SelfieStick.dll core build: runs the unit tests, then links dist\SelfieStick.dll.
rem Links selfie_render.cpp when it exists, otherwise selfie_render_stub.cpp.
rem Links selfie_capture.cpp when it exists, otherwise selfie_capture_stub.cpp.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist vendor\minhook\src\hook.c (
  xcopy /E /I /Q /Y "%~dp0..\..\PersonalDiscTrainer\native\vendor\minhook\include" vendor\minhook\include >nul
  xcopy /E /I /Q /Y "%~dp0..\..\PersonalDiscTrainer\native\vendor\minhook\src" vendor\minhook\src >nul
)
if not exist vendor\minhook\src\hook.c (echo MinHook missing & exit /b 1)
if not exist build mkdir build
if not exist dist mkdir dist

echo === unit tests ===
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 selfie_tests.cpp /Fo:build\selfie_tests.obj /Fe:build\tests.exe
if errorlevel 1 exit /b 1
build\tests.exe
if not "%errorlevel%"=="0" (echo TESTS FAILED & exit /b 1)

echo === MinHook ===
cl /nologo /O2 /MT /W3 /c /TC vendor\minhook\src\buffer.c vendor\minhook\src\hook.c vendor\minhook\src\trampoline.c vendor\minhook\src\hde\hde64.c /Fo:build\
if errorlevel 1 exit /b 1

set RENDER_SRC=selfie_render_stub.cpp
if exist selfie_render.cpp set RENDER_SRC=selfie_render.cpp
set CAPTURE_SRC=selfie_capture_stub.cpp
if exist selfie_capture.cpp set CAPTURE_SRC=selfie_capture.cpp
echo === SelfieStick.dll (render: %RENDER_SRC%, capture: %CAPTURE_SRC%) ===
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /wd4324 /LD /Ivendor\minhook\include selfie_core.cpp selfie_camera.cpp selfie_input.cpp %RENDER_SRC% %CAPTURE_SRC% build\buffer.obj build\hook.obj build\trampoline.obj build\hde64.obj /Fo:build\ /Fe:dist\SelfieStick.dll /link /IMPLIB:build\SelfieStick.lib d3d12.lib dxgi.lib user32.lib winmm.lib ole32.lib oleaut32.lib windowscodecs.lib gdi32.lib shell32.lib
if errorlevel 1 exit /b 1
rem keep the stubs compiling even when the real modules are linked
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /c selfie_render_stub.cpp selfie_capture_stub.cpp /Fo:build\
if errorlevel 1 exit /b 1
echo OK: dist\SelfieStick.dll
exit /b 0

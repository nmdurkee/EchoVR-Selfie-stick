@echo off
rem build_capture.cmd - builds and runs the SelfieStick screenshot-module tests (WARP GPU, no headset, no Echo).
rem Output: build\capture\capture_tests.exe and capture_tests_alt.exe. The DLL itself is built by build.cmd,
rem which compiles selfie_capture.cpp alongside the other sources (system libs come from #pragma comment).
rem The "alt" build includes selfie_shared.h before d3d12.h, proving selfie_render::EchoQueue() has one
rem symbol whatever the include order (the contract forward-declares ::ID3D12CommandQueue).
rem NOTE: the tests put a test image on the clipboard (the text clipboard is saved and restored).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\capture mkdir build\capture
echo === capture tests (d3d12.h included first) ===
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 capture_tests.cpp selfie_capture.cpp /Fo:build\capture\ /Fe:build\capture\capture_tests.exe /link d3d12.lib dxgi.lib user32.lib ole32.lib shell32.lib windowscodecs.lib
if errorlevel 1 exit /b 1
build\capture\capture_tests.exe
if not "%errorlevel%"=="0" (echo CAPTURE TESTS FAILED & exit /b 1)
echo === capture tests (selfie_shared.h included first) ===
if not exist build\capture\alt mkdir build\capture\alt
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DCAPTURE_ALT_MANGLING capture_tests.cpp selfie_capture.cpp /Fo:build\capture\alt\ /Fe:build\capture\capture_tests_alt.exe /link d3d12.lib dxgi.lib user32.lib ole32.lib shell32.lib windowscodecs.lib
if errorlevel 1 exit /b 1
build\capture\capture_tests_alt.exe
if not "%errorlevel%"=="0" (echo CAPTURE TESTS FAILED [alt] & exit /b 1)
echo OK: capture tests passed
exit /b 0

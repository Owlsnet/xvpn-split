@echo off
rem Builds build\xvpn-ui.exe.
rem Needs Visual Studio 2022 or the Build Tools for Visual Studio 2022 with the
rem "Desktop development with C++" workload (MSVC and a Windows SDK).
rem Set VCVARS to the full path of vcvars64.bat to skip the automatic lookup.
setlocal
set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "IMGUI=%ROOT%third_party\imgui"

if defined VCVARS goto have_vcvars
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_msvc
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_msvc
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"

:have_vcvars
if not exist "%VCVARS%" goto no_msvc
call "%VCVARS%" >nul
if errorlevel 1 goto no_msvc
if not exist "%OUT%" mkdir "%OUT%"

echo compiling resources
pushd "%ROOT%src"
rc /nologo /fo "%OUT%\xvpn.res" xvpn.rc
set RC_RESULT=%errorlevel%
popd
if not "%RC_RESULT%"=="0" goto failed

echo compiling xvpn-ui.exe
cl /nologo /utf-8 /std:c++17 /O2 /MT /EHsc /W3 /DNDEBUG /DUNICODE /D_UNICODE ^
   /I"%IMGUI%" /I"%IMGUI%\backends" ^
   "%ROOT%src\main.cpp" "%ROOT%src\ui.cpp" "%ROOT%src\net.cpp" "%ROOT%src\engine.cpp" "%ROOT%src\common.cpp" ^
   "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" ^
   "%IMGUI%\backends\imgui_impl_win32.cpp" "%IMGUI%\backends\imgui_impl_dx11.cpp" ^
   "%OUT%\xvpn.res" ^
   /Fe:"%OUT%\xvpn-ui.exe" /Fo:"%OUT%\\" ^
   /link /SUBSYSTEM:WINDOWS ^
   d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib user32.lib gdi32.lib shell32.lib ole32.lib ^
   iphlpapi.lib ws2_32.lib winhttp.lib
if errorlevel 1 goto failed
echo.
echo built %OUT%\xvpn-ui.exe
exit /b 0

:no_msvc
echo error: MSVC was not found. Install Visual Studio 2022 or its Build Tools with the
echo        "Desktop development with C++" workload, or set VCVARS to vcvars64.bat.
exit /b 1

:failed
echo build failed
exit /b 1

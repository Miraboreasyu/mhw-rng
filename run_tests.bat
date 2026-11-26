@echo off
setlocal

where cl.exe >nul 2>&1
if not errorlevel 1 goto :have_cl

set "VSDIR="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
)
if "%VSDIR%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
)
if "%VSDIR%"=="" (
    echo ERROR: MSVC x64 toolchain not found.
    exit /b 1
)
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:have_cl
set "ROOT=%~dp0"
set "SRCDIR=%ROOT%src"
set "INCDIR=%ROOT%include"
set "OUTDIR=%ROOT%build"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

ml64.exe /nologo /c /Fo"%OUTDIR%\rng_trampolines.obj" "%SRCDIR%\rng_trampolines.asm"
if %ERRORLEVEL% NEQ 0 exit /b 1

cl.exe /nologo /std:c++17 /O2 /MP /W4 /EHsc /MT /utf-8 /D "NOMINMAX" /D "MHW_RNG_EXPORTS" ^
    /I "%SRCDIR%" /I "%INCDIR%" /I "%ROOT%tests" ^
    "%ROOT%tests\test_main.cpp" ^
    "%ROOT%tests\test_kernel_core.cpp" ^
    "%ROOT%tests\test_sdk_v3.cpp" ^
    "%ROOT%tests\test_console_i18n.cpp" ^
    "%ROOT%tests\test_plugin_size_thk_steam.cpp" ^
    "%ROOT%tests\test_plugin_endemic_crit.cpp" ^
    "%ROOT%tests\test_world_e2e.cpp" ^
    "%SRCDIR%\audio_mark.cpp" ^
    "%SRCDIR%\command_router.cpp" ^
    "%SRCDIR%\console_core.cpp" ^
    "%SRCDIR%\hook.cpp" ^
    "%SRCDIR%\hud_mark.cpp" ^
    "%SRCDIR%\log_config.cpp" ^
    "%SRCDIR%\peer_beacon.cpp" ^
    "%SRCDIR%\plugin_host.cpp" ^
    "%SRCDIR%\rng_engine.cpp" ^
    "%OUTDIR%\rng_trampolines.obj" ^
    /Fe:"%OUTDIR%\test_rng_harness.exe" ^
    /Fo:"%OUTDIR%\\" ^
    /link kernel32.lib user32.lib ole32.lib

if %ERRORLEVEL% NEQ 0 exit /b 1
"%OUTDIR%\test_rng_harness.exe" %*
exit /b %ERRORLEVEL%

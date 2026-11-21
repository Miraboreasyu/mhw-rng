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

cl.exe /nologo /std:c++17 /O2 /W4 /EHsc /MT /utf-8 /D "MHW_RNG_EXPORTS" ^
    /I "%SRCDIR%" /I "%INCDIR%" ^
    "%ROOT%tests\test_rng_harness.cpp" ^
    "%SRCDIR%\hook.cpp" ^
    "%SRCDIR%\log_config.cpp" ^
    "%SRCDIR%\plugin_host.cpp" ^
    "%SRCDIR%\rng_engine.cpp" ^
    /Fe:"%OUTDIR%\test_rng_harness.exe" ^
    /Fo:"%OUTDIR%\\" ^
    /link kernel32.lib user32.lib

if %ERRORLEVEL% NEQ 0 exit /b 1
"%OUTDIR%\test_rng_harness.exe" %*
exit /b %ERRORLEVEL%

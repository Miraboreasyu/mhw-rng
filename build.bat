@echo off
:: ============================================================================
:: Build MHW Universal RNG Microkernel + Official Hot-Reloadable Plugins
::   1. build\mhw_rng.dll                  -> nativePC\plugins\mhw_rng.dll
::   2. build\mhw_rng\rng_inspector.dll    -> nativePC\plugins\mhw_rng\rng_inspector.dll
::   3. build\mhw_rng\rng_size.dll         -> nativePC\plugins\mhw_rng\rng_size.dll
::   4. build\mhw_rng\rng_endemic.dll      -> nativePC\plugins\mhw_rng\rng_endemic.dll
:: ============================================================================

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
    echo ERROR: Visual Studio MSVC x64 toolchain not found.
    exit /b 1
)

set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:have_cl
set "ROOT=%~dp0"
set "SRCDIR=%ROOT%src"
set "INCDIR=%ROOT%include"
set "PLUGSRC=%ROOT%plugins"
set "OUTDIR=%ROOT%build"
set "PLUGOUT=%OUTDIR%\mhw_rng"

if exist "%ROOT%sync_plugin_sdks.bat" call "%ROOT%sync_plugin_sdks.bat"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%PLUGOUT%" mkdir "%PLUGOUT%"

echo ===== [1/4] Building RNG Microkernel (mhw_rng.dll) =====
cl.exe /nologo /std:c++17 /O2 /GL /GR- ^
    /W4 /WX- /EHsc /MT ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "NOMINMAX" /D "MHW_RNG_EXPORTS" ^
    /utf-8 ^
    /I "%SRCDIR%" /I "%INCDIR%" ^
    "%SRCDIR%\dllmain.cpp" ^
    "%SRCDIR%\hook.cpp" ^
    "%SRCDIR%\log_config.cpp" ^
    "%SRCDIR%\plugin_host.cpp" ^
    "%SRCDIR%\rng_engine.cpp" ^
    /Fe:"%OUTDIR%\mhw_rng.dll" ^
    /Fo:"%OUTDIR%\\" ^
    /LD ^
    /link kernel32.lib user32.lib /OPT:REF /OPT:ICF ^
    /OUT:"%OUTDIR%\mhw_rng.dll"
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo ===== [2/4] Building Plugin: rng_inspector.dll =====
cl.exe /nologo /std:c++17 /O2 /GL /GR- ^
    /W4 /WX- /EHsc /MT ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "NOMINMAX" ^
    /utf-8 ^
    /I "%INCDIR%" /I "%PLUGSRC%\inspector" ^
    "%PLUGSRC%\inspector\inspector_plugin.cpp" ^
    /Fe:"%PLUGOUT%\rng_inspector.dll" ^
    /Fo:"%OUTDIR%\\" ^
    /LD ^
    /link kernel32.lib /OPT:REF /OPT:ICF ^
    /OUT:"%PLUGOUT%\rng_inspector.dll"
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo ===== [3/4] Building Plugin: rng_size.dll =====
cl.exe /nologo /std:c++17 /O2 /GL /GR- ^
    /W4 /WX- /EHsc /MT ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "NOMINMAX" ^
    /utf-8 ^
    /I "%INCDIR%" ^
    "%PLUGSRC%\size\size_plugin.cpp" ^
    /Fe:"%PLUGOUT%\rng_size.dll" ^
    /Fo:"%OUTDIR%\\" ^
    /LD ^
    /link kernel32.lib /OPT:REF /OPT:ICF ^
    /OUT:"%PLUGOUT%\rng_size.dll"
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo ===== [4/4] Building Plugin: rng_endemic.dll =====
cl.exe /nologo /std:c++17 /O2 /GL /GR- ^
    /W4 /WX- /EHsc /MT ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "NOMINMAX" ^
    /utf-8 ^
    /I "%INCDIR%" ^
    "%PLUGSRC%\endemic\endemic_plugin.cpp" ^
    /Fe:"%PLUGOUT%\rng_endemic.dll" ^
    /Fo:"%OUTDIR%\\" ^
    /LD ^
    /link kernel32.lib /OPT:REF /OPT:ICF ^
    /OUT:"%PLUGOUT%\rng_endemic.dll"
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo ===== ALL BUILDS SUCCEEDED =====
exit /b 0

@echo off
:: ============================================================================
:: Build MHW Universal RNG Microkernel + Official Hot-Reloadable Plugins
::   1. build\mhw_rng.dll                  -> nativePC\plugins\mhw_rng.dll
::   2. build\mhw_rng\rng_inspector.dll    -> nativePC\plugins\mhw_rng\rng_inspector.dll
::   3. build\mhw_rng\rng_size.dll         -> nativePC\plugins\mhw_rng\rng_size.dll
:: ============================================================================

setlocal

set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: Visual Studio not found at %VSDIR%
    exit /b 1
)

set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set "ROOT=%~dp0"
set "SRCDIR=%ROOT%src"
set "INCDIR=%ROOT%include"
set "PLUGSRC=%ROOT%plugins"
set "OUTDIR=%ROOT%build"
set "PLUGOUT=%OUTDIR%\mhw_rng"

if exist "%ROOT%sync_plugin_sdks.bat" call "%ROOT%sync_plugin_sdks.bat"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%PLUGOUT%" mkdir "%PLUGOUT%"

echo ===== [1/3] Building RNG Microkernel (mhw_rng.dll) =====
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
echo ===== [2/3] Building Plugin: rng_inspector.dll =====
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
echo ===== [3/3] Building Plugin: rng_size.dll =====
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
echo ===== ALL BUILDS SUCCEEDED =====
exit /b 0

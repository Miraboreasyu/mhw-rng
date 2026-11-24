@echo off
:: Syncs `include\mhw_rng_api.h` from the core framework into official plugin sub-repos.
setlocal

set "ROOT=%~dp0"
set "SRC_HDR=%ROOT%include\mhw_rng_api.h"

if not exist "%SRC_HDR%" (
    echo ERROR: SDK header not found: %SRC_HDR%
    exit /b 1
)

for %%P in (size inspector endemic thk steam crit) do (
    if exist "%ROOT%plugins\%%P" (
        if not exist "%ROOT%plugins\%%P\include" mkdir "%ROOT%plugins\%%P\include"
        copy /y "%SRC_HDR%" "%ROOT%plugins\%%P\include\mhw_rng_api.h" >nul
        if errorlevel 1 (
            echo ERROR: Failed to sync header to plugins\%%P\include\
            exit /b 1
        )
        echo [sync] Updated plugins\%%P\include\mhw_rng_api.h
    )
)

exit /b 0

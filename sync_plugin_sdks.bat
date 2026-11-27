@echo off
:: Syncs `include\mhw_rng_api.h` into all 6 official plugin sub-repos, and syncs
:: companion SDK headers into the 5 refactored plugins (leaving `steam` pure SDK-only).
setlocal

set "ROOT=%~dp0"
set "INC=%ROOT%include"

if not exist "%INC%\mhw_rng_api.h" (
    echo ERROR: SDK header not found: %INC%\mhw_rng_api.h
    exit /b 1
)

for %%P in (size inspector endemic thk steam crit) do (
    if exist "%ROOT%plugins\%%P" (
        if not exist "%ROOT%plugins\%%P\include" mkdir "%ROOT%plugins\%%P\include"
        copy /y "%INC%\mhw_rng_api.h" "%ROOT%plugins\%%P\include\mhw_rng_api.h" >nul
        if errorlevel 1 (
            echo ERROR: Failed to sync mhw_rng_api.h to plugins\%%P\include\
            exit /b 1
        )
        echo [sync] Updated plugins\%%P\include\mhw_rng_api.h
    )
)

for %%P in (size inspector endemic thk crit) do (
    if exist "%ROOT%plugins\%%P" (
        for %%H in (mhw_rng_math.h mhw_1523_world.h mhw_world_api.h mhw_catalog_api.h) do (
            if not exist "%INC%\%%H" (
                echo ERROR: Companion header not found: %INC%\%%H
                exit /b 1
            )
            copy /y "%INC%\%%H" "%ROOT%plugins\%%P\include\%%H" >nul
            if errorlevel 1 (
                echo ERROR: Failed to sync %%H to plugins\%%P\include\
                exit /b 1
            )
        )
        echo [sync] Updated companion headers in plugins\%%P\include\
    )
)

if exist "%ROOT%plugins\thk" (
    copy /y "%INC%\mhw_thk_api.h" "%ROOT%plugins\thk\include\mhw_thk_api.h" >nul
    if errorlevel 1 (
        echo ERROR: Failed to sync mhw_thk_api.h to plugins\thk\include\
        exit /b 1
    )
    echo [sync] Updated plugins\thk\include\mhw_thk_api.h
)

if exist "%ROOT%plugins\crit" (
    copy /y "%INC%\mhw_crit_api.h" "%ROOT%plugins\crit\include\mhw_crit_api.h" >nul
    if errorlevel 1 (
        echo ERROR: Failed to sync mhw_crit_api.h to plugins\crit\include\
        exit /b 1
    )
    echo [sync] Updated plugins\crit\include\mhw_crit_api.h
)

exit /b 0

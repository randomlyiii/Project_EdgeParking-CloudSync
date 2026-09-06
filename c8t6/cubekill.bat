@echo off
REM ================================================================
REM  cubekill.bat - portable cleaner for STM32CubeMX / CubeIDE projects
REM
REM  Usage:
REM    cubekill.bat                 clean the folder this .bat is in
REM    cubekill.bat D:\path\to\proj clean any CubeMX/CubeIDE project
REM
REM  Deletes (all regenerable by "Build Project" in CubeIDE / CubeMX):
REM    - build output directories (Debug\, Release\, build\, and any dir
REM      carrying CubeIDE's objects.list or CMake's CMakeCache.txt),
REM      recursively located at ANY depth (e.g. m4_fw\CM4\Debug).
REM      Conservative double-check: a dir is deleted only when it ALSO
REM      contains real build artifacts (*.o *.mk makefile Makefile *.elf *.map)
REM      at its own top level - a source folder that merely shares a name
REM      (or accidentally holds a lone marker file) survives.
REM    - *.tmp scattered across the tree (regenerable temp files)
REM    - *.bak is NOT swept: source-tree .bak files may be user backups
REM      (e.g. a .dts.bak). .bak inside removed build dirs are removed
REM      together with the dir.
REM
REM  Never touches: .ioc .mxproject .project .cproject .settings\
REM    Core\ Drivers\ Middlewares\ *.ld and all other source/config files.
REM    A dir named Debug/Release/build is only deleted if it actually
REM    contains compiled output (*.o *.mk makefile *.elf *.map), so a
REM    source folder that happens to share the name survives.
REM ================================================================
setlocal EnableExtensions
title cubekill - STM32CubeMX/CubeIDE cleaner

if "%~1"=="" (
    set "TARGET=%~dp0"
) else (
    set "TARGET=%~1"
)
if "%TARGET:~-1%"=="\" set "TARGET=%TARGET:~0,-1%"

if not exist "%TARGET%\" (
    echo [ERROR] Target folder not found: %TARGET%
    pause
    exit /b 1
)

echo ================================================================
echo  cubekill: STM32CubeMX / STM32CubeIDE cleaner
echo  Target: %TARGET%
echo ================================================================
echo.

set "IOCINFO="
for /r "%TARGET%" %%F in (*.ioc) do if not defined IOCINFO set "IOCINFO=%%F"
if defined IOCINFO (
    echo [INFO] CubeMX project: %IOCINFO%
) else (
    echo [INFO] No .ioc found under target - cleaning build outputs anyway.
)
echo.

set /a NDEL=0
set "LIST=%TEMP%\cubekill_%RANDOM%%RANDOM%.lst"
type nul > "%LIST%"

echo Scanning for build output directories ...
for /d /r "%TARGET%" %%D in (*) do call :CheckDir "%%D"
echo.

for /f "usebackq delims=" %%P in ("%LIST%") do (
    echo [DEL] %%P
    rd /s /q "%%P" >nul 2>&1
    if exist "%%P\" (
        echo       [WARN] Not fully removed - close STM32CubeIDE / terminals
        echo       using it, then run cubekill again.
    ) else (
        set /a NDEL+=1
    )
)

del /s /q "%TARGET%\*.tmp" >nul 2>&1

echo.
if "%NDEL%"=="0" (
    echo Nothing to clean - already clean.
) else (
    echo Done: %NDEL% build folder^(s^) removed. Rebuild in the IDE
    echo regenerates everything deleted.
)
echo Kept: all sources, project config (.ioc/.project/.cproject/.settings) and *.bak backups.
del /q "%LIST%" >nul 2>&1
echo.
pause
exit /b 0


:CheckDir
REM Appends %1 to the delete list only if it is a genuine build dir.
REM 保守判定：名字命中 或 标记文件命中，都还必须满足"本目录顶层确有编译产物"，
REM 防止源码目录（恰好叫 Debug/Release/build，或误留 objects.list 等）被误删。
setlocal
set "D=%~1"
set "NAME=%~nx1"
set "ARTIFACT="
if exist "%D%\*.o"       set "ARTIFACT=1"
if exist "%D%\*.mk"      set "ARTIFACT=1"
if exist "%D%\makefile"  set "ARTIFACT=1"
if exist "%D%\Makefile"  set "ARTIFACT=1"
if exist "%D%\*.elf"     set "ARTIFACT=1"
if exist "%D%\*.map"     set "ARTIFACT=1"
if not defined ARTIFACT (
    endlocal
    exit /b 0
)
set "HIT="
set "MARK="
if exist "%D%\objects.list"   set "MARK=1"
if exist "%D%\CMakeCache.txt" set "MARK=1"
set "CAND="
if /i "%NAME%"=="Debug"   set "CAND=1"
if /i "%NAME%"=="Release" set "CAND=1"
if /i "%NAME%"=="build"   set "CAND=1"
if defined CAND set "HIT=1"
if defined MARK set "HIT=1"
if defined HIT >>"%LIST%" echo %D%
endlocal
exit /b 0

@echo off
rem =====================================================================
rem  run_test.bat - launcher for cloud_api_test.py
rem  EdgeParking cloud fallback API test (step 7 / P7-01)
rem
rem  Usage:
rem     run_test.bat                 interactive menu
rem     run_test.bat 1               real image recognition round trip
rem     run_test.bat 2               connectivity test (text only)
rem     run_test.bat 3               offline self test
rem     run_test.bat 4               image report
rem     run_test.bat 5               probe model names
rem     run_test.bat 6               open README.txt
rem     run_test.bat 7               recognize with qwen-vl-plus
rem     run_test.bat 8               recognize with qwen-vl-ocr
rem     run_test.bat --mode ...      any python argument is passed through
rem  Inputs, all in this folder:
rem     key.txt        api_key, and api_base when present (fallback: sample_key.txt)
rem     sample_key.txt base_url reference snippet from the cloud docs
rem     the plate image (default: the .jpg in this folder), auto-detected
rem =====================================================================
setlocal EnableExtensions

rem UTF-8 console so the Chinese plate prints correctly
chcp 65001 >nul 2>&1
title EdgeParking cloud API test

cd /d "%~dp0"
set "SCRIPT=%~dp0cloud_api_test.py"
if not exist "%SCRIPT%" (
    echo [ERROR] cloud_api_test.py not found next to this script.
    goto :end
)

rem ---- locate a python interpreter -------------------------------------
set "PY="
where python >nul 2>&1
if not errorlevel 1 set "PY=python"
if not defined PY (
    where python3 >nul 2>&1
    if not errorlevel 1 set "PY=python3"
)
if not defined PY (
    for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*") do (
        if exist "%%D\python.exe" set "PY=%%D\python.exe"
    )
)
if not defined PY (
    for /d %%D in ("C:\Python3*") do (
        if exist "%%D\python.exe" set "PY=%%D\python.exe"
    )
)
if not defined PY (
    for /d %%D in ("%ProgramFiles%\Python3*") do (
        if exist "%%D\python.exe" set "PY=%%D\python.exe"
    )
)
if not defined PY (
    echo [ERROR] no Python interpreter found.
    echo         Install Python 3.8+ or add it to PATH, then run again.
    echo         The test needs no extra packages ^(standard library only^).
    goto :end
)

rem ---- pick the mode ----------------------------------------------------
rem A leading "-" means a raw python command line; a bare digit is a shortcut
rem and any extra arguments are passed through unchanged.
set "MODE=%~1"
if "%MODE%"=="" goto :menu
set "EXTRA="
if not "%~2"=="" (
    for /f "tokens=1,* delims= " %%A in ("%*") do set "EXTRA=%%B"
)
if "%MODE:~0,1%"=="-" goto :run
if "%MODE%"=="1" set "MODE=--mode recognize"                      & goto :run
if "%MODE%"=="2" set "MODE=--mode test"                           & goto :run
if "%MODE%"=="3" set "MODE=--mode selftest"                       & goto :run
if "%MODE%"=="4" set "MODE=--mode info"                           & goto :run
if "%MODE%"=="5" set "MODE=--mode models"                         & goto :run
if "%MODE%"=="6" goto :readme
if "%MODE%"=="7" set "MODE=--mode recognize --model qwen3-vl-flash" & goto :run
if "%MODE%"=="8" set "MODE=--mode recognize --model qwen-vl-ocr"  & goto :run
rem anything else: pass the whole command line through to python
set "MODE=%*"
set "EXTRA="
goto :run

:menu
echo.
echo  ============================================================
echo   EdgeParking cloud API test  ^(OpenAI compatible endpoint^)
echo   endpoint : key.txt api_base ^(public reference: sample_key.txt^)
echo   api key  : key.txt only  ^(sample_key.txt keeps the sk-xxx placeholder^)
echo   model    : key.txt / sample_key.txt model=, or --model
echo   image    : the plate image in this folder
echo  ============================================================
echo.
echo    [1] Recognize the sample plate image   ^(real call^)
echo    [2] Connectivity test, text only       ^(qwen-plus, real call^)
echo    [3] Offline self test                  ^(no network^)
echo    [4] Image report                       ^(no network^)
echo    [5] Probe model names                  ^(real calls^)
echo    [6] Show README.txt                    ^(usage / troubleshooting^)
echo    [7] Recognize with qwen3-vl-flash   ^(cheapest vision^)
echo    [8] Recognize with qwen-vl-ocr
echo    [0] Quit
echo.
set "CHOICE="
set /p "CHOICE=Select [1-8, default=1]: "
if not defined CHOICE set "CHOICE=1"
if "%CHOICE%"=="0" goto :end
set "MODE=--mode recognize"
if "%CHOICE%"=="2" set "MODE=--mode test"
if "%CHOICE%"=="3" set "MODE=--mode selftest"
if "%CHOICE%"=="4" set "MODE=--mode info"
if "%CHOICE%"=="5" set "MODE=--mode models"
if "%CHOICE%"=="6" goto :readme
if "%CHOICE%"=="7" set "MODE=--mode recognize --model qwen3-vl-flash"
if "%CHOICE%"=="8" set "MODE=--mode recognize --model qwen-vl-ocr"

:run
echo.
echo  interpreter : %PY%
echo  arguments   : %MODE% %EXTRA%
echo  ------------------------------------------------------------
"%PY%" "%SCRIPT%" %MODE% %EXTRA%
set "RC=%ERRORLEVEL%"
echo  ------------------------------------------------------------
if "%RC%"=="0" echo  [OK]   accepted ^(exit 0^)
if "%RC%"=="1" echo  [FAIL] failed   ^(exit 1^) - see the lines above
if "%RC%"=="2" echo  [WARN] unreadable ^(exit 2^) - no plate / low confidence
if "%RC%"=="3" echo  [ERR]  usage or environment problem ^(exit 3^)
goto :end

:readme
rem Open in the default text viewer: README.txt is UTF-8 with BOM and holds
rem Chinese text, which the console renders badly under a GBK code page.
rem "type" is used as a fallback because not "more" nor "start" is guaranteed.
if exist "%~dp0README.txt" (
    start "" "%~dp0README.txt"
    if errorlevel 1 type "%~dp0README.txt"
) else (
    echo [WARN] README.txt not found.
)

:end
echo.
pause
endlocal

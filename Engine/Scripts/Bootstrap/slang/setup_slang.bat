@echo off
setlocal enabledelayedexpansion

call "%~dp0\..\..\Env.bat"

REM ====== Config ======
set SLANG_VERSION=2026.5.2
set SLANG_FILE=slang-%SLANG_VERSION%-windows-x86_64.zip
set DOWNLOAD_URL=https://github.com/shader-slang/slang/releases/download/v%SLANG_VERSION%/%SLANG_FILE%

set SLANG_DIR=%THIRD_PARTY_DIR%\slang
set ZIP_PATH=%THIRD_PARTY_DIR%\%SLANG_FILE%
set TMP_DIR=%THIRD_PARTY_DIR%\_slang_tmp

REM 1=keep zip, 0=delete zip
set KEEP_ZIP=0

echo [1/5] Prepare directories...
if not exist "%THIRD_PARTY_DIR%" mkdir "%THIRD_PARTY_DIR%"
if exist "%TMP_DIR%" rmdir /s /q "%TMP_DIR%"
mkdir "%TMP_DIR%"

echo [2/5] Download %SLANG_FILE% ...
curl -L --fail --retry 3 -o "%ZIP_PATH%" "%DOWNLOAD_URL%"
if errorlevel 1 (
  echo [ERROR] Download failed: %DOWNLOAD_URL%
  exit /b 1
)

echo [3/5] Clean old Slang dir...
if exist "%SLANG_DIR%" rmdir /s /q "%SLANG_DIR%"
mkdir "%SLANG_DIR%"

echo [4/5] Extract zip...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Expand-Archive -Path '%ZIP_PATH%' -DestinationPath '%TMP_DIR%' -Force"
if errorlevel 1 (
  echo [ERROR] Extract failed.
  exit /b 1
)

REM Some archives contain a top-level folder, some don't.
REM Copy everything under TMP_DIR into SLANG_DIR.
xcopy "%TMP_DIR%\*" "%SLANG_DIR%\" /E /I /Y >nul
if errorlevel 1 (
  echo [ERROR] Copy extracted files failed.
  exit /b 1
)

echo [5/5] Cleanup temp files...
rmdir /s /q "%TMP_DIR%"
if "%KEEP_ZIP%"=="0" (
  del /q "%ZIP_PATH%"
)

echo.
echo [OK] Slang is ready at:
echo      %SLANG_DIR%
echo.

exit /b 0
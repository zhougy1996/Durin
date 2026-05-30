@echo off
:: Download.bat -- A utility script for downloading and extracting ZIP files for third-party dependencies.

:: This helper is kept for package-style dependencies that still need manual download/extract behavior.
:: Shared source-built dependencies should use the bootstrap install flow under Engine/Scripts/Bootstrap instead of ad-hoc in-project integration.

call "%~dp0\..\Env.bat"

set "TARGET_LABEL=%~1"
if "!TARGET_LABEL:~0,1!"==":" (
    shift
    goto %TARGET_LABEL%
)

echo Error: Please call this script with a function label, e.g., call Download.bat :DownloadAndExtract
exit /b 1

:: ============================================================
:: Function: DownloadAndExtract
:: Description: Download a ZIP file from a URL, extract it to a target directory, and clean up.
:: ============================================================
:DownloadAndExtract
set "LIB_NAME=%~1"
set "LIB_VERSION=%~2"
set "URL=%~3"
set "ZIP_NAME=%~4"

echo LIB_NAME=%LIB_NAME%

:: Define internal paths
set "TARGET_DIR=%THIRD_PARTY_DIR%\%LIB_NAME%"
set "ZIP_PATH=%THIRD_PARTY_DIR%\%ZIP_NAME%"
set "TMP_DIR=%THIRD_PARTY_DIR%\_%LIB_NAME%_tmp"

echo --------------------------------------------------------
echo Processing [%LIB_NAME%] version %LIB_VERSION%
echo --------------------------------------------------------

:: If the target directory already exists, skip the download and extraction. (optionally)
if exist "%TARGET_DIR%" (echo [%LIB_NAME%] already exists, skipping. & exit /b 0)

echo [1/4] Downloading from %URL%...
curl -L --fail --retry 3 -o "%ZIP_PATH%" "%URL%"
if errorlevel 1 (
    echo [ERROR] Failed to download %LIB_NAME%
    exit /b 1
)

echo [2/4] Preparing directories...
if exist "%TMP_DIR%" rmdir /s /q "%TMP_DIR%"
mkdir "%TMP_DIR%"

echo [3/4] Extracting to %TARGET_DIR%...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Expand-Archive -Path '%ZIP_PATH%' -DestinationPath '%TMP_DIR%' -Force"
if errorlevel 1 (
    echo [ERROR] Extraction failed for %LIB_NAME%
    exit /b 1
)

:: Clear the target directory and copy extracted files from temp directory.
:: Some archives may contain a top-level folder, so we copy everything under TMP_DIR to TARGET_DIR.
if exist "%TARGET_DIR%" rmdir /s /q "%TARGET_DIR%"
mkdir "%TARGET_DIR%"
xcopy "%TMP_DIR%\*" "%TARGET_DIR%\" /E /I /Y >nul

echo [4/4] Cleanup...
rmdir /s /q "%TMP_DIR%"
del /q "%ZIP_PATH%"

echo [OK] %LIB_NAME% is ready.
echo.
exit /b 0

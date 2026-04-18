@echo off
setlocal enabledelayedexpansion

set DOWNLOAD_UTILS="%~dp0\..\Utils\Download.bat"

set "LIB_NAME=slang"
set "VERSION=2026.5.2"
set "ZIP_FILE=slang-%VERSION%-windows-x86_64.zip"
set "DOWNLOAD_URL=https://github.com/shader-slang/slang/releases/download/v%VERSION%/%ZIP_FILE%"

call %DOWNLOAD_UTILS% :DownloadAndExtract "%LIB_NAME%" "%VERSION%" "%DOWNLOAD_URL%" "%ZIP_FILE%"

exit /b 0
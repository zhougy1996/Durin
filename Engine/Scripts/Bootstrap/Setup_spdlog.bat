@echo off
setlocal enabledelayedexpansion

set DOWNLOAD_UTILS="%~dp0\..\Utils\Download.bat"

set "LIB_NAME=spdlog"
set "VERSION=1.17.0"
set "ZIP_FILE=v%VERSION%.zip"
set "DOWNLOAD_URL=https://github.com/gabime/spdlog/archive/refs/tags/%ZIP_FILE%"

call %DOWNLOAD_UTILS% :DownloadAndExtract "%LIB_NAME%" "%VERSION%" "%DOWNLOAD_URL%" "%ZIP_FILE%"

pause
exit /b 0
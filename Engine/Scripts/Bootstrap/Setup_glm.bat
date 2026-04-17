@echo off
setlocal enabledelayedexpansion

set DOWNLOAD_UTILS="%~dp0\..\Utils\Download.bat"

set "LIB_NAME=glm"
set "VERSION=1.0.3"
set "ZIP_FILE=glm-%VERSION%.zip"
set "DOWNLOAD_URL=https://github.com/g-truc/glm/releases/download/%VERSION%/%ZIP_FILE%"

call %DOWNLOAD_UTILS% :DownloadAndExtract "%LIB_NAME%" "%VERSION%" "%DOWNLOAD_URL%" "%ZIP_FILE%"

pause
exit /b 0
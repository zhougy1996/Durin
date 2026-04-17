@echo off
setlocal enabledelayedexpansion

set DOWNLOAD_UTILS="%~dp0\..\Utils\Download.bat"

set "LIB_NAME=glfw"
set "VERSION=3.4"
set "ZIP_FILE=glfw-%VERSION%.bin.WIN64.zip"
set "DOWNLOAD_URL=https://github.com/glfw/glfw/releases/download/%VERSION%/%ZIP_FILE%"

call %DOWNLOAD_UTILS% :DownloadAndExtract "%LIB_NAME%" "%VERSION%" "%DOWNLOAD_URL%" "%ZIP_FILE%"

pause
exit /b 0
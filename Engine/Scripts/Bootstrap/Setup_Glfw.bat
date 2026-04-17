@echo off
setlocal enabledelayedexpansion

set DOWNLOAD_UTILS="%~dp0\..\Utils\Download.bat"

set "GLFW_VERSION=3.4"
set "ZIP_FILE=glfw-%GLFW_VERSION%.bin.WIN64.zip"
set "DOWNLOAD_URL=https://github.com/glfw/glfw/releases/download/%GLFW_VERSION%/%ZIP_FILE%"

call %DOWNLOAD_UTILS% :DownloadAndExtract "glfw" "%GLFW_VERSION%" "%DOWNLOAD_URL%" "%ZIP_FILE%"

pause
exit /b 0
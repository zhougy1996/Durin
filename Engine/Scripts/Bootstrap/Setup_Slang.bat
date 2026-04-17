@echo off
setlocal enabledelayedexpansion

set DOWNLOAD_UTILS="%~dp0\..\Utils\Download.bat"

set SLANG_VERSION=2026.5.2
set ZIP_FILE=slang-%SLANG_VERSION%-windows-x86_64.zip
set DOWNLOAD_URL=https://github.com/shader-slang/slang/releases/download/v%SLANG_VERSION%/%SLANG_FILE%

call %DOWNLOAD_UTILS% :DownloadAndExtract "slang" "%SLANG_VERSION%" "%DOWNLOAD_URL%" "%ZIP_FILE%"

exit /b 0
@echo off
setlocal enabledelayedexpansion

for %%I in ("%~dp0\..\Utils\Download.bat") do set "DOWNLOAD_UTILS=%%~fI"
for %%I in ("%~dp0\..\..\Source\ThirdParty\slang") do set "SLANG_ROOT=%%~fI"
set "SLANG_DLL=%SLANG_ROOT%\bin\slang.dll"
set "SLANG_COMPILER_DLL=%SLANG_ROOT%\bin\slang-compiler.dll"

set "LIB_NAME=slang"
set "VERSION=2026.5.2"
set "ZIP_FILE=slang-%VERSION%-windows-x86_64.zip"
set "DOWNLOAD_URL=https://github.com/shader-slang/slang/releases/download/v%VERSION%/%ZIP_FILE%"

if exist "%SLANG_DLL%" if exist "%SLANG_COMPILER_DLL%" (
	echo slang is already installed at "%SLANG_ROOT%".
	exit /b 0
)

call "%DOWNLOAD_UTILS%" :DownloadAndExtract "%LIB_NAME%" "%VERSION%" "%DOWNLOAD_URL%" "%ZIP_FILE%"

exit /b 0

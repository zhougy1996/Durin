@echo off
setlocal

set SPDLOG_DIR=..\Source\ThirdParty\spdlog
set SPDLOG_BUILD_DIR=%SPDLOG_DIR%\build

set VS_GENERATOR="Visual Studio 17 2022"

if exist "%SPDLOG_BUILD_DIR%" (
    rd /s /q "%SPDLOG_BUILD_DIR%"
)

mkdir "%SPDLOG_BUILD_DIR%"
cd "%SPDLOG_BUILD_DIR%"

cmake -G %VS_GENERATOR% -A x64 ".."

echo Building spdlog Debug configuration...
msbuild spdlog.sln /p:Configuration=Debug /m

echo Building spdlog Release configuration...
msbuild spdlog.sln /p:Configuration=Release /m

endlocal
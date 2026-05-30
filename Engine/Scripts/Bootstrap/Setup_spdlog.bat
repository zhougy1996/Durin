@echo off
setlocal

for %%I in ("%~dp0\..\..\CMake\ThirdParty\spdlog") do set "SPDLOG_CMAKE_DIR=%%~fI"
for %%I in ("%~dp0\..\..\Source\ThirdParty\spdlog") do set "SPDLOG_SOURCE_DIR=%%~fI"
set "SPDLOG_GIT_URL=https://github.com/gabime/spdlog.git"
set "SPDLOG_GIT_TAG=v1.17.0"
set "DEFAULT_CONFIGS=Debug Release"

call "%~dp0\Setup_installed_thirdparty.bat" :ResolveConfigs "%~1" "%DEFAULT_CONFIGS%" "spdlog"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_installed_thirdparty.bat" :RequireTool cmake "CMake was not found in PATH. Please install CMake or run from a shell that has it configured."
if errorlevel 1 exit /b 1

call :EnsureSpdlogSource
if errorlevel 1 exit /b 1

for %%C in (%CONFIGS%) do (
	call :EnsureSpdlogInstalled "%%C"
	if errorlevel 1 exit /b 1
)

exit /b 0

:EnsureSpdlogSource
call "%~dp0\Setup_installed_thirdparty.bat" :EnsureGitSource "%SPDLOG_SOURCE_DIR%" "CMakeLists.txt" "spdlog" "%SPDLOG_GIT_URL%" "%SPDLOG_GIT_TAG%"
exit /b %errorlevel%

:EnsureSpdlogInstalled
set "CONFIG=%~1"
call "%~dp0\Setup_installed_thirdparty.bat" :InstallPackage "spdlog" "spdlog" "%SPDLOG_CMAKE_DIR%" "%CONFIG%" "include\spdlog\spdlog.h lib\spdlog.lib"
if not errorlevel 1 exit /b 0

call "%~dp0\Setup_installed_thirdparty.bat" :InstallPackage "spdlog" "spdlog" "%SPDLOG_CMAKE_DIR%" "%CONFIG%" "include\spdlog\spdlog.h lib\spdlogd.lib"
exit /b %errorlevel%

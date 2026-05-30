@echo off
setlocal

for %%I in ("%~dp0\..\..\CMake\ThirdParty\rapidyaml") do set "RAPIDYAML_CMAKE_DIR=%%~fI"
for %%I in ("%~dp0\..\..\Source\ThirdParty\rapidyaml") do set "RAPIDYAML_SOURCE_DIR=%%~fI"
set "RAPIDYAML_GIT_URL=https://github.com/biojppm/rapidyaml.git"
set "RAPIDYAML_GIT_TAG=v0.11.1"
set "DEFAULT_CONFIGS=Debug Release"

call "%~dp0\Setup_installed_thirdparty.bat" :ResolveConfigs "%~1" "%DEFAULT_CONFIGS%" "rapidyaml"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_installed_thirdparty.bat" :RequireTool cmake "CMake was not found in PATH. Please install CMake or run from a shell that has it configured."
if errorlevel 1 exit /b 1

call :EnsureRapidyamlSource
if errorlevel 1 exit /b 1

for %%C in (%CONFIGS%) do (
	call :EnsureRapidyamlInstalled "%%C"
	if errorlevel 1 exit /b 1
)

exit /b 0

:EnsureRapidyamlSource
call "%~dp0\Setup_installed_thirdparty.bat" :EnsureGitSource "%RAPIDYAML_SOURCE_DIR%" "CMakeLists.txt" "rapidyaml" "%RAPIDYAML_GIT_URL%" "%RAPIDYAML_GIT_TAG%"
exit /b %errorlevel%

:EnsureRapidyamlInstalled
set "CONFIG=%~1"
call "%~dp0\Setup_installed_thirdparty.bat" :InstallPackage "rapidyaml" "rapidyaml" "%RAPIDYAML_CMAKE_DIR%" "%CONFIG%" "include\ryml.hpp lib\ryml.lib"
if not errorlevel 1 exit /b 0

call "%~dp0\Setup_installed_thirdparty.bat" :InstallPackage "rapidyaml" "rapidyaml" "%RAPIDYAML_CMAKE_DIR%" "%CONFIG%" "include\ryml.hpp lib\rapidyaml.lib"
exit /b %errorlevel%

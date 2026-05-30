@echo off
setlocal

for %%I in ("%~dp0\..\..\CMake\ThirdParty\assimp") do set "ASSIMP_CMAKE_DIR=%%~fI"
for %%I in ("%~dp0\..\..\Source\ThirdParty\assimp") do set "ASSIMP_SOURCE_DIR=%%~fI"
set "ASSIMP_GIT_URL=https://github.com/assimp/assimp.git"
set "ASSIMP_GIT_TAG=v6.0.4"
set "DEFAULT_CONFIGS=Debug Release"

call "%~dp0\Setup_installed_thirdparty.bat" :ResolveConfigs "%~1" "%DEFAULT_CONFIGS%" "assimp"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_installed_thirdparty.bat" :RequireTool cmake "CMake was not found in PATH. Please install CMake or run from a shell that has it configured."
if errorlevel 1 exit /b 1

call :EnsureAssimpSource
if errorlevel 1 exit /b 1

for %%C in (%CONFIGS%) do (
	call :EnsureAssimpInstalled "%%C"
	if errorlevel 1 exit /b 1
)

exit /b 0

:EnsureAssimpSource
call "%~dp0\Setup_installed_thirdparty.bat" :EnsureGitSource "%ASSIMP_SOURCE_DIR%" "CMakeLists.txt" "assimp" "%ASSIMP_GIT_URL%" "%ASSIMP_GIT_TAG%"
exit /b %errorlevel%

:EnsureAssimpInstalled
set "CONFIG=%~1"
call "%~dp0\Setup_installed_thirdparty.bat" :InstallPackage "assimp" "assimp" "%ASSIMP_CMAKE_DIR%" "%CONFIG%" "include\assimp\Importer.hpp lib\assimp-vc143-mt.lib bin\assimp-vc143-mt.dll"
if not errorlevel 1 exit /b 0

call "%~dp0\Setup_installed_thirdparty.bat" :InstallPackage "assimp" "assimp" "%ASSIMP_CMAKE_DIR%" "%CONFIG%" "include\assimp\Importer.hpp lib\assimp.lib bin\assimp.dll"
exit /b %errorlevel%

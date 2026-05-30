@echo off
setlocal

for %%I in ("%~dp0\..\..\CMake\ThirdParty\glfw") do set "GLFW_CMAKE_DIR=%%~fI"
for %%I in ("%~dp0\..\..\Source\ThirdParty\glfw") do set "GLFW_SOURCE_DIR=%%~fI"
set "GLFW_GIT_URL=https://github.com/glfw/glfw.git"
set "GLFW_GIT_TAG=3.4"
set "DEFAULT_CONFIGS=Debug Release"

call "%~dp0\Setup_installed_thirdparty.bat" :ResolveConfigs "%~1" "%DEFAULT_CONFIGS%" "GLFW"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_installed_thirdparty.bat" :RequireTool cmake "CMake was not found in PATH. Please install CMake or run from a shell that has it configured."
if errorlevel 1 exit /b 1

call :EnsureGlfwSource
if errorlevel 1 exit /b 1

for %%C in (%CONFIGS%) do (
	call :EnsureGlfwInstalled "%%C"
	if errorlevel 1 exit /b 1
)

exit /b 0

:EnsureGlfwSource
call "%~dp0\Setup_installed_thirdparty.bat" :EnsureGitSource "%GLFW_SOURCE_DIR%" "CMakeLists.txt" "GLFW" "%GLFW_GIT_URL%" "%GLFW_GIT_TAG%"
exit /b %errorlevel%

:EnsureGlfwInstalled
set "CONFIG=%~1"
call "%~dp0\Setup_installed_thirdparty.bat" :InstallPackage "glfw" "GLFW" "%GLFW_CMAKE_DIR%" "%CONFIG%" "include\GLFW\glfw3.h lib\glfw3.lib"
if not errorlevel 1 exit /b 0

call "%~dp0\Setup_installed_thirdparty.bat" :InstallPackage "glfw" "GLFW" "%GLFW_CMAKE_DIR%" "%CONFIG%" "include\GLFW\glfw3.h lib\glfw.lib"
exit /b %errorlevel%

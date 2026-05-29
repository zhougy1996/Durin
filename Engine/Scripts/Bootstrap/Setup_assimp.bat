@echo off
setlocal

for %%I in ("%~dp0") do set "SCRIPT_DIR=%%~fI"
for %%I in ("%~dp0\..\..\CMake\ThirdParty\assimp") do set "ASSIMP_CMAKE_DIR=%%~fI"
for %%I in ("%~dp0\..\..\Source\ThirdParty\assimp") do set "ASSIMP_SOURCE_DIR=%%~fI"
set "ASSIMP_GIT_URL=https://github.com/assimp/assimp.git"
set "ASSIMP_GIT_TAG=v6.0.4"
set "DEFAULT_CONFIGS=Debug Release"

if /i "%~1"=="Debug" (
	set "CONFIGS=Debug"
) else if /i "%~1"=="Release" (
	set "CONFIGS=Release"
) else if /i "%~1"=="All" (
	set "CONFIGS=%DEFAULT_CONFIGS%"
) else if "%~1"=="" (
	set "CONFIGS=%DEFAULT_CONFIGS%"
) else (
	echo Unsupported assimp config "%~1". Use Debug, Release, or All.
	exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
	echo CMake was not found in PATH. Please install CMake or run from a shell that has it configured.
	exit /b 1
)

call :EnsureAssimpSource
if errorlevel 1 exit /b 1

for %%C in (%CONFIGS%) do (
	call :EnsureAssimpInstalled "%%C"
	if errorlevel 1 exit /b 1
)

exit /b 0

:EnsureAssimpSource
if exist "%ASSIMP_SOURCE_DIR%\CMakeLists.txt" (
	echo assimp source is already available at "%ASSIMP_SOURCE_DIR%".
	exit /b 0
)

where git >nul 2>nul
if errorlevel 1 (
	echo Git was not found in PATH. Please install Git or run from a shell that has it configured.
	exit /b 1
)

if exist "%ASSIMP_SOURCE_DIR%" (
	echo assimp source directory exists but is incomplete: "%ASSIMP_SOURCE_DIR%".
	echo Please remove it and run Setup.bat again, or repair it manually.
	exit /b 1
)

echo Cloning assimp source %ASSIMP_GIT_TAG% into "%ASSIMP_SOURCE_DIR%"...
git clone --branch %ASSIMP_GIT_TAG% --depth 1 "%ASSIMP_GIT_URL%" "%ASSIMP_SOURCE_DIR%"
if errorlevel 1 (
	echo Failed to clone assimp source.
	exit /b 1
)

if exist "%ASSIMP_SOURCE_DIR%\CMakeLists.txt" (
	echo assimp source cloned successfully.
	exit /b 0
)

echo assimp source clone completed, but "%ASSIMP_SOURCE_DIR%\CMakeLists.txt" was not found.
exit /b 1

:EnsureAssimpInstalled
set "CONFIG=%~1"
for %%I in ("%SCRIPT_DIR%\..\..\..\Build\ThirdParty\Install\Win64\%CONFIG%\assimp") do set "INSTALL_DIR=%%~fI"
set "ASSIMP_HEADER=%INSTALL_DIR%\include\assimp\Importer.hpp"
set "ASSIMP_LIB=%INSTALL_DIR%\lib\assimp-vc143-mt.lib"
set "ASSIMP_DLL=%INSTALL_DIR%\bin\assimp-vc143-mt.dll"

if exist "%ASSIMP_HEADER%" if exist "%ASSIMP_LIB%" if exist "%ASSIMP_DLL%" (
	echo assimp %CONFIG% is already installed at "%INSTALL_DIR%".
	exit /b 0
)

echo Installing assimp %CONFIG%...
pushd "%ASSIMP_CMAKE_DIR%" >nul
cmake --preset Win64-%CONFIG%-assimp
if errorlevel 1 (
	popd >nul
	echo Failed to configure assimp %CONFIG%.
	exit /b 1
)

cmake --build --preset Win64-%CONFIG%-assimp
if errorlevel 1 (
	popd >nul
	echo Failed to build/install assimp %CONFIG%.
	exit /b 1
)
popd >nul

if exist "%ASSIMP_HEADER%" if exist "%ASSIMP_LIB%" if exist "%ASSIMP_DLL%" (
	echo assimp %CONFIG% installed successfully.
	exit /b 0
)

echo assimp %CONFIG% install completed, but expected files were not found in "%INSTALL_DIR%".
exit /b 1

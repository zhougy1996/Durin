@echo off
if "%~1"=="" goto :eof
set "TP_HELPER_LABEL=%~1"
shift /1
goto %TP_HELPER_LABEL%

:ResolveConfigs
set "CONFIGS="
set "REQUESTED_CONFIG=%~1"
set "DEFAULT_CONFIGS=%~2"
set "PACKAGE_DISPLAY_NAME=%~3"

if /i "%REQUESTED_CONFIG%"=="Debug" (
	set "CONFIGS=Debug"
) else if /i "%REQUESTED_CONFIG%"=="Release" (
	set "CONFIGS=Release"
) else if /i "%REQUESTED_CONFIG%"=="All" (
	set "CONFIGS=%DEFAULT_CONFIGS%"
) else if "%REQUESTED_CONFIG%"=="" (
	set "CONFIGS=%DEFAULT_CONFIGS%"
) else (
	echo Unsupported %PACKAGE_DISPLAY_NAME% config "%REQUESTED_CONFIG%". Use Debug, Release, or All.
	exit /b 1
)

exit /b 0

:RequireTool
where "%~1" >nul 2>nul
if errorlevel 1 (
	echo %~2
	exit /b 1
)
exit /b 0

:EnsureGitSource
set "TP_SOURCE_DIR=%~1"
set "TP_SOURCE_MARKER=%~2"
set "TP_PACKAGE_DISPLAY_NAME=%~3"
set "TP_GIT_URL=%~4"
set "TP_GIT_TAG=%~5"

if exist "%TP_SOURCE_DIR%\%TP_SOURCE_MARKER%" (
	echo %TP_PACKAGE_DISPLAY_NAME% source is already available at "%TP_SOURCE_DIR%".
	exit /b 0
)

call :RequireTool git "Git was not found in PATH. Please install Git or run from a shell that has it configured."
if errorlevel 1 exit /b 1

if exist "%TP_SOURCE_DIR%" (
	echo %TP_PACKAGE_DISPLAY_NAME% source directory exists but is incomplete: "%TP_SOURCE_DIR%".
	echo Please remove it and run Setup.bat again, or repair it manually.
	exit /b 1
)

echo Cloning %TP_PACKAGE_DISPLAY_NAME% source %TP_GIT_TAG% into "%TP_SOURCE_DIR%"...
git clone --branch %TP_GIT_TAG% --depth 1 "%TP_GIT_URL%" "%TP_SOURCE_DIR%"
if errorlevel 1 (
	echo Failed to clone %TP_PACKAGE_DISPLAY_NAME% source.
	exit /b 1
)

if exist "%TP_SOURCE_DIR%\%TP_SOURCE_MARKER%" (
	echo %TP_PACKAGE_DISPLAY_NAME% source cloned successfully.
	exit /b 0
)

echo %TP_PACKAGE_DISPLAY_NAME% source clone completed, but "%TP_SOURCE_DIR%\%TP_SOURCE_MARKER%" was not found.
exit /b 1

:InstallPackage
set "TP_PACKAGE_NAME=%~1"
set "TP_PACKAGE_DISPLAY_NAME=%~2"
set "TP_CMAKE_DIR=%~3"
set "TP_CONFIG=%~4"
set "TP_REQUIRED_FILES=%~5"

for %%I in ("%~dp0") do set "TP_SCRIPT_DIR=%%~fI"
for %%I in ("%TP_SCRIPT_DIR%\..\..\..\Build\ThirdParty\Install\Win64\%TP_CONFIG%\%TP_PACKAGE_NAME%") do set "TP_INSTALL_DIR=%%~fI"
for %%I in ("%TP_SCRIPT_DIR%\..\..\..\Build\ThirdParty\Build\Win64-%TP_CONFIG%-%TP_PACKAGE_NAME%") do set "TP_BUILD_DIR=%%~fI"

call :VerifyInstalledFiles "%TP_INSTALL_DIR%" "%TP_REQUIRED_FILES%"
if not errorlevel 1 (
	echo %TP_PACKAGE_DISPLAY_NAME% %TP_CONFIG% is already installed at "%TP_INSTALL_DIR%".
	exit /b 0
)

echo Installing %TP_PACKAGE_DISPLAY_NAME% %TP_CONFIG%...
cmake -S "%TP_CMAKE_DIR%" -B "%TP_BUILD_DIR%" -D CMAKE_BUILD_TYPE=%TP_CONFIG% -D CMAKE_INSTALL_PREFIX="%TP_INSTALL_DIR%"
if errorlevel 1 (
	echo Failed to configure %TP_PACKAGE_DISPLAY_NAME% %TP_CONFIG%.
	exit /b 1
)

cmake --build "%TP_BUILD_DIR%" --target install
if errorlevel 1 (
	echo Failed to build/install %TP_PACKAGE_DISPLAY_NAME% %TP_CONFIG%.
	exit /b 1
)

call :VerifyInstalledFiles "%TP_INSTALL_DIR%" "%TP_REQUIRED_FILES%"
if not errorlevel 1 (
	echo %TP_PACKAGE_DISPLAY_NAME% %TP_CONFIG% installed successfully.
	exit /b 0
)

echo %TP_PACKAGE_DISPLAY_NAME% %TP_CONFIG% install completed, but expected files were not found in "%TP_INSTALL_DIR%".
exit /b 1

:VerifyInstalledFiles
set "TP_VERIFY_ROOT=%~1"
set "TP_VERIFY_FILES=%~2"

for %%F in (%TP_VERIFY_FILES%) do (
	if not exist "%TP_VERIFY_ROOT%\%%~F" exit /b 1
)

exit /b 0

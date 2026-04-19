@echo off

net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -ExecutionPolicy Bypass ^
    -Command "Start-Process wt -ArgumentList '-d %cd% cmd /k ""%~f0 %*""' -Verb RunAs"
    exit
)

echo param1=%1
set CONFIG=%~1
echo CONFIG=%CONFIG%
if "%CONFIG%"=="" set CONFIG=Debug

call "%~dp0\..\Env.bat"

vcperf /stopnoanalyze >nul 2>&1
xperf -stop >nul 2>&1

echo CONFIG=%CONFIG%
vcperf /start Doge_%CONFIG%

:: Ninja
set BUILD_DIR="%ROOT_DIR%\Build\x64-%CONFIG%"
cmake --build "%BUILD_DIR%" --target all -j 18

vcperf /stop Doge_%CONFIG% "%BUILD_DIR%\Doge.etl"

pause
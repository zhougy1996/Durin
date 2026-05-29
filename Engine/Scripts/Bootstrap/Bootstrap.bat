@echo off
setlocal

:: Durin use FetchContent in CMakeLists.txt to download and extract the dependencies.
:: However, some dependencies may be not based on CMakeLists.txt, so we can keep this batch file as a backup plan in case we need to manually download the dependencies.
:: Some dependencies may take a very long time ang very sophisticated steps to build, such as slang, so we can use this batch file to download the pre-built binaries of these dependencies.

call "%~dp0\Setup_slang.bat"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_assimp.bat"
if errorlevel 1 exit /b 1

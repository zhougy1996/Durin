@echo off
setlocal

:: Currently not use this batch file to download dependencies, since we can directly use FetchContent in CMakeLists.txt to download and extract the dependencies.
:: However, we can keep this batch file as a backup plan in case we need to manually download the dependencies in the future.
@REM call "%~dp0\Setup_spdlog.bat"
@REM call "%~dp0\Setup_glm.bat"
@REM call "%~dp0\Setup_glfw.bat"
@REM call "%~dp0\Setup_slang.bat"

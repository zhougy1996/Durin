@echo off
setlocal

:: Durin mixes source-built and prebuilt third-party dependencies.
:: Use this script to prepare reusable third-party install trees and downloaded binaries ahead of project configuration.
:: Shared source-built libraries are installed via the Setup_<Library>.bat scripts with direct cmake -S/-B/-DCMAKE_INSTALL_PREFIX invocations.

call "%~dp0\Setup_slang.bat"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_glm.bat"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_googletest.bat"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_spdlog.bat"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_glfw.bat"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_rapidyaml.bat"
if errorlevel 1 exit /b 1

call "%~dp0\Setup_assimp.bat"
if errorlevel 1 exit /b 1

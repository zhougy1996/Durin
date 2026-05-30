@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs spdlog %*
exit /b %errorlevel%

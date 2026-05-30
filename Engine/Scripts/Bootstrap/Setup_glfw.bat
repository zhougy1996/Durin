@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs glfw %*
exit /b %errorlevel%

@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs tracy %*
exit /b %errorlevel%

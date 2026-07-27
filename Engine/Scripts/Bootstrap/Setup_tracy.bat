@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs tracy,tracy-tools %*
exit /b %errorlevel%

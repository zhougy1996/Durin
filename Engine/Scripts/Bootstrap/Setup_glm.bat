@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs glm %*
exit /b %errorlevel%

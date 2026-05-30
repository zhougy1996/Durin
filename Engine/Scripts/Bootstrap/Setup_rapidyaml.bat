@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs rapidyaml %*
exit /b %errorlevel%

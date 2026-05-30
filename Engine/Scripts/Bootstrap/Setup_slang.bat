@echo off
setlocal
call "%~dp0\RunBootstrap.bat" --libs slang %*
exit /b %errorlevel%

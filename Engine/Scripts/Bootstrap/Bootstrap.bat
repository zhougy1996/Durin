@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --all --with-tests %*
exit /b %errorlevel%

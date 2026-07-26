@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --all --with-tests --with-development %*
exit /b %errorlevel%

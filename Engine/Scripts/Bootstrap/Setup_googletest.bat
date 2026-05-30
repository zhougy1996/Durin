@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs googletest %*
exit /b %errorlevel%

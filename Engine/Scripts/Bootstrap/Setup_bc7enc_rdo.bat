@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs bc7enc_rdo %*
exit /b %errorlevel%

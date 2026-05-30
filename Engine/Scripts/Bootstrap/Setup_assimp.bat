@echo off
setlocal

call "%~dp0\RunBootstrap.bat" --libs assimp %*
exit /b %errorlevel%

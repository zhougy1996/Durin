@echo off
setlocal

set "SCRIPT=%~dp0Engine\Scripts\Utils\OpenWorktrees.ps1"
pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
exit /b %ERRORLEVEL%

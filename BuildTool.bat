@echo off
setlocal

set "VSLANG=1033"
python "%~dp0Engine\Scripts\Build\agent_build.py" %*
exit /b %ERRORLEVEL%

@echo off
setlocal

set "ROOT=%~dp0"
set "SCRIPT=%ROOT%Engine\Scripts\Utils\worktree_tool.py"
set "PYTHON_EXE=%ROOT%.venv\Scripts\python.exe"

if exist "%PYTHON_EXE%" (
  call "%PYTHON_EXE%" "%SCRIPT%" %*
  exit /b %ERRORLEVEL%
)

where py >nul 2>nul
if not errorlevel 1 (
  py -3 "%SCRIPT%" %*
  exit /b %ERRORLEVEL%
)

where python >nul 2>nul
if errorlevel 1 (
  echo Python was not found through the py launcher or PATH, and "%PYTHON_EXE%" does not exist.
  exit /b 1
)

call python "%SCRIPT%" %*
exit /b %ERRORLEVEL%

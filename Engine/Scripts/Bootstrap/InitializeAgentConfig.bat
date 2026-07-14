@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..\..") do set "REPO_ROOT=%%~fI"
set "PYTHON_EXE=%REPO_ROOT%\.venv\Scripts\python.exe"

if exist "%PYTHON_EXE%" (
  call "%PYTHON_EXE%" "%SCRIPT_DIR%initialize_agent_config.py" %*
  exit /b %errorlevel%
)

where python >nul 2>nul
if errorlevel 1 (
  echo Python was not found in PATH and "%PYTHON_EXE%" does not exist.
  exit /b 1
)

call python "%SCRIPT_DIR%initialize_agent_config.py" %*
exit /b %errorlevel%

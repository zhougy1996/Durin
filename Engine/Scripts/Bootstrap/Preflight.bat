@echo off
setlocal

set "SCRIPT=%~dp0setup_preflight.py"

where py >nul 2>nul
if not errorlevel 1 (
  py -3 "%SCRIPT%"
  if errorlevel 1 exit /b 1
  exit /b 0
)

where python >nul 2>nul
if errorlevel 1 (
  echo Python 3.10 or newer was not found.
  echo Install Python from https://www.python.org/downloads/windows/ and enable the Python launcher or PATH option.
  exit /b 1
)

python "%SCRIPT%"
if errorlevel 1 exit /b 1
exit /b 0

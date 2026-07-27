@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "PRODUCT_DIR=%~dp0"
for %%I in ("%PRODUCT_DIR%..\..") do set "REPO_ROOT=%%~fI"
set "VENV_PYTHON=%REPO_ROOT%\.venv\Scripts\python.exe"
set "ENTRY_POINT=%PRODUCT_DIR%durin_dev_tool\__main__.py"

if exist "%VENV_PYTHON%" (
  "%VENV_PYTHON%" "%ENTRY_POINT%" %*
  exit /b !ERRORLEVEL!
)

where py >nul 2>&1
if not errorlevel 1 (
  py -3 "%ENTRY_POINT%" %*
  exit /b !ERRORLEVEL!
)

where python >nul 2>&1
if not errorlevel 1 (
  python "%ENTRY_POINT%" %*
  exit /b !ERRORLEVEL!
)

echo Python 3.10 or newer was not found. 1>&2
echo Install Python, enable the Python Launcher or add Python to PATH, then rerun DevTool. 1>&2
exit /b 1

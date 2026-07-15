@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..\..") do set "REPO_ROOT=%%~fI"
set "VENV_DIR=%REPO_ROOT%\.venv"
set "VENV_PYTHON=%VENV_DIR%\Scripts\python.exe"
set "REQUIREMENTS=%REPO_ROOT%\requirements.txt"

if not exist "%VENV_PYTHON%" goto create_venv
goto validate_python

:create_venv
echo Creating Python virtual environment at "%VENV_DIR%"...
where py >nul 2>nul
if not errorlevel 1 (
  py -3 -m venv "%VENV_DIR%"
  if errorlevel 1 goto create_failed
  goto validate_python
)

where python >nul 2>nul
if errorlevel 1 goto python_missing
python -m venv "%VENV_DIR%"
if errorlevel 1 goto create_failed

:validate_python
"%VENV_PYTHON%" -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
if errorlevel 1 (
  echo Durin requires Python 3.10 or newer. The environment at "%VENV_DIR%" uses:
  "%VENV_PYTHON%" --version
  echo Remove .venv, install a supported Python, and run Setup.bat again.
  exit /b 1
)

echo Installing Python dependencies from "%REQUIREMENTS%"...
"%VENV_PYTHON%" -m pip install --disable-pip-version-check --requirement "%REQUIREMENTS%"
if errorlevel 1 exit /b %errorlevel%

"%VENV_PYTHON%" -c "import clang.cindex; from clang import native; from pathlib import Path; raise SystemExit(0 if (Path(native.__file__).parent / 'libclang.dll').is_file() else 1)"
if errorlevel 1 (
  echo libclang was installed, but its Windows native library could not be found.
  exit /b 1
)

echo Python environment is ready: "%VENV_PYTHON%"
exit /b 0

:python_missing
echo Python 3.10 or newer was not found.
echo Install Python from https://www.python.org/downloads/windows/ and enable the Python launcher or PATH option.
exit /b 1

:create_failed
echo Failed to create "%VENV_DIR%". Make sure the installed Python includes the venv module.
exit /b 1

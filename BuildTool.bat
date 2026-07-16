@echo off
setlocal

set "VSLANG=1033"
set "PYTHON_EXE=%~dp0.venv\Scripts\python.exe"
if not exist "%PYTHON_EXE%" (
  echo Durin's Python environment is missing. Run Setup.bat first.
  exit /b 1
)

"%PYTHON_EXE%" "%~dp0Engine\Scripts\Build\durin_build_tool\__main__.py" %*
exit /b %ERRORLEVEL%

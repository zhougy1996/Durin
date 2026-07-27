@echo off
setlocal

set "VSLANG=1033"
set "PYTHON_EXE=%~dp0.venv\Scripts\python.exe"
if not exist "%PYTHON_EXE%" (
  echo Durin's Python environment is missing.
  echo Run Setup.bat in the main checkout or WorktreeTool prepare in a linked worktree.
  exit /b 1
)

"%PYTHON_EXE%" "%~dp0Tools\BuildTool\durin_build_tool\__main__.py" %*
exit /b %ERRORLEVEL%

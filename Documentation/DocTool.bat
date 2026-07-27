@echo off
setlocal

set "PYTHON_EXE=%~dp0..\.venv\Scripts\python.exe"
if not exist "%PYTHON_EXE%" (
  echo Durin's Python environment is missing.
  echo Run Setup.bat in the main checkout or WorktreeTool prepare in a linked worktree.
  exit /b 1
)

pushd "%~dp0" >nul
"%PYTHON_EXE%" -m doc_tool %*
set "EXIT_CODE=%ERRORLEVEL%"
popd >nul
exit /b %EXIT_CODE%

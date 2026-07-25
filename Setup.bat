@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "GIT_ENTRY=%ROOT%\.git"
set "GIT_DIR_VALUE="
set "GIT_DIR_ABS="
set "EXIT_CODE=0"
set "PAUSE_AT_END=1"

if /I "%~1"=="--no-pause" (
  set "PAUSE_AT_END=0"
  shift
)

if not "%~1"=="" (
  echo Unknown Setup argument: %~1
  exit /b 2
)

if exist "%GIT_ENTRY%\" goto bootstrap
if not exist "%GIT_ENTRY%" goto bootstrap

for /f "usebackq tokens=1,* delims=:" %%A in ("%GIT_ENTRY%") do (
  if /I "%%A"=="gitdir" (
    set "GIT_DIR_VALUE=%%B"
    goto resolve_gitdir
  )
)
goto bootstrap

:resolve_gitdir
for /f "tokens=* delims= " %%I in ("!GIT_DIR_VALUE!") do set "GIT_DIR_VALUE=%%I"
if not defined GIT_DIR_VALUE goto bootstrap

if "!GIT_DIR_VALUE:~1,1!"==":" (
  set "GIT_DIR_ABS=!GIT_DIR_VALUE!"
) else if "!GIT_DIR_VALUE:~0,2!"=="\\" (
  set "GIT_DIR_ABS=!GIT_DIR_VALUE!"
) else (
  for %%I in ("%ROOT%!GIT_DIR_VALUE!") do set "GIT_DIR_ABS=%%~fI"
)

if not exist "!GIT_DIR_ABS!\commondir" goto bootstrap
goto prepare_worktree

:bootstrap
call "%ROOT%Engine\Scripts\Bootstrap\InitializeAgentConfig.bat"
if errorlevel 1 (
  set "EXIT_CODE=!errorlevel!"
  goto end
)
call "%ROOT%Engine\Scripts\Bootstrap\Preflight.bat"
if errorlevel 1 (
  set "EXIT_CODE=!errorlevel!"
  goto end
)
call "%ROOT%Engine\Scripts\Bootstrap\SetupPython.bat"
if errorlevel 1 (
  set "EXIT_CODE=!errorlevel!"
  goto end
)
call "%ROOT%Engine\Scripts\Bootstrap\Bootstrap.bat"
set "EXIT_CODE=!errorlevel!"
goto end

:prepare_worktree
call "%ROOT%Engine\Scripts\Bootstrap\PrepareWorktree.bat"
if errorlevel 1 (
  set "EXIT_CODE=!errorlevel!"
  goto end
)
call "%ROOT%Engine\Scripts\Bootstrap\Preflight.bat"
set "EXIT_CODE=!errorlevel!"

:end
if "!PAUSE_AT_END!"=="1" pause
exit /b !EXIT_CODE!

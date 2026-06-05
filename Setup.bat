@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "GIT_ENTRY=%ROOT%\.git"
set "GIT_DIR_VALUE="
set "GIT_DIR_ABS="

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
call "%ROOT%Engine\Scripts\Bootstrap\Bootstrap.bat"
goto end

:prepare_worktree
call "%ROOT%Engine\Scripts\Bootstrap\PrepareWorktree.bat"

:end
pause

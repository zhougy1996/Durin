@echo off
setlocal

set "ROOT=%~dp0"
for %%I in ("%ROOT%") do set "ROOT_ABS=%%~fI"
set "GIT_DIR="
set "GIT_COMMON_DIR="
set "GIT_DIR_ABS="
set "GIT_COMMON_DIR_ABS="

if not exist "%ROOT%\.git" goto bootstrap

where git >nul 2>nul
if errorlevel 1 goto bootstrap

for /f "usebackq delims=" %%I in (`git -C "%ROOT%" rev-parse --git-dir 2^>nul`) do set "GIT_DIR=%%I"
for /f "usebackq delims=" %%I in (`git -C "%ROOT%" rev-parse --git-common-dir 2^>nul`) do set "GIT_COMMON_DIR=%%I"

if not defined GIT_DIR goto bootstrap
if not defined GIT_COMMON_DIR goto bootstrap

for %%I in ("%ROOT_ABS%%GIT_DIR%") do set "GIT_DIR_ABS=%%~fI"
for %%I in ("%ROOT_ABS%%GIT_COMMON_DIR%") do set "GIT_COMMON_DIR_ABS=%%~fI"

if not defined GIT_DIR_ABS goto bootstrap
if not defined GIT_COMMON_DIR_ABS goto bootstrap

if /I not "%GIT_DIR_ABS%"=="%GIT_COMMON_DIR_ABS%" goto prepare_worktree

:bootstrap
call "%ROOT%Engine\Scripts\Bootstrap\Bootstrap.bat"
goto end

:prepare_worktree
call "%ROOT%Engine\Scripts\Bootstrap\PrepareWorktree.bat"

:end
pause

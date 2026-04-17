@echo off
set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "ROOT_DIR=%%~fI"
set "ENGINE_DIR=%ROOT_DIR%\Engine"
set "THIRD_PARTY_DIR=%ENGINE_DIR%\Source\ThirdParty"


@echo off

for /f "usebackq tokens=*" %%i in (`
  "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
`) do (
  call "%%i\VC\Auxiliary\Build\vcvars64.bat"
)

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "ROOT_DIR=%%~fI"
set "ENGINE_DIR=%ROOT_DIR%\Engine"
set "THIRD_PARTY_DIR=%ENGINE_DIR%\Source\ThirdParty"


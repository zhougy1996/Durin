@echo off
setlocal

for %%I in ("%~dp0\..\..\Source\ThirdParty\glm") do set "GLM_SOURCE_DIR=%%~fI"
set "GLM_GIT_URL=https://github.com/g-truc/glm.git"
set "GLM_GIT_TAG=1.0.3"

call "%~dp0\Setup_installed_thirdparty.bat" :EnsureGitSource "%GLM_SOURCE_DIR%" "CMakeLists.txt" "GLM" "%GLM_GIT_URL%" "%GLM_GIT_TAG%"
exit /b %errorlevel%

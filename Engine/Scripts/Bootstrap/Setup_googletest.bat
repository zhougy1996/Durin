@echo off
setlocal

for %%I in ("%~dp0\..\..\Source\ThirdParty\googletest") do set "GOOGLETEST_SOURCE_DIR=%%~fI"
set "GOOGLETEST_GIT_URL=https://github.com/google/googletest.git"
set "GOOGLETEST_GIT_TAG=v1.17.0"

call "%~dp0\Setup_installed_thirdparty.bat" :EnsureGitSource "%GOOGLETEST_SOURCE_DIR%" "CMakeLists.txt" "googletest" "%GOOGLETEST_GIT_URL%" "%GOOGLETEST_GIT_TAG%"
exit /b %errorlevel%

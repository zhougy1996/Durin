@echo off
setlocal

set SCRIPT_DIR=Engine\Scripts

cd %SCRIPT_DIR%

rem call bat files
call PrepareVulkanModule.bat
call BuildGlfw.bat
call BuildSpdlog.bat

pause
endlocal
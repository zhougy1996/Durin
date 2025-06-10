@echo off
setlocal

set VULKAN_SDK_PATH=%VULKAN_SDK%

set SHADER_DIR=../shaders
set INPUT_DIR=%SHADER_DIR%/glsl
set OUTPUT_DIR=%SHADER_DIR%/spv

set PATH=%VULKAN_SDK_PATH%\Bin;%PATH%


REM Compile shaders
REM Calculate the number of files in the directory
set /a count=0
for %%f in (%INPUT_DIR%\*.vert) do (
    glslc.exe "%%f" -o "%OUTPUT_DIR%\%%~nf_vert.spv"
    set /a count+=1
)

for %%f in (%INPUT_DIR%\*.frag) do (
    glslc.exe "%%f" -o "%OUTPUT_DIR%\%%~nf_frag.spv"
    set /a count+=1
)
echo Compiled %count% shaders successfully.

endlocal
pause
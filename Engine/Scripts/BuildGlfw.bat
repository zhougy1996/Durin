@echo off
setlocal

set GLFW_DIR=..\Source\ThirdParty\glfw
set GLFW_BUILD_DIR=%GLFW_DIR%\build

set VS_GENERATOR="Visual Studio 17 2022"

if exist "%GLFW_BUILD_DIR%" (
    rd /s /q "%GLFW_BUILD_DIR%"
)

mkdir "%GLFW_BUILD_DIR%"
cd "%GLFW_BUILD_DIR%"

rem cmake -G %VS_GENERATOR% -A x64 -DBUILD_SHARED_LIBS=ON ..
cmake -G %VS_GENERATOR% -A x64 ..

echo Building GLFW Debug configuration...
msbuild GLFW.sln /p:Configuration=Debug /m

echo Building GLFW Release configuration...
msbuild GLFW.sln /p:Configuration=Release /m

endlocal

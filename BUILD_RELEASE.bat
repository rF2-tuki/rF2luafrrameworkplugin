@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
set "PROJECT=%ROOT%VC2022\rF2LuaFramework.vcxproj"

echo ==================================================
echo rFactor 2 Lua Framework - Release x64 build
echo ==================================================

if not exist "%PROJECT%" (
  echo [ERROR] Project file not found:
  echo "%PROJECT%"
  pause
  exit /b 2
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] Visual Studio Installer / vswhere.exe was not found.
  echo Install Visual Studio 2022 with "Desktop development with C++".
  pause
  exit /b 3
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
  set "MSBUILD=%%I"
  goto :found_msbuild
)

:found_msbuild
if not defined MSBUILD (
  echo [ERROR] MSBuild was not found.
  echo In Visual Studio Installer, add:
  echo   - Desktop development with C++
  echo   - MSVC v143 C++ x64/x86 build tools
  echo   - Windows 10 or Windows 11 SDK
  pause
  exit /b 4
)

echo [INFO] MSBuild: "%MSBUILD%"
echo [INFO] Building the .vcxproj directly; the .sln is not used.
echo.

"%MSBUILD%" "%PROJECT%" /m /v:minimal /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
  echo.
  echo [FAILED] Build failed. See the first MSBuild error above.
  pause
  exit /b %RESULT%
)

echo.
echo [SUCCESS] Build completed.
echo Output: "%ROOT%Build\x64\Release\rF2LuaFramework.dll"
pause
exit /b 0

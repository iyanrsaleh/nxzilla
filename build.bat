@echo off
setlocal enabledelayedexpansion

rem Locate the Visual Studio C++ toolchain, then build nxzilla.exe (x64).

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="

if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
  if defined VSPATH set "VCVARS=!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
)

if not defined VCVARS (
  echo [build] Could not locate Visual Studio C++ build tools ^(vcvars64.bat^).
  echo [build] Install the "Desktop development with C++" workload.
  exit /b 1
)

call "!VCVARS!" >nul
if errorlevel 1 (
  echo [build] Failed to initialize the MSVC environment.
  exit /b 1
)

pushd "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /utf-8 nxzilla.cpp /Fe:nxzilla.exe /link ws2_32.lib shell32.lib advapi32.lib user32.lib
set "RC=%errorlevel%"
del /q nxzilla.obj 2>nul
popd

if "%RC%"=="0" (
  echo [build] Built %~dp0nxzilla.exe
) else (
  echo [build] Build failed with errorlevel %RC%
)
exit /b %RC%

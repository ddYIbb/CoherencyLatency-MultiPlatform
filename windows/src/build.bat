@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if "%~1"=="" goto usage
set "ARCH=%~1"
set "TARGET="
set "OUTDIR="

if /I "%ARCH%"=="x86" (
  set "TARGET=x86"
  set "OUTDIR=x86"
)
if /I "%ARCH%"=="x64" (
  set "TARGET=x64"
  set "OUTDIR=x64"
)
if /I "%ARCH%"=="arm64" (
  set "TARGET=arm64"
  set "OUTDIR=ARM64"
)
if not defined TARGET goto usage

set "SUBSYSTEM=6.00"
if /I "%ARCH%"=="arm64" set "SUBSYSTEM=6.02"

if not defined VSINSTALL set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" (
  echo Visual Studio C++ tools not found.
  echo Set VSINSTALL to the Visual Studio installation directory.
  exit /b 1
)

call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=%TARGET% -host_arch=x64 >nul
if errorlevel 1 exit /b 1

if not exist "..\%OUTDIR%" mkdir "..\%OUTDIR%"

cl /nologo /W4 /permissive- /O2 /std:c++17 /EHsc /MT /utf-8 /D_WIN32_WINNT=0x0601 "CoherencyLatency.cpp" /Fe:"..\%OUTDIR%\CoherencyLatency.exe" /link /SUBSYSTEM:CONSOLE,%SUBSYSTEM%
if errorlevel 1 exit /b 1
echo Built ..\%OUTDIR%\CoherencyLatency.exe
exit /b 0

:usage
echo Usage: build.bat x86^|x64^|arm64
exit /b 1

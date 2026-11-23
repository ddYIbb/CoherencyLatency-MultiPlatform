@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if not defined VSINSTALL set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist "..\build-gui" mkdir "..\build-gui"
cl /nologo /W4 /permissive- /O2 /std:c++17 /EHsc /MT /utf-8 /D_WIN32_WINNT=0x0601 /DCL_NO_MAIN selftest.cpp engine_api.cpp /Fe:"..\build-gui\selftest.exe" /link /SUBSYSTEM:CONSOLE,6.01
if errorlevel 1 exit /b 1
echo OK

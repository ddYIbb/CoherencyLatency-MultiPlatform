@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
rc /nologo /fo payloads.res launcher.rc
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /std:c++17 /EHsc /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0600 launcher.cpp payloads.res /Fe:launcher.exe /link /SUBSYSTEM:CONSOLE,6.00
if errorlevel 1 exit /b 1
echo Built launcher.exe

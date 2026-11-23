@echo off
setlocal
set SRC_DIR=%~dp0
set BUILD_DIR=%SRC_DIR%..\..\build-gui\windows\selftest
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo >nul
cd /d "%SRC_DIR%"
if errorlevel 1 (echo CD FAILED & exit /b 1)
cl /nologo /W4 /permissive- /O2 /std:c++17 /EHsc /MT /utf-8 /D_WIN32_WINNT=0x0601 /DUNICODE /D_UNICODE /I. layout_selftest.cpp /link /SUBSYSTEM:CONSOLE /OUT:..\..\build-gui\windows\selftest\layout_selftest.exe
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK

@echo off
rem ---------------------------------------------------------------------------
rem CoherencyLatency Windows GUI build script.
rem   build.bat [x86|x64|arm64] [clean]
rem Produces build-gui\windows\<arch>\CoherencyLatency.exe (single file, /MT).
rem ---------------------------------------------------------------------------
setlocal enableextensions

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if /I "%ARCH%"=="amd64" set "ARCH=x64"
if /I "%ARCH%"=="win32" set "ARCH=x86"

if /I not "%ARCH%"=="x86" if /I not "%ARCH%"=="x64" if /I not "%ARCH%"=="arm64" (
    echo [build] unknown architecture "%ARCH%" ^(use x86, x64 or arm64^)
    exit /b 2
)

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%" >nul
set "SRC_DIR=%CD%"
popd >nul

set "BUILD_ROOT=%SRC_DIR%\..\..\build-gui\windows\%ARCH%"
set "OBJ_DIR=%BUILD_ROOT%\obj"
if /I "%~2"=="clean" (
    if exist "%BUILD_ROOT%" rmdir /s /q "%BUILD_ROOT%"
)
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"
if errorlevel 1 (
    echo [build] cannot create "%OBJ_DIR%"
    exit /b 3
)

set "VSDEV=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEV%" (
    echo [build] VsDevCmd.bat not found at "%VSDEV%"
    exit /b 4
)

set "SUBSYSTEM=6.00"
if /I "%ARCH%"=="arm64" set "SUBSYSTEM=6.02"

call "%VSDEV%" -arch=%ARCH% -host_arch=x64 -no_logo
if errorlevel 1 (
    echo [build] VsDevCmd failed
    exit /b 5
)

set "CFLAGS=/nologo /W4 /permissive- /O2 /std:c++17 /EHsc /MT /utf-8 /D_WIN32_WINNT=0x0601 /DUNICODE /D_UNICODE /I"%SRC_DIR%" /I"%SRC_DIR%\..""
set "LDFLAGS=/nologo /SUBSYSTEM:WINDOWS,%SUBSYSTEM% /DYNAMICBASE /NXCOMPAT /OPT:REF /OPT:ICF"

echo [build] arch=%ARCH% target=%BUILD_ROOT%\CoherencyLatency.exe

pushd "%OBJ_DIR%"
rc.exe /nologo /fo app.res "%SRC_DIR%\app.rc"
if errorlevel 1 (
    popd
    echo [build] rc failed
    exit /b 6
)

cl.exe %CFLAGS% /c "%SRC_DIR%\main.cpp" "%SRC_DIR%\..\engine_api.cpp"
if errorlevel 1 (
    popd
    echo [build] compile failed
    exit /b 7
)

link.exe %LDFLAGS% /OUT:"%BUILD_ROOT%\CoherencyLatency.exe" /PDB:CoherencyLatency.pdb /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" main.obj engine_api.obj app.res
if errorlevel 1 (
    popd
    echo [build] link failed
    exit /b 8
)
popd

echo [build] ok: %BUILD_ROOT%\CoherencyLatency.exe
endlocal
exit /b 0

@echo off
setlocal
rem Build the Android native port for all four ABIs with NDK r21e.
rem 32-bit ABIs target API 16 (Android 4.1), 64-bit ABIs target API 21.
rem Usage: build-android.bat [abi]   (abi = arm64-v8a | armeabi-v7a | x86 | x86_64 | all)
set NDK=D:\ndk21\android-ndk-r21e
set BIN=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin
set STRIP=%BIN%\llvm-strip.exe
cd /d "%~dp0"
if not exist build mkdir build
set TARGET=%1
if "%TARGET%"=="" set TARGET=all
set FLAGS=-std=c++17 -O3 -fPIE -pie -pthread -static-libstdc++ -Wall -Wextra -s
set FAILED=0
if "%TARGET%"=="all" goto do_arm64
if "%TARGET%"=="arm64-v8a" goto do_arm64
if "%TARGET%"=="armeabi-v7a" goto do_armv7
if "%TARGET%"=="x86" goto do_x86
if "%TARGET%"=="x86_64" goto do_x86_64
echo Unknown ABI: %TARGET%
exit /b 2

:do_arm64
echo === arm64-v8a (API 21) ===
call "%BIN%\aarch64-linux-android21-clang++.cmd" %FLAGS% src\main.cpp -o build\CoherencyLatency-arm64-v8a
if errorlevel 1 set FAILED=1
if "%TARGET%"=="arm64-v8a" goto done

:do_armv7
echo === armeabi-v7a (API 16) ===
call "%BIN%\armv7a-linux-androideabi16-clang++.cmd" %FLAGS% src\main.cpp -o build\CoherencyLatency-armeabi-v7a -latomic
if errorlevel 1 set FAILED=1
if "%TARGET%"=="armeabi-v7a" goto done

:do_x86
echo === x86 (API 16) ===
call "%BIN%\i686-linux-android16-clang++.cmd" %FLAGS% src\main.cpp -o build\CoherencyLatency-x86 -latomic
if errorlevel 1 set FAILED=1
if "%TARGET%"=="x86" goto done

:do_x86_64
echo === x86_64 (API 21) ===
call "%BIN%\x86_64-linux-android21-clang++.cmd" %FLAGS% src\main.cpp -o build\CoherencyLatency-x86_64
if errorlevel 1 set FAILED=1

:done
if "%FAILED%"=="1" (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK
dir /b build\CoherencyLatency-*
exit /b 0

#!/usr/bin/env python3
"""Builds the CoherencyLatency Android APK without Gradle.

Pipeline: NDK clang++ native libraries -> launcher icons -> aapt2 compile/link
-> javac -> d8 -> zip -> zipalign -> apksigner (debug keystore).

Builds one universal APK: minSdk 16, 32-bit ARM/x86 for Android 4.1+
and 64-bit ARM/x86_64 for Android 5.0+ (the 64-bit slices simply do not load
on older devices).  Native code is built with NDK r21e, which still ships the
API 16 sysroots; build-tools 31.0.0, platforms/android-31 and JDK 17.
"""

import os
import shutil
import subprocess
import sys
import zipfile

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
JAVA_HOME = os.environ.get("JAVA_HOME", "D:/AndroidTools/jdk17/jdk-17.0.2")
SDK = os.environ.get("ANDROID_SDK_ROOT", "D:/AndroidTools/sdk")
NDK = os.environ.get("ANDROID_NDK_ROOT", "D:/ndk21/android-ndk-r21e")
BUILD_TOOLS = os.path.join(SDK, "build-tools", "31.0.0")
ANDROID_JAR = os.path.join(SDK, "platforms", "android-31", "android.jar")

PACKAGE = "com.ddyibb.coherencylatency"
APK_NAME = "CoherencyLatency-Android-UI-v1.3.0.apk"

BUILD = os.path.join(HERE, "build")
RES_ZIP = os.path.join(BUILD, "res.zip")
BASE_APK = os.path.join(BUILD, "base.apk")
GEN_DIR = os.path.join(BUILD, "gen")
CLASSES_DIR = os.path.join(BUILD, "classes")
CLASSES_JAR = os.path.join(BUILD, "classes.jar")
DEX_DIR = os.path.join(BUILD, "dex")
UNSIGNED_APK = os.path.join(BUILD, "unsigned.apk")
ALIGNED_APK = os.path.join(BUILD, "aligned.apk")
SIGNED_APK = os.path.join(BUILD, "android", APK_NAME)
KEYSTORE = os.path.join(BUILD, "debug.keystore")

TOOLCHAIN = os.path.join(NDK, "toolchains", "llvm", "prebuilt", "windows-x86_64")
SYSROOT = os.path.join(TOOLCHAIN, "sysroot")
CLANG = os.path.join(TOOLCHAIN, "bin", "clang++.exe")
STRIP = os.path.join(TOOLCHAIN, "bin", "llvm-strip.exe")

# 64-bit ABIs only exist from API 21 on, so their slices are built for 21;
# the 32-bit slices go all the way down to API 16 (Android 4.1).
# Android 15+ devices can run with 16 KB memory pages; the loader then
# requires every PT_LOAD segment to be 16 KB aligned.  NDK r21e's clang 9
# defaults to 4 KB, so the 64-bit slices ask the linker for 16 KB explicitly.
PAGE_ALIGN_16K = ["-Wl,-z,max-page-size=16384"]

ABIS = {
    "arm64-v8a": ("aarch64-linux-android21", PAGE_ALIGN_16K),
    "armeabi-v7a": ("armv7a-linux-androideabi16", ["-latomic"]),
    "x86": ("i686-linux-android16", ["-latomic"]),
    "x86_64": ("x86_64-linux-android21", PAGE_ALIGN_16K),
}

MIN_SDK = 16

NATIVE_FLAGS = ["-shared", "-fPIC", "-O3", "-std=c++17", "-Wall", "-Wextra",
                "-static-libstdc++"]


def log(message):
    print("==> " + message, flush=True)


def run(command, cwd=HERE, env=None):
    log(" ".join(command))
    merged = os.environ.copy()
    if env:
        merged.update(env)
    result = subprocess.run(command, cwd=cwd, env=merged,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    if result.stdout.strip():
        print(result.stdout.rstrip())
    if result.returncode != 0:
        raise SystemExit("FAILED (exit %d): %s" % (result.returncode,
                                                   " ".join(command)))


def java_env():
    return {"JAVA_HOME": JAVA_HOME,
            "PATH": os.path.join(JAVA_HOME, "bin") + os.pathsep + os.environ["PATH"]}


def build_native():
    log("building native libraries")
    source = os.path.join(HERE, "jni", "clbench_jni.cpp")
    for abi, (target, extra) in ABIS.items():
        output_dir = os.path.join(BUILD, abi)
        os.makedirs(output_dir, exist_ok=True)
        output = os.path.join(output_dir, "libclbench.so")
        run([CLANG, "--target=" + target, "--sysroot=" + SYSROOT] + NATIVE_FLAGS +
            extra + ["-o", output, source])
        run([STRIP, "--strip-unneeded", output])


def build_icons():
    log("drawing launcher icons")
    run([sys.executable, os.path.join(HERE, "tools", "make_icons.py")])


def build_resources():
    # aapt2 on Windows cannot open absolute paths that contain non-ASCII
    # characters, so every argument here is relative to the app directory.
    log("compiling resources")
    os.makedirs(BUILD, exist_ok=True)
    run([os.path.join(BUILD_TOOLS, "aapt2.exe"), "compile", "--dir", "res",
         "-o", "build/res.zip"],
        cwd=HERE)

    log("linking resources and manifest")
    if os.path.isdir(GEN_DIR):
        shutil.rmtree(GEN_DIR)
    os.makedirs(GEN_DIR)
    run([os.path.join(BUILD_TOOLS, "aapt2.exe"), "link", "-o", "build/base.apk",
         "-I", ANDROID_JAR, "--manifest", "AndroidManifest.xml",
         "--java", "build/gen", "--min-sdk-version", str(MIN_SDK),
         "--target-sdk-version", "31", "--version-code", "6",
         "--version-name", "1.3.0", "build/res.zip"],
        cwd=HERE)


def java_sources():
    sources = []
    for root, _dirs, files in os.walk(os.path.join(HERE, "java")):
        for name in files:
            if name.endswith(".java"):
                sources.append(os.path.join(root, name))
    for root, _dirs, files in os.walk(GEN_DIR):
        for name in files:
            if name.endswith(".java"):
                sources.append(os.path.join(root, name))
    return sources


def build_dex():
    log("compiling Java sources")
    if os.path.isdir(CLASSES_DIR):
        shutil.rmtree(CLASSES_DIR)
    os.makedirs(CLASSES_DIR)
    sources = java_sources()
    run([os.path.join(JAVA_HOME, "bin", "javac.exe"), "-encoding", "UTF-8",
         "-source", "8", "-target", "8", "-Xlint:-options",
         "-bootclasspath", ANDROID_JAR, "-d", CLASSES_DIR] + sources)

    log("packing classes.jar")
    if os.path.exists(CLASSES_JAR):
        os.remove(CLASSES_JAR)
    run([os.path.join(JAVA_HOME, "bin", "jar.exe"), "cf", CLASSES_JAR, "-C",
         CLASSES_DIR, "."])

    log("dexing")
    if os.path.isdir(DEX_DIR):
        shutil.rmtree(DEX_DIR)
    os.makedirs(DEX_DIR)
    run([os.path.join(JAVA_HOME, "bin", "java.exe"), "-cp",
         os.path.join(BUILD_TOOLS, "lib", "d8.jar"), "com.android.tools.r8.D8",
         "--lib", ANDROID_JAR, "--min-api", str(MIN_SDK), "--output", DEX_DIR,
         CLASSES_JAR], env=java_env())


def package_apk():
    log("assembling APK")
    if os.path.exists(UNSIGNED_APK):
        os.remove(UNSIGNED_APK)
    with zipfile.ZipFile(UNSIGNED_APK, "w", zipfile.ZIP_DEFLATED) as output:
        with zipfile.ZipFile(BASE_APK) as base:
            for info in base.infolist():
                output.writestr(info, base.read(info.filename),
                                compress_type=info.compress_type)
        output.write(os.path.join(DEX_DIR, "classes.dex"), "classes.dex")
        for abi in ABIS:
            output.write(os.path.join(BUILD, abi, "libclbench.so"),
                         "lib/%s/libclbench.so" % abi)

    log("aligning")
    if os.path.exists(ALIGNED_APK):
        os.remove(ALIGNED_APK)
    # The libraries stay deflated and the manifest keeps extractNativeLibs, so
    # the loader uses the extracted copies and only their ELF p_align (16 KB,
    # see PAGE_ALIGN_16K) has to satisfy a 16 KB-page device.  Keeping them
    # compressed also keeps the download at roughly half the uncompressed size.
    run([os.path.join(BUILD_TOOLS, "zipalign.exe"), "-f", "-p", "4",
         UNSIGNED_APK, ALIGNED_APK])


def ensure_keystore():
    if os.path.exists(KEYSTORE):
        return
    log("creating debug keystore")
    os.makedirs(os.path.dirname(KEYSTORE), exist_ok=True)
    run([os.path.join(JAVA_HOME, "bin", "keytool.exe"), "-genkeypair",
         "-keystore", KEYSTORE, "-storepass", "android", "-keypass", "android",
         "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize", "2048",
         "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"])


def sign_apk():
    ensure_keystore()
    log("signing")
    os.makedirs(os.path.dirname(SIGNED_APK), exist_ok=True)
    if os.path.exists(SIGNED_APK):
        os.remove(SIGNED_APK)
    run([os.path.join(BUILD_TOOLS, "apksigner.bat"), "sign",
         "--ks", KEYSTORE, "--ks-pass", "pass:android", "--key-pass", "pass:android",
         "--ks-key-alias", "androiddebugkey", "--min-sdk-version", str(MIN_SDK),
         "--out", SIGNED_APK, ALIGNED_APK], env=java_env())

    log("verifying signature")
    run([os.path.join(BUILD_TOOLS, "apksigner.bat"), "verify", "--print-certs",
         SIGNED_APK], env=java_env())

    final = os.path.join(os.path.dirname(HERE), APK_NAME)
    shutil.copyfile(SIGNED_APK, final)
    size = os.path.getsize(final)
    print("\nAPK: %s (%.1f KiB)" % (final, size / 1024.0))


def main():
    build_native()
    build_icons()
    build_resources()
    build_dex()
    package_apk()
    sign_apk()


if __name__ == "__main__":
    main()

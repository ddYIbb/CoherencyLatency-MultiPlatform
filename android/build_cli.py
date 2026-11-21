import os, subprocess, sys
ROOT = os.path.dirname(os.path.abspath(__file__))
BIN = r"D:\ndk21\android-ndk-r21e\toolchains\llvm\prebuilt\windows-x86_64\bin"
CLANG = os.path.join(BIN, "clang++.exe")
SYSROOT = r"D:\ndk21\android-ndk-r21e\toolchains\llvm\prebuilt\windows-x86_64\sysroot"
FLAGS = ["-std=c++17", "-O3", "-fPIE", "-pie", "-pthread", "-static-libstdc++",
         "-Wall", "-Wextra", "-s"]
TARGETS = [
    ("arm64-v8a", "aarch64-linux-android21", []),
    ("armeabi-v7a", "armv7a-linux-androideabi16", ["-latomic"]),
    ("x86", "i686-linux-android16", ["-latomic"]),
    ("x86_64", "x86_64-linux-android21", []),
]
os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
src = os.path.join(ROOT, "src", "main.cpp")
failed = False
for abi, target, extra in TARGETS:
    out = os.path.join(ROOT, "build", "CoherencyLatency-" + abi)
    cmd = [CLANG, "--target=" + target, "--sysroot=" + SYSROOT] + FLAGS + extra + [src, "-o", out]
    print("==>", abi, target, flush=True)
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace")
    if r.stdout.strip():
        print(r.stdout.rstrip())
    if r.returncode != 0:
        failed = True
        print("FAILED", abi, r.returncode)
print("BUILD", "FAILED" if failed else "OK")
sys.exit(1 if failed else 0)

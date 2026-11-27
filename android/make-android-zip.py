import os
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parent / "CoherencyLatency-Android-v1.3.0.zip"
FOLDER = "CoherencyLatency-Android-v1.3.0"

FILES = [
    ("build/CoherencyLatency-arm64-v8a", 0o755, "CoherencyLatency-arm64-v8a"),
    ("build/CoherencyLatency-armeabi-v7a", 0o755, "CoherencyLatency-armeabi-v7a"),
    ("build/CoherencyLatency-x86", 0o755, "CoherencyLatency-x86"),
    ("build/CoherencyLatency-x86_64", 0o755, "CoherencyLatency-x86_64"),
    ("docs/使用手册.md", 0o644, "使用手册.md"),
    ("docs/使用手册.html", 0o644, "使用手册.html"),
    ("docs/使用手册.pdf", 0o644, "使用手册.pdf"),
    ("build/SHA256SUMS", 0o644, "SHA256SUMS"),
    ("README.md", 0o644, "README.md"),
    ("LICENSE", 0o644, "LICENSE"),
]

def main() -> int:
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rel, mode, arc in FILES:
            src = ROOT / rel
            if not src.is_file():
                print("missing:", src)
                return 1
            info = zipfile.ZipInfo(f"{FOLDER}/{arc}", date_time=(2026, 11, 17, 12, 0, 0))
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, src.read_bytes())
    print("wrote", OUT, OUT.stat().st_size, "bytes")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

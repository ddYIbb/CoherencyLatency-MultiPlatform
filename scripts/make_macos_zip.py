#!/usr/bin/env python3
"""Build the CoherencyLatency macOS release zip (binary + manual + license)."""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import zipfile

REPO = pathlib.Path(r"C:/Users/Administrator/AppData/Local/Temp/cl_repo")
OUT_DIR = REPO / "dist"
STAGE = OUT_DIR / "CoherencyLatency-macOS-Release"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def add(zf, path, arcname, mode=0o644):
    info = zipfile.ZipInfo(arcname, date_time=(2026, 11, 23, 12, 0, 0))
    info.external_attr = (0o100000 | mode) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    with open(path, "rb") as fh:
        zf.writestr(info, fh.read())


def main():
    binary = REPO / "macos/build/CoherencyLatency"
    manual_md = REPO / "docs/使用手册.md"
    manual_html = REPO / "docs/使用手册.html"
    manual_pdf = REPO / "docs/使用手册.pdf"
    license_file = REPO / "LICENSE"
    for path in (binary, manual_md, manual_html, manual_pdf, license_file):
        if not path.exists():
            sys.exit("missing " + str(path))

    OUT_DIR.mkdir(exist_ok=True)
    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir()

    shutil.copy2(binary, STAGE / "CoherencyLatency")
    os.chmod(STAGE / "CoherencyLatency", 0o755)
    shutil.copy2(manual_md, STAGE / "使用手册.md")
    shutil.copy2(manual_html, STAGE / "使用手册.html")
    shutil.copy2(manual_pdf, STAGE / "使用手册.pdf")
    shutil.copy2(license_file, STAGE / "LICENSE")

    names = ["CoherencyLatency", "使用手册.md", "使用手册.html", "使用手册.pdf", "LICENSE"]
    lines = []
    for name in names:
        lines.append("%s  %s" % (sha256(STAGE / name), name))
    (STAGE / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")

    zip_path = OUT_DIR / "CoherencyLatency-macOS-v1.2.0.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for name in names + ["SHA256SUMS"]:
            mode = 0o755 if name == "CoherencyLatency" else 0o644
            add(zf, STAGE / name, "CoherencyLatency-macOS-Release/" + name, mode)

    print("zip:", zip_path, zip_path.stat().st_size)
    print("sha256:", sha256(zip_path))
    with zipfile.ZipFile(zip_path) as zf:
        bad = zf.testzip()
        print("testzip:", "OK" if bad is None else bad)
        for info in zf.infolist():
            print("  %-52s %8d %s" % (info.filename, info.file_size, oct(info.external_attr >> 16)))


if __name__ == "__main__":
    main()

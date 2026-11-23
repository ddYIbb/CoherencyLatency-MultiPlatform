#!/usr/bin/env python3
"""Build the CoherencyLatency macOS GUI release zip (.app bundle + standalone binary)."""
import hashlib, os, pathlib, shutil, sys, zipfile

REPO = pathlib.Path(r"C:/Users/Administrator/AppData/Local/Temp/cl_repo")
OUT = REPO / "dist"
STAGE = OUT / "CoherencyLatency-macOS-GUI-Release"
APP_SRC = REPO / "gui/macos/build/CoherencyLatency.app"
BIN = REPO / "gui/macos/build/CoherencyLatency"
MANUAL_MD = REPO / "docs/GUI-使用说明.md"
MANUAL_HTML = REPO / "docs/GUI-使用说明.html"
MANUAL_PDF = REPO / "docs/GUI-使用说明.pdf"
LICENSE = REPO / "LICENSE"

def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()

def add_tree(zf, root, rel_prefix):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            full = pathlib.Path(dirpath) / name
            rel = full.relative_to(root)
            arc = rel_prefix + "/" + str(rel).replace(os.sep, "/")
            # Windows reports every file as executable, so decide by name.
            executable = name == "CoherencyLatency"
            mode = 0o755 if executable else 0o644
            info = zipfile.ZipInfo(arc, date_time=(2026, 11, 23, 12, 0, 0))
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            zf.writestr(info, full.read_bytes())

def main():
    for p in (APP_SRC, BIN, MANUAL_MD, MANUAL_HTML, MANUAL_PDF, LICENSE):
        if not p.exists():
            sys.exit("missing " + str(p))
    OUT.mkdir(exist_ok=True)
    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir()

    shutil.copytree(APP_SRC, STAGE / "CoherencyLatency.app")
    shutil.copy2(BIN, STAGE / "CoherencyLatency")
    os.chmod(STAGE / "CoherencyLatency", 0o755)
    shutil.copy2(MANUAL_MD, STAGE / "使用手册.md")
    shutil.copy2(MANUAL_HTML, STAGE / "使用手册.html")
    shutil.copy2(MANUAL_PDF, STAGE / "使用手册.pdf")
    shutil.copy2(LICENSE, STAGE / "LICENSE")

    sums = []
    for name in ["CoherencyLatency", "使用手册.md", "使用手册.html", "使用手册.pdf", "LICENSE"]:
        sums.append("%s  %s" % (sha256(STAGE / name), name))
    app_bin = STAGE / "CoherencyLatency.app/Contents/MacOS/CoherencyLatency"
    sums.append("%s  %s" % (sha256(app_bin), "CoherencyLatency.app/Contents/MacOS/CoherencyLatency"))
    (STAGE / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")

    zp = OUT / "CoherencyLatency-macOS-GUI-v1.2.0.zip"
    if zp.exists():
        zp.unlink()
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        add_tree(zf, STAGE, "CoherencyLatency-macOS-GUI-Release")

    print("zip:", zp.name, zp.stat().st_size)
    print("sha256:", sha256(zp))
    with zipfile.ZipFile(zp) as zf:
        print("testzip:", "OK" if zf.testzip() is None else "BAD")
        for i in zf.infolist():
            print("  %-70s %8d %s" % (i.filename, i.file_size, oct(i.external_attr >> 16)))

if __name__ == "__main__":
    main()

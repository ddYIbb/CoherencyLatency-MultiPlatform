#!/usr/bin/env python3
"""Build the CoherencyLatency Windows GUI release zip."""
import hashlib, pathlib, shutil, sys, zipfile

REPO = pathlib.Path(r"C:/Users/Administrator/AppData/Local/Temp/cl_repo")
OUT = REPO / "dist"
STAGE = OUT / "CoherencyLatency-Windows-GUI-Release"
EXE = REPO / "tools/launcher-gui/build/CoherencyLatency.exe"
MD = REPO / "docs/GUI-使用说明.md"
HTML = REPO / "docs/GUI-使用说明.html"
PDF = REPO / "docs/GUI-使用说明.pdf"
LICENSE = REPO / "LICENSE"

def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()

def main():
    for p in (EXE, MD, HTML, PDF, LICENSE):
        if not p.exists():
            sys.exit("missing " + str(p))
    OUT.mkdir(exist_ok=True)
    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir()
    shutil.copy2(EXE, STAGE / "CoherencyLatency.exe")
    shutil.copy2(MD, STAGE / "使用手册.md")
    shutil.copy2(HTML, STAGE / "使用手册.html")
    shutil.copy2(PDF, STAGE / "使用手册.pdf")
    shutil.copy2(LICENSE, STAGE / "LICENSE")
    names = ["CoherencyLatency.exe", "使用手册.md", "使用手册.html", "使用手册.pdf", "LICENSE"]
    (STAGE / "SHA256SUMS").write_text(
        "".join("%s  %s\n" % (sha256(STAGE / n), n) for n in names), encoding="utf-8")
    zp = OUT / "CoherencyLatency-Windows-GUI-v1.2.0.zip"
    if zp.exists():
        zp.unlink()
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for name in names + ["SHA256SUMS"]:
            info = zipfile.ZipInfo("CoherencyLatency-Windows-GUI-Release/" + name,
                                   date_time=(2026, 11, 24, 1, 0, 0))
            info.external_attr = (0o100000 | (0o755 if name.endswith(".exe") else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            zf.writestr(info, (STAGE / name).read_bytes())
    print("zip:", zp.name, zp.stat().st_size)
    print("sha256:", sha256(zp))
    with zipfile.ZipFile(zp) as zf:
        print("testzip:", "OK" if zf.testzip() is None else "BAD")
        for i in zf.infolist():
            print("  %-60s %9d" % (i.filename, i.file_size))

if __name__ == "__main__":
    main()

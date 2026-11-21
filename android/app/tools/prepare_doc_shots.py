from PIL import Image
from pathlib import Path
art = Path("art")
out = Path("../docs/screenshots")
out.mkdir(parents=True, exist_ok=True)

def save(src, dst, crop=None):
    im = Image.open(art / src).convert("RGB")
    if crop:
        im = im.crop(crop)
    im.save(out / dst)
    print(dst, im.size)

save("p_idle.png", "apk-01-home.png")
save("p_run.png", "apk-02-running.png")
save("p_done.png", "apk-03-finished.png")
save("t1.png", "apk-07-tablet.png", (0, 0, 1600, 853))
save("w4_round.png", "apk-08-watch-round.png")
save("w_run5_round.png", "apk-09-watch-running.png")
save("w_done5_round.png", "apk-10-watch-done.png")
save("w5_round.png", "apk-11-watch-export.png")
save("tv_idle.png", "apk-12-tv-home.png")
save("tv_run.png", "apk-13-tv-running.png")
save("tv_done.png", "apk-14-tv-done.png")

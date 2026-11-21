from PIL import Image
import sys
for src, dst in [("art/tv_idle_raw.png", "art/tv_idle.png"), ("art/tv_run_raw.png", "art/tv_run.png"), ("art/tv_done_raw.png", "art/tv_done.png")]:
    im = Image.open(src).convert("RGB").crop((0, 0, 1920, 933))
    im.save(dst)
    print(dst, im.size)

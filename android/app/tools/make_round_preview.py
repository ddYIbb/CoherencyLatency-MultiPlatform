import sys
from PIL import Image, ImageDraw
src, dst = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGB")
im = im.crop((0, 0, 454, 454))
mask = Image.new("L", (454, 454), 0)
d = ImageDraw.Draw(mask)
d.ellipse((0, 0, 453, 453), fill=255)
out = Image.new("RGB", (454, 454), (24, 24, 24))
out.paste(im, (0, 0), mask)
out.save(dst)
print("saved", dst, im.size)

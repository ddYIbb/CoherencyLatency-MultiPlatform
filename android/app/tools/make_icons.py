#!/usr/bin/env python3
"""Draws the CoherencyLatency launcher icon (a small latency matrix heatmap)."""

import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "res")
ART = os.path.join(HERE, "..", "art")

# One 4x4 latency matrix (nanoseconds) drives both the geometry and the colours
# so the icon looks like the result the app produces.
MATRIX = [
    [None, 23.4, 30.8, 18.2],
    [23.1, None, 15.3, 27.4],
    [30.6, 15.1, None, 21.0],
    [18.4, 27.2, 20.8, None],
]

LOW_COLOR = (56, 189, 248)     # sky
HIGH_COLOR = (37, 99, 235)     # blue
DIAGONAL_COLOR = (30, 41, 59)  # slate
BACKGROUND = (8, 13, 28)       # near-black navy


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def draw_icon(size):
    scale = size / 512.0
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # Rounded background.
    radius = int(112 * scale)
    draw.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius,
                           fill=BACKGROUND + (255,))

    cells = 4
    gap = int(13 * scale)
    cell = int((size - int(2 * 84 * scale) - gap * (cells - 1)) / cells)
    margin = (size - (cell * cells + gap * (cells - 1))) // 2
    corner = max(2, int(16 * scale))

    values = [v for row in MATRIX for v in row if v is not None]
    low, high = min(values), max(values)

    for row in range(cells):
        for column in range(cells):
            left = margin + column * (cell + gap)
            top = margin + row * (cell + gap)
            box = [left, top, left + cell, top + cell]
            value = MATRIX[row][column]
            if value is None:
                colour = DIAGONAL_COLOR
            else:
                t = (value - low) / (high - low) if high > low else 0.0
                colour = lerp(LOW_COLOR, HIGH_COLOR, t)
            draw.rounded_rectangle(box, radius=corner, fill=colour + (255,))

            if value is None:
                # Small "x" marks the diagonal, matching the app's matrix view.
                inset = int(cell * 0.34)
                width = max(2, int(4 * scale))
                draw.line([box[0] + inset, box[1] + inset,
                           box[2] - inset, box[3] - inset],
                          fill=(100, 116, 139, 255), width=width)
                draw.line([box[0] + inset, box[3] - inset,
                           box[2] - inset, box[1] + inset],
                          fill=(100, 116, 139, 255), width=width)

    return image


def load_font(bold, size):
    candidates = [
        "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                pass
    return ImageFont.load_default()


def draw_banner():
    """320x180 Android TV launcher banner (xhdpi)."""
    width, height = 320, 180
    image = Image.new("RGBA", (width, height), BACKGROUND + (255,))
    draw = ImageDraw.Draw(image)

    # Opaque vertical gradient; alpha here would look washed out when the
    # launcher composites the banner over a light background.
    for y in range(height):
        t = y / float(height - 1)
        colour = lerp(BACKGROUND, (10, 20, 42), t)
        draw.line([(0, y), (width, y)], fill=colour + (255,))

    title_font = load_font(True, 24)
    small_font = load_font(False, 10)
    body_font = load_font(False, 12)
    tiny_font = load_font(False, 10)

    draw.text((18, 13), "CoherencyLatency", font=title_font,
              fill=(255, 255, 255, 255))
    draw.text((20, 45), "CACHE COHERENCE LATENCY", font=small_font,
              fill=(148, 197, 255, 255))

    # 4x4 heatmap on the left, echoing the app's result matrix.
    cell, gap, left, top = 18, 4, 18, 74
    values = [v for row in MATRIX for v in row if v is not None]
    low, high = min(values), max(values)
    for row in range(4):
        for column in range(4):
            box_left = left + column * (cell + gap)
            box_top = top + row * (cell + gap)
            box = [box_left, box_top, box_left + cell, box_top + cell]
            value = MATRIX[row][column]
            if value is None:
                colour = DIAGONAL_COLOR
            else:
                t = (value - low) / (high - low) if high > low else 0.0
                colour = lerp(LOW_COLOR, HIGH_COLOR, t)
            draw.rounded_rectangle(box, radius=4, fill=colour + (255,))

    draw.text((118, 74), "Native Android benchmark", font=body_font,
              fill=(255, 255, 255, 255))
    draw.text((118, 94), "Bounce · ns/op", font=body_font,
              fill=(148, 197, 255, 255))
    draw.text((118, 118), "phone · tablet · watch · TV", font=tiny_font,
              fill=(203, 213, 225, 255))
    draw.text((118, 136), "one APK · all screens", font=tiny_font,
              fill=(148, 163, 184, 255))

    # Thin accent line at the bottom.
    draw.rectangle([0, height - 4, width, height - 1],
                   fill=(37, 99, 235, 255))
    return image


def main():
    os.makedirs(ART, exist_ok=True)
    master = draw_icon(512)
    master.save(os.path.join(ART, "icon-512.png"))

    densities = {
        "mipmap-mdpi": 48,
        "mipmap-hdpi": 72,
        "mipmap-xhdpi": 96,
        "mipmap-xxhdpi": 144,
        "mipmap-xxxhdpi": 192,
    }
    for folder, size in densities.items():
        directory = os.path.join(RES, folder)
        os.makedirs(directory, exist_ok=True)
        icon = master.resize((size, size), Image.LANCZOS)
        icon.save(os.path.join(directory, "ic_launcher.png"))
        print("wrote", os.path.join(folder, "ic_launcher.png"), size)

    banner_dir = os.path.join(RES, "drawable-xhdpi")
    os.makedirs(banner_dir, exist_ok=True)
    banner = draw_banner()
    banner.save(os.path.join(banner_dir, "tv_banner.png"))
    banner.save(os.path.join(ART, "tv_banner-320x180.png"))
    print("wrote", os.path.join("drawable-xhdpi", "tv_banner.png"), "320x180")


if __name__ == "__main__":
    main()

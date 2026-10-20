#!/usr/bin/env python3
"""Draws the Android launcher icon (an original design: a flashlight beam in a dark
hallway) into packaging/android/res/mipmap-*/ic_launcher.png. Needs Pillow."""
import os
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def draw(size: int) -> Image.Image:
    s = 4 * size  # supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    bg = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(bg)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=s // 5, fill=(24, 20, 48, 255))
    # floor glow
    d.ellipse([s * 0.15, s * 0.70, s * 0.95, s * 0.98], fill=(60, 50, 90, 255))
    img = Image.alpha_composite(img, bg)
    # beam: a soft yellow wedge from the lower left
    beam = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    b = ImageDraw.Draw(beam)
    b.polygon([(s * 0.22, s * 0.74), (s * 0.95, s * 0.28), (s * 0.95, s * 0.86)], fill=(255, 226, 120, 170))
    beam = beam.filter(ImageFilter.GaussianBlur(s * 0.02))
    img = Image.alpha_composite(img, beam)
    # flashlight body
    f = ImageDraw.Draw(img)
    f.rounded_rectangle([s * 0.08, s * 0.70, s * 0.26, s * 0.80], radius=s * 0.03, fill=(70, 160, 90, 255))
    f.polygon([(s * 0.24, s * 0.68), (s * 0.30, s * 0.66), (s * 0.30, s * 0.84), (s * 0.24, s * 0.82)], fill=(200, 200, 210, 255))
    # two little eyes in the dark
    for cx in (0.62, 0.72):
        f.ellipse([s * (cx - 0.03), s * 0.40, s * (cx + 0.03), s * 0.48], fill=(255, 255, 255, 235))
    # clip to the rounded square
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=s // 5, fill=255)
    img.putalpha(Image.composite(img.getchannel("A"), mask, mask))
    return img.resize((size, size), Image.LANCZOS)


def main():
    for name, px in SIZES.items():
        out = os.path.join(ROOT, "packaging", "android", "res", f"mipmap-{name}")
        os.makedirs(out, exist_ok=True)
        draw(px).save(os.path.join(out, "ic_launcher.png"))
    print("icons written")


if __name__ == "__main__":
    main()

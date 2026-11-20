"""Render the About dialog banner bitmaps (logo and wordmark only; the text is drawn by GDI) at 100-200% DPI."""
import sys
from PIL import Image, ImageFilter

NAVY = (3, 7, 62)
SCALES = {"reactos": 100, "reactos-125": 125, "reactos-150": 150, "reactos-175": 175, "reactos-200": 200}


def build(src, scale):
    w, h = 413 * scale // 100, 72 * scale // 100
    k = src.width / 2000.0
    art = src.crop((int(120 * k), int(60 * k), int(1950 * k), int(580 * k)))
    th = 38 * scale // 100
    art = art.resize((art.width * th // art.height, th), Image.LANCZOS)
    mask = Image.new("L", art.size, 0)
    m = max(2, th // 6)
    mask.paste(255, (m, m, art.width - m, art.height - m))
    mask = mask.filter(ImageFilter.GaussianBlur(m / 2.0))
    out = Image.new("RGB", (w, h), NAVY)
    out.paste(art, (6 * scale // 100, 3 * scale // 100), mask)
    return out


if __name__ == "__main__":
    src = Image.open(sys.argv[1]).convert("RGB")
    for name, scale in SCALES.items():
        build(src, scale).save("%s/%s.bmp" % (sys.argv[2], name), "BMP")

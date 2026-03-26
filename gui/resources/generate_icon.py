#!/usr/bin/env python3
"""
generate_icon.py — Generate platform-specific icon files for XOPTrader.

Usage:
    python generate_icon.py <output_dir> <format>

    <format> is one of: ico  icns  png

Requires: Pillow  (pip install pillow)
"""

import math
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Pillow is required:  pip install pillow", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# Icon design
# ---------------------------------------------------------------------------
# Dark navy background, cyan chart line, bold "XOP" label.
# The hexagonal outline nods to Chia's geometry; the upward-trending
# polyline represents market-making activity.
# ---------------------------------------------------------------------------

BG_COLOR    = (15,  25,  50, 255)   # dark navy
ACCENT      = (0,  212, 255, 255)   # Chia cyan
BORDER      = (0,  180, 220, 200)


def _hex_points(cx: float, cy: float, r: float) -> list[tuple[float, float]]:
    """Return the 6 vertices of a flat-top hexagon."""
    return [
        (cx + r * math.cos(math.radians(60 * i - 30)),
         cy + r * math.sin(math.radians(60 * i - 30)))
        for i in range(6)
    ]


def create_base_image(size: int = 256) -> Image.Image:
    img  = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    cx, cy = size / 2, size / 2
    r      = size * 0.46          # hexagon radius

    # ---- Hexagon background ------------------------------------------------
    hex_pts = _hex_points(cx, cy, r)
    draw.polygon(hex_pts, fill=BG_COLOR)
    # Glow border (two passes for soft effect)
    draw.line(hex_pts + [hex_pts[0]], fill=(*BORDER[:3], 120), width=max(2, size // 40))
    draw.line(hex_pts + [hex_pts[0]], fill=ACCENT,             width=max(1, size // 80))

    # ---- Candlestick-style chart line --------------------------------------
    # Five data points tracing an upward trend.
    s = size
    chart = [
        (s * 0.22, s * 0.68),
        (s * 0.35, s * 0.50),
        (s * 0.50, s * 0.60),
        (s * 0.65, s * 0.35),
        (s * 0.78, s * 0.42),
    ]
    lw = max(3, size // 32)
    draw.line(chart, fill=ACCENT, width=lw, joint="curve")

    # Dot at each vertex
    dot = max(2, size // 48)
    for px, py in chart:
        draw.ellipse([px - dot, py - dot, px + dot, py + dot], fill=ACCENT)

    # ---- "XOP" label -------------------------------------------------------
    label     = "XOP"
    font_size = max(16, size // 7)
    try:
        font = ImageFont.truetype("arialbd.ttf", font_size)
    except Exception:
        try:
            font = ImageFont.truetype("arial.ttf", font_size)
        except Exception:
            font = ImageFont.load_default()

    bbox = draw.textbbox((0, 0), label, font=font)
    tw   = bbox[2] - bbox[0]
    tx   = (size - tw) // 2
    ty   = int(size * 0.73)
    draw.text((tx, ty), label, fill=ACCENT, font=font)

    return img


# ---------------------------------------------------------------------------
# Format savers
# ---------------------------------------------------------------------------

ICO_SIZES  = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)]
ICNS_SIZES = [16, 32, 64, 128, 256, 512]


def save_ico(img: Image.Image, path: Path) -> None:
    imgs = [img.resize((s, s), Image.LANCZOS) for s in (256, 128, 64, 48, 32, 16)]
    imgs[0].save(path, format="ICO", append_images=imgs[1:],
                 sizes=ICO_SIZES)
    print(f"Saved {path}")


def save_icns(img: Image.Image, path: Path) -> None:
    # Pillow writes ICNS directly since 9.1
    imgs = [img.resize((s, s), Image.LANCZOS) for s in ICNS_SIZES]
    imgs[0].save(path, format="ICNS", append_images=imgs[1:])
    print(f"Saved {path}")


def save_png(img: Image.Image, path: Path, size: int = 256) -> None:
    img.resize((size, size), Image.LANCZOS).save(path, format="PNG")
    print(f"Saved {path}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    out_dir  = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    fmt      = sys.argv[2].lower() if len(sys.argv) > 2 else "png"

    out_dir.mkdir(parents=True, exist_ok=True)
    img = create_base_image(512)   # generate at 512 px; downscale as needed

    if fmt == "ico":
        save_ico(img, out_dir / "icon.ico")
    elif fmt == "icns":
        save_icns(img, out_dir / "icon.icns")
    else:
        save_png(img, out_dir / "icon.png")


if __name__ == "__main__":
    main()

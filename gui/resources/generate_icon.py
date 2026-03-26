#!/usr/bin/env python3
"""
generate_icon.py — Generate platform-specific icon files for XOPTrader.

Usage:
    python generate_icon.py <output_dir> <format>

    <format> is one of: ico  icns  png

Requires: Pillow  (pip install pillow)
"""

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
# Sky-blue background, green tree with layered foliage canopy, brown trunk,
# and a small white "XOP" label at the bottom.
# ---------------------------------------------------------------------------

BG_COLOR    = (200, 230, 255, 255)   # light sky blue
TRUNK_COLOR = (101,  67,  33, 255)   # brown
LEAF_DARK   = ( 34, 120,  34, 255)   # dark green (bottom layer)
LEAF_MID    = ( 56, 160,  56, 255)   # mid green
LEAF_LIGHT  = ( 80, 200,  80, 255)   # bright green (top layer)
TEXT_COLOR  = (255, 255, 255, 255)   # white for "XOP" label


def create_base_image(size: int = 256) -> Image.Image:
    img  = Image.new("RGBA", (size, size), BG_COLOR)
    draw = ImageDraw.Draw(img)

    s  = size
    cx = s / 2

    # ---- Trunk -------------------------------------------------------
    tw  = s * 0.10   # trunk width
    th  = s * 0.22   # trunk height
    ty0 = s * 0.78   # trunk top y
    ty1 = ty0 + th   # trunk bottom y
    draw.rectangle(
        [cx - tw / 2, ty0, cx + tw / 2, ty1],
        fill=TRUNK_COLOR,
    )

    # ---- Foliage — three stacked triangles (bottom → top) ---------------
    # Each layer is slightly narrower and higher to give a classic pine shape.
    layers = [
        # (base_y, tip_y, half_width, color)
        (s * 0.82, s * 0.55, s * 0.40, LEAF_DARK),
        (s * 0.68, s * 0.38, s * 0.32, LEAF_MID),
        (s * 0.52, s * 0.18, s * 0.22, LEAF_LIGHT),
    ]
    for base_y, tip_y, hw, color in layers:
        pts = [
            (cx,       tip_y),
            (cx - hw,  base_y),
            (cx + hw,  base_y),
        ]
        draw.polygon(pts, fill=color)

    # ---- "XOP" label ---------------------------------------------------
    label     = "XOP"
    font_size = max(12, s // 9)
    try:
        font = ImageFont.truetype("arialbd.ttf", font_size)
    except Exception:
        try:
            font = ImageFont.truetype("arial.ttf", font_size)
        except Exception:
            font = ImageFont.load_default()

    bbox = draw.textbbox((0, 0), label, font=font)
    tw_text = bbox[2] - bbox[0]
    tx = (s - tw_text) // 2
    ty = int(s * 0.92)
    draw.text((tx, ty), label, fill=TEXT_COLOR, font=font)

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

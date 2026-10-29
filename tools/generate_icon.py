#!/usr/bin/env python3
"""Generates the application icon (src/resources/icon.ico and icon.png).

The icon is a play triangle above two timeline clips on a dark rounded tile.
The very same geometry is drawn at runtime in the title bar by drawAppIcon(),
see src/app/AppIcon.cpp: keep both in sync when changing the design.

Requires Pillow. Usage: python tools/generate_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

# Geometry, relative to the icon size (same values as in AppIcon.cpp).
TILE_ROUNDING = 0.22

PLAY_LEFT   = 0.34
PLAY_RIGHT  = 0.74
PLAY_TOP    = 0.20
PLAY_BOTTOM = 0.64

CLIPS_TOP     = 0.72
CLIPS_BOTTOM  = 0.82
CLIPS_LEFT    = 0.18
CLIPS_CUT     = 0.56
CLIPS_GAP     = 0.05
CLIPS_RIGHT   = 0.82
CLIP_ROUNDING = 0.03

# Colors (same values as in AppIcon.cpp).
TILE_COLOR  = (18, 21, 28, 255)
PLAY_COLOR  = (255, 122, 0, 255)
CLIP1_COLOR = (84, 142, 214, 255)
CLIP2_COLOR = (70, 176, 128, 255)

# Sizes embedded in the .ico: 16 to 48 pixels at 100% to 200% scaling, then
# the large sizes used by the Explorer.
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
PNG_SIZE = 256

# Each size is rendered on its own, supersampled, rather than downscaled from
# the largest one.
SUPERSAMPLING = 8

OUTPUT_DIR = Path(__file__).resolve().parent.parent / "src" / "resources"


def render(size):
    full = size * SUPERSAMPLING

    image = Image.new("RGBA", (full, full), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def at(x, y):
        return (x * full, y * full)

    # Tile
    draw.rounded_rectangle(
        (0, 0, full - 1, full - 1),
        radius=TILE_ROUNDING * full,
        fill=TILE_COLOR,
    )

    # Play triangle
    draw.polygon(
        [
            at(PLAY_LEFT, PLAY_TOP),
            at(PLAY_RIGHT, 0.5 * (PLAY_TOP + PLAY_BOTTOM)),
            at(PLAY_LEFT, PLAY_BOTTOM),
        ],
        fill=PLAY_COLOR,
    )

    # Clips
    draw.rounded_rectangle(
        (*at(CLIPS_LEFT, CLIPS_TOP), *at(CLIPS_CUT - 0.5 * CLIPS_GAP, CLIPS_BOTTOM)),
        radius=CLIP_ROUNDING * full,
        fill=CLIP1_COLOR,
    )
    draw.rounded_rectangle(
        (*at(CLIPS_CUT + 0.5 * CLIPS_GAP, CLIPS_TOP), *at(CLIPS_RIGHT, CLIPS_BOTTOM)),
        radius=CLIP_ROUNDING * full,
        fill=CLIP2_COLOR,
    )

    return image.resize((size, size), Image.Resampling.LANCZOS)


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    images = [render(size) for size in ICO_SIZES]

    # Pillow picks, for each requested size, the matching appended image.
    images[-1].save(
        OUTPUT_DIR / "icon.ico",
        sizes=[(size, size) for size in ICO_SIZES],
        append_images=images[:-1],
    )

    render(PNG_SIZE).save(OUTPUT_DIR / "icon.png")

    print(f"Generated icon.ico and icon.png in {OUTPUT_DIR}")


if __name__ == "__main__":
    main()

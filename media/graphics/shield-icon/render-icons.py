#!/usr/bin/env python3
"""
PROJECT:     LiberNT user32
LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
PURPOSE:     Render the IDI_SHIELD elevation icon from the Fluent shield glyph
COPYRIGHT:   Copyright 2026 Leeway Open Software
"""

from pathlib import Path
import runpy

SIZES = (16, 20, 24, 32, 40, 48)
TARGET = "win32ss/user/user32/resources/oic_shield.ico"


def main():
    assets = Path(__file__).resolve().parent
    root = assets.parents[2]
    helpers = runpy.run_path(str(assets.parent / "user-folders/render-icons.py"))
    designs = sorted(int(f.stem.split("_")[-2]) for f in (assets / "fluent").glob("ic_fluent_shield_*_filled.svg"))
    frames = []
    for size in SIZES:
        design = next((d for d in designs if d >= size), designs[-1])
        frames.append(helpers["render"](assets / "fluent" / ("ic_fluent_shield_%d_filled.svg" % design), size))
    helpers["write_frames"](frames, root / TARGET)
    print("shield -> " + TARGET)


if __name__ == "__main__":
    main()

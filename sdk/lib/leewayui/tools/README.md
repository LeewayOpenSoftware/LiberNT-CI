# leewayui icon tools

Build-time only. These regenerate committed artifacts; nothing here runs at app runtime.

## gen-icon-ids.py
Generates `sdk/include/leewayui/ribbon_icons.h` (the icon_id enum) and
`sdk/lib/leewayui/aureline_slugs.inc` (id -> "app/slug") from the Aureline SVG
masters. Run and commit the diff when masters are added/renamed.

## gen-aureline-ico.py
Rasterises the Aureline SVG masters to multi-size `.ico` strips (16/24/32, colour
and mono) under `sdk/lib/leewayui/icons/<app>/`, which the stock resolver loads.
Commit the generated `.ico` and this script; do NOT commit the rasteriser exe or
the intermediate PNGs.

```
RESVG=/path/to/resvg.exe python gen-aureline-ico.py <MASTERS_DIR> ../icons
```

### Pinned rasteriser: resvg
A standalone, dependency-free SVG renderer (single static exe, MPL-2.0). It is a
build-time tool and is NOT committed to the repo.

- Version (pinned): **v0.50.0**
- Asset: `resvg-win-x86_64.zip`
- URL: https://github.com/linebender/resvg/releases/download/v0.50.0/resvg-win-x86_64.zip
- zip sha256: `e4817e40ac63d3eb5c9e5762526cf6cc91f0cc108de38cd09dd60be765184efd`

Download, verify the sha256, unzip, and point `RESVG` at `resvg.exe`. The masters
currently live in `shell/appframework/icons/<app>/aur.<app>.<slug>@{16,32,mono}.svg`
(original Leeway art, MIT); pass that directory as `MASTERS_DIR`.

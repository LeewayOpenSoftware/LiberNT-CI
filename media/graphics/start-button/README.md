# Start button logo

default.png, hover.png and pressed.png are 640x640 RGBA images of the Orosi neon arch (cyan left, violet right, transparent
background, no text) in its normal, brighter and darker states. They were AI-generated for LiberNT with the owner brand board
as the reference and delivered by the owner on 2026-11-18; they are original LiberNT art, GPL-3.0-or-later, subject to the
terms that apply to the generation tool.

The three reactos-start-*.svg files keep their old names for compatibility. Each embeds the matching PNG (viewBox
"-4 -4 40 40"); they are not vector traces. render.js re-exports them with @resvg/resvg-js. base/shell/explorer/res/ico/258.ico
is the fallback icon (16, 20, 24, 28, 32, 36, 40, 48 and 64 px DIB images with AND masks), used when the PNG decoder is
unavailable. The logo is drawn at 36x36 at 96 DPI centred in the 48x48 Start button; disabled and focused states reuse the
normal image.

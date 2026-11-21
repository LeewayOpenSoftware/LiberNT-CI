Twemoji Mozilla 0.7.0 (COLR version 0), the colour emoji font for DirectWrite (COLR layers
via TranslateColorGlyphRun). GDI cannot draw it: its base glyphs are empty, so "Segoe UI Emoji"
resolves to Noto Emoji. Shipped unmodified under the name TwemojiMozilla.ttf (upstream asset
Twemoji.Mozilla.ttf).

Origin:   https://github.com/mozilla/twemoji-colr/releases/tag/v0.7.0 (released 2022-10-28)
SHA-256:  6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e
Licences: the code that builds the font is Apache License 2.0 (Apache-2.0.txt),
          Copyright 2016-2018 Mozilla Foundation; the emoji artwork is Twemoji by Twitter, Inc.
          and other contributors, licensed under CC BY 4.0 (CC-BY-4.0.txt). LICENSE.md is the
          upstream licence file of the project and states both.
Credit:   Twemoji graphics copyright Twitter, Inc. and other contributors, CC BY 4.0
          (https://creativecommons.org/licenses/by/4.0/). The font is a derivative of that
          artwork converted to COLR layers by Mozilla; LiberNT ships Mozilla's file as is.
Coverage: 1418 code points in the cmap, 13723 glyphs; sequences (flags, ZWJ, skin tones) are
          GSUB ligatures. GDI draws the monochrome base outlines; colour needs a COLR
          capable renderer (DirectWrite, F10b).

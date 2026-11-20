# Icon fonts

`mkiconfont.py` builds `../segmdl2.ttf` ("Segoe MDL2 Assets") and
`../SegoeIcons.ttf` ("Segoe Fluent Icons") from:

- `iconmap.csv`: one row per code point published for the two Windows icon
  fonts (Microsoft Learn, "Segoe MDL2 Assets icons" and "Segoe Fluent Icons
  font").
- the Fluent UI System Icons fonts (MIT), at the commit and hashes pinned in
  the script;
- `../segmdl2.ttx` for the caption buttons (U+E8BB, U+E921-E923, U+EF2C-EF2F).

```
git clone https://github.com/microsoft/fluentui-system-icons
git -C fluentui-system-icons checkout <FLUENT_COMMIT from mkiconfont.py>
python3 mkiconfont.py fluentui-system-icons/fonts ..
```

Requires fontTools 4.x. The output is byte-reproducible.

## iconmap.csv

| Column | Meaning |
|---|---|
| codepoint | hex code point |
| fonts | `m` in segmdl2.ttf, `f` in SegoeIcons.ttf |
| name | Windows icon name (Fluent name / MDL2 name when they differ) |
| source | Fluent UI System Icons base name; empty = not provided yet |
| style | `regular` or `filled` |
| match | how the source was chosen: `exact`, `family` (numbered set), `manual`, `partial` (part of the name), `fuzzy` (shared word), `caption`, `none` |

The 20 px design is used when it exists, then 16, 24, 28, 32, 12, 48 and 10.
To improve a glyph, edit its row (and set `match` to `manual`), then rerun
the script.

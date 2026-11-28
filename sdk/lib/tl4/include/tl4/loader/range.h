/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Value ranges of REQUIREMENTS section 6.1 (R-MOD-3, R-MOD-7): one table, one implementation, here
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The range function takes the type and the property id of a model record and the value(s) a getter returned, and
 * answers in range (TL4_OK) or out of range (TL4_E_RANGE); it changes no value. A property row of the table wins over
 * the type row, and the narrower row wins when a property is in two. A consumer (the uxtheme adapter) calls it after
 * every getter and holds no ranges of its own; out of range is a reject of that property. The "every other ..." sets
 * of the table are closed by the lists below (property numbers of vssym32.h); tl4_range_members() exposes them so that
 * the test compares them with the table as written.
 */
#ifndef TL4_LOADER_RANGE_H
#define TL4_LOADER_RANGE_H

#include <stdint.h>
#include "status.h"
#include "model.h"

enum tl4_range_row {
    TL4_RANGE_NONE = 0,   /* no value range: COLOR, and the reference types resolved by the validator */
    TL4_RANGE_PIXEL,      /* INT, SIZE, the components of POSITION / RECT / MARGINS, the entries of an INTLIST: -8,192..8,192 */
    TL4_RANGE_DIVISOR,    /* counts used as a divisor of an image size: 1..256 */
    TL4_RANGE_INDEX,      /* values used as an index: 0..255 */
    TL4_RANGE_LEVEL,      /* 8-bit levels: 0..255 */
    TL4_RANGE_TIME,       /* durations and delays in milliseconds: 0..65,535 */
    TL4_RANGE_DPI,        /* DPI values: 1..8,192 */
    TL4_RANGE_METRIC,     /* system metric sizes taken from a style: 0..512 */
    TL4_RANGE_BOOL,       /* 0 or 1 */
    TL4_RANGE_ENUM,       /* 0..255; the consumer accepts only the values it knows for that property */
    TL4_RANGE_GLYPH,      /* TMT_GLYPHINDEX: one UTF-16 code unit of the glyph font, 0..65,535 (ruling 33bfd74) */
    TL4_RANGE_COLOR_INT,  /* colours a style carries as INT (TMT_COLORIZATIONCOLOR): any 32-bit value (ruling 33bfd74) */
    TL4_RANGE_GLOW,       /* TMT_GLOWINTENSITY: 0..1,024, not an 8-bit level in real styles (ruling 33bfd74) */
    TL4_RANGE_NEVER       /* always out of range: HBITMAP, FLOAT, FLOATLIST, and any type the table does not name */
};

#define TL4_RANGE_PIXEL_MIN   (-8192)
#define TL4_RANGE_PIXEL_MAX   8192
#define TL4_RANGE_DIVISOR_MAX 256
#define TL4_RANGE_INDEX_MAX   255
#define TL4_RANGE_LEVEL_MAX   255
#define TL4_RANGE_TIME_MAX    65535
#define TL4_RANGE_DPI_MAX     8192
#define TL4_RANGE_METRIC_MAX  512
#define TL4_RANGE_ENUM_MAX    255
#define TL4_RANGE_GLYPH_MAX   65535
#define TL4_RANGE_GLOW_MAX    1024
#define TL4_RANGE_INTLIST_MAX 402     /* entries of an INTLIST (R-V4-6) */
#define TL4_RANGE_STRING_MAX  255     /* characters of a referenced string (R-RES-2) */
#define TL4_RANGE_FACE_MAX    31      /* characters of a font face (LF_FACESIZE) */
#define TL4_RANGE_POINTS_MAX  255

/* The row that decides for (type, property id): a property row first, then the type. */
enum tl4_range_row tl4_range_row(uint32_t type, uint32_t name_id);

/* The numeric getters: `count` values of one record, 1 for INT / SIZE / BOOL / ENUM / COLOR, 2 for POSITION, 4 for
 * RECT and MARGINS, 0..TL4_RANGE_INTLIST_MAX for an INTLIST, 0 for a reference type. A count that does not fit the
 * type, or NULL values with a count, is TL4_E_ARG. TL4_OK when every value is inside its row, else TL4_E_RANGE. */
tl4_status tl4_range_check(uint32_t type, uint32_t name_id, const int32_t *values, uint32_t count);

/* FONT: face 1..31 characters, size 1..255 points (R-RES-2). */
tl4_status tl4_range_check_font(const tl4_font *font);

/* STRING: 1..255 characters when a record references it (R-RES-2; a code unit 0 is refused by the reader, R-RES-8). */
tl4_status tl4_range_check_string(uint32_t chars);

/* The property ids listed for a row (sorted ascending); 0 and *ids NULL for a row that is decided by the type alone.
 * The test compares each list with REQUIREMENTS section 6.1. */
uint32_t tl4_range_members(enum tl4_range_row row, const uint32_t **ids);

#endif

/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Value ranges of REQUIREMENTS section 6.1 (R-MOD-3, R-MOD-7)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Property numbers are those of vssym32.h (TMT_*). Each list closes one "and every other ..." set of the table; a
 * property that the consumer uses as a divisor, an index, a level, a time or a DPI value and is missing here is a
 * defect of this file, reviewed with the code.
 */
#include <stddef.h>
#include <tl4/loader/range.h>
#include <tl4/loader/v4.h>

/* 1..256: TMT_IMAGECOUNT (the image strip is divided by it), TMT_FRAMESPERSECOND (1000 / fps) */
static const uint32_t divisor_ids[] = { 2401, 2426 };
/* 0..255, "every value that the consumer uses as an index into an object": no named member today (TMT_GLYPHINDEX is
 * a glyph, below); the row stays for the next one */
static const uint32_t index_ids[1];   /* unused: the row has no member; a sized array keeps the table shape */
#define N_INDEX 0u
/* 0..65,535: TMT_GLYPHINDEX, one UTF-16 code unit of the glyph font */
static const uint32_t glyph_ids[] = { 2418 };
/* any 32-bit value: TMT_COLORIZATIONCOLOR, a colour carried as INT */
static const uint32_t color_int_ids[] = { 2431 };
/* 0..1,024: TMT_GLOWINTENSITY */
static const uint32_t glow_ids[] = { 2429 };
/* 0..255: TMT_ALPHALEVEL, TMT_GRADIENTRATIO1..5, TMT_SATURATION, TMT_ALPHATHRESHOLD, TMT_OPACITY, TMT_COLORIZATIONOPACITY
 * (not TMT_GLOWINTENSITY) */
static const uint32_t level_ids[] = { 2402, 2406, 2407, 2408, 2409, 2410, 2413, 2415, 2430, 2432 };
/* 0..65,535: TMT_ANIMATIONDELAY, TMT_ANIMATIONDURATION, the entries of TMT_TRANSITIONDURATIONS */
static const uint32_t time_ids[] = { 2428, 5006, 6000 };
/* 1..8,192: TMT_MINDPI1..5, TMT_MINDPI6, TMT_MINDPI7 */
static const uint32_t dpi_ids[] = { 2420, 2421, 2422, 2423, 2424, 2433, 2434 };
/* 0..512: TMT_SIZINGBORDERWIDTH .. TMT_PADDEDBORDERWIDTH */
static const uint32_t metric_ids[] = { 1201, 1202, 1203, 1204, 1205, 1206, 1207, 1208, 1209, 1210 };

#define N(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static int in_list(const uint32_t *ids, uint32_t n, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < n; i++) if (ids[i] == id) return 1;
    return 0;
}

uint32_t tl4_range_members(enum tl4_range_row row, const uint32_t **ids)
{
    const uint32_t *list = 0;
    uint32_t n = 0;
    switch (row)
    {
        case TL4_RANGE_DIVISOR: list = divisor_ids; n = N(divisor_ids); break;
        case TL4_RANGE_INDEX:   list = N_INDEX ? index_ids : 0; n = N_INDEX; break;
        case TL4_RANGE_GLYPH:   list = glyph_ids;   n = N(glyph_ids);   break;
        case TL4_RANGE_COLOR_INT: list = color_int_ids; n = N(color_int_ids); break;
        case TL4_RANGE_GLOW:    list = glow_ids;    n = N(glow_ids);    break;
        case TL4_RANGE_LEVEL:   list = level_ids;   n = N(level_ids);   break;
        case TL4_RANGE_TIME:    list = time_ids;    n = N(time_ids);    break;
        case TL4_RANGE_DPI:     list = dpi_ids;     n = N(dpi_ids);     break;
        case TL4_RANGE_METRIC:  list = metric_ids;  n = N(metric_ids);  break;
        default: break;
    }
    if (ids) *ids = list;
    return n;
}

enum tl4_range_row tl4_range_row(uint32_t type, uint32_t name_id)
{
    switch (type)
    {
        case TL4_V4_INT: case TL4_V4_SIZE: case TL4_V4_POSITION: case TL4_V4_RECT: case TL4_V4_MARGINS: case TL4_V4_INTLIST:
        case TL4_V4_BOOL: case TL4_V4_ENUM: case TL4_V4_COLOR: case TL4_V4_STRING: case TL4_V4_FONT:
        case TL4_V4_FILENAME: case TL4_V4_DISKSTREAM: case TL4_V4_STREAM: case TL4_V4_BITMAPREF:
            break;
        default:
            return TL4_RANGE_NEVER;                       /* HBITMAP, FLOAT, FLOATLIST, and any type the table does not name */
    }
    /* a property row wins over the type row; the lists are disjoint, so the narrower-row rule needs no second step */
    if (in_list(divisor_ids, N(divisor_ids), name_id)) return TL4_RANGE_DIVISOR;
    if (in_list(index_ids, N_INDEX, name_id)) return TL4_RANGE_INDEX;
    if (in_list(glyph_ids, N(glyph_ids), name_id)) return TL4_RANGE_GLYPH;
    if (in_list(color_int_ids, N(color_int_ids), name_id)) return TL4_RANGE_COLOR_INT;
    if (in_list(glow_ids, N(glow_ids), name_id)) return TL4_RANGE_GLOW;
    if (in_list(level_ids, N(level_ids), name_id)) return TL4_RANGE_LEVEL;
    if (in_list(time_ids, N(time_ids), name_id)) return TL4_RANGE_TIME;
    if (in_list(dpi_ids, N(dpi_ids), name_id)) return TL4_RANGE_DPI;
    if (in_list(metric_ids, N(metric_ids), name_id)) return TL4_RANGE_METRIC;
    switch (type)
    {
        case TL4_V4_INT: case TL4_V4_SIZE: case TL4_V4_POSITION: case TL4_V4_RECT: case TL4_V4_MARGINS: case TL4_V4_INTLIST:
            return TL4_RANGE_PIXEL;
        case TL4_V4_BOOL: return TL4_RANGE_BOOL;
        case TL4_V4_ENUM: return TL4_RANGE_ENUM;
        default:          return TL4_RANGE_NONE;          /* COLOR, STRING and FONT (own checks), the reference types */
    }
}

/* the number of values a getter returns for a type; UINT32_MAX for a list, 0 for a type without numeric values */
static uint32_t values_of(uint32_t type)
{
    switch (type)
    {
        case TL4_V4_INT: case TL4_V4_SIZE: case TL4_V4_BOOL: case TL4_V4_ENUM: case TL4_V4_COLOR: return 1;
        case TL4_V4_POSITION: return 2;
        case TL4_V4_RECT: case TL4_V4_MARGINS: return 4;
        case TL4_V4_INTLIST: return UINT32_MAX;
        default: return 0;
    }
}

static int bounds(enum tl4_range_row row, int32_t *lo, int32_t *hi)
{
    switch (row)
    {
        case TL4_RANGE_PIXEL:   *lo = TL4_RANGE_PIXEL_MIN; *hi = TL4_RANGE_PIXEL_MAX; return 1;
        case TL4_RANGE_DIVISOR: *lo = 1; *hi = TL4_RANGE_DIVISOR_MAX; return 1;
        case TL4_RANGE_INDEX:   *lo = 0; *hi = TL4_RANGE_INDEX_MAX; return 1;
        case TL4_RANGE_LEVEL:   *lo = 0; *hi = TL4_RANGE_LEVEL_MAX; return 1;
        case TL4_RANGE_TIME:    *lo = 0; *hi = TL4_RANGE_TIME_MAX; return 1;
        case TL4_RANGE_DPI:     *lo = 1; *hi = TL4_RANGE_DPI_MAX; return 1;
        case TL4_RANGE_METRIC:  *lo = 0; *hi = TL4_RANGE_METRIC_MAX; return 1;
        case TL4_RANGE_BOOL:    *lo = 0; *hi = 1; return 1;
        case TL4_RANGE_ENUM:    *lo = 0; *hi = TL4_RANGE_ENUM_MAX; return 1;
        case TL4_RANGE_GLYPH:   *lo = 0; *hi = TL4_RANGE_GLYPH_MAX; return 1;
        case TL4_RANGE_GLOW:    *lo = 0; *hi = TL4_RANGE_GLOW_MAX; return 1;
        default: return 0;                                /* NONE, COLOR_INT and NEVER have no bounds */
    }
}

tl4_status tl4_range_check(uint32_t type, uint32_t name_id, const int32_t *values, uint32_t count)
{
    enum tl4_range_row row = tl4_range_row(type, name_id);
    uint32_t want = values_of(type), i;
    int32_t lo, hi;

    if (row == TL4_RANGE_NEVER) return TL4_E_RANGE;
    if (want == UINT32_MAX) { if (count > TL4_RANGE_INTLIST_MAX) return TL4_E_RANGE; }
    else if (count != want) return TL4_E_ARG;
    if (count && !values) return TL4_E_ARG;
    if (!bounds(row, &lo, &hi)) return TL4_OK;             /* NONE: COLOR and the reference types */
    for (i = 0; i < count; i++)
        if (values[i] < lo || values[i] > hi) return TL4_E_RANGE;
    return TL4_OK;
}

tl4_status tl4_range_check_font(const tl4_font *font)
{
    if (!font) return TL4_E_ARG;
    if (font->face_chars < 1 || font->face_chars > TL4_RANGE_FACE_MAX) return TL4_E_RANGE;
    if (font->points < 1 || font->points > TL4_RANGE_POINTS_MAX) return TL4_E_RANGE;
    return TL4_OK;
}

tl4_status tl4_range_check_string(uint32_t chars)
{
    return chars >= 1 && chars <= TL4_RANGE_STRING_MAX ? TL4_OK : TL4_E_RANGE;
}

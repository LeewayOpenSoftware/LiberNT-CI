/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Version-4 container validator (REQUIREMENTS section 4)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include "tl4/loader/v4.h"
#include "tl4/loader/strings.h"

#define RECORD_HEADER 32u
#define NO_BASE 0xFFFFFFFFu

/* ---- bounded reads (R-GEN-2) ------------------------------------------------------------------------------- */

static int in_range(uint32_t size, uint64_t off, uint64_t len)
{
    return off <= size && len <= (uint64_t)size - off;
}

/* Every read names the buffer, its size and an offset, and fails by itself (R-GEN-2), as in pe.c. */
static int rd32(const uint8_t *d, uint32_t size, uint64_t off, uint32_t *v)
{
    if (!in_range(size, off, 4)) return 0;
    *v = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) | ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
    return 1;
}

static int rd16(const uint8_t *d, uint32_t size, uint64_t off, uint32_t *v)
{
    if (!in_range(size, off, 2)) return 0;
    *v = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8);
    return 1;
}

/* ---- class map (R-V4-2) ------------------------------------------------------------------------------------ */

/* Length in characters of the NUL-terminated UTF-16 string at off, or 0 when no NUL lies inside the map. */
static uint32_t utf16z_len(const uint8_t *data, uint32_t size, uint32_t off)
{
    uint32_t n = 0, c;
    while (rd16(data, size, (uint64_t)off + 2u * n, &c))
    {
        if (c == 0) return n;
        n++;
    }
    return 0; /* unterminated: treated as "no string here" by the caller, which then rejects */
}

/* Check one name: at most one "::", both halves 1..59 characters (R-V4-2). */
static int name_shape_ok(const uint8_t *data, uint32_t size, uint32_t off, uint32_t chars)
{
    uint32_t i, sep = chars, a, b;
    for (i = 0; i + 1 < chars; i++)
    {
        if (!rd16(data, size, (uint64_t)off + 2u * i, &a) || !rd16(data, size, (uint64_t)off + 2u * (i + 1), &b)) return 0;
        if (a == ':' && b == ':')
        {
            if (sep != chars) return 0;    /* a second "::" */
            sep = i;
            i++;
        }
    }
    if (sep == chars) return chars >= 1 && chars <= TL4_V4_MAX_NAME_HALF;
    return sep >= 1 && sep <= TL4_V4_MAX_NAME_HALF && chars - sep - 2 >= 1 && chars - sep - 2 <= TL4_V4_MAX_NAME_HALF;
}

tl4_status tl4_v4_check_cmap(const uint8_t *data, uint32_t size, tl4_v4_cmap *out)
{
    uint32_t off = 0, count = 0, c;
    if (!out) return TL4_E_ARG;
    out->data = 0; out->size = 0; out->count = 0;
    if (!data) return TL4_E_ARG;
    if (size > TL4_V4_MAX_CMAP_BYTES) return TL4_E_V4_LIMIT;
    if (size < 2 || (size & 1u)) return TL4_E_V4_CMAP;

    /* first pass: count the names and check each one's shape; the padding between names is zero code units */
    while (off < size)
    {
        if (!rd16(data, size, off, &c)) return TL4_E_V4_CMAP;
        if (c == 0) { off += 2; continue; }
        {
            uint32_t len = utf16z_len(data, size, off);
            if (len == 0) return TL4_E_V4_CMAP;            /* unterminated */
            if (!name_shape_ok(data, size, off, len)) return TL4_E_V4_CMAP;
            if (count == TL4_V4_MAX_CLASSES) return TL4_E_V4_LIMIT;
            count++;
            off += 2u * (len + 1);
        }
    }
    if (count == 0) return TL4_E_V4_CMAP;
    out->data = data; out->size = size; out->count = count;
    /* a repeated name is allowed (R-V4-2 ruling): no pairwise pass over the map (R-V4-14) */
    return TL4_OK;
}

tl4_status tl4_v4_cmap_name(const tl4_v4_cmap *cmap, uint32_t ordinal, const uint8_t **utf16, uint32_t *chars)
{
    uint32_t off = 0, n = 0, c;
    if (!cmap || !cmap->data || !utf16 || !chars) return TL4_E_ARG;
    *utf16 = 0; *chars = 0;
    if (ordinal >= cmap->count) return TL4_E_NOT_FOUND;
    while (off < cmap->size)
    {
        if (!rd16(cmap->data, cmap->size, off, &c)) return TL4_E_V4_CMAP;
        if (c == 0) { off += 2; continue; }
        {
            uint32_t len = utf16z_len(cmap->data, cmap->size, off);
            if (len == 0) return TL4_E_V4_CMAP;
            if (n == ordinal) { *utf16 = cmap->data + off; *chars = len; return TL4_OK; }
            n++;
            off += 2u * (len + 1);
        }
    }
    return TL4_E_NOT_FOUND;
}

/* ---- variant map (R-V4-3) ---------------------------------------------------------------------------------- */

static tl4_status vmap_string(const uint8_t *data, uint32_t size, uint32_t *off, const uint8_t **s, uint32_t *chars)
{
    uint32_t n, c;
    if (!rd32(data, size, *off, &n)) return TL4_E_V4_VMAP; /* characters including the terminating NUL */
    if (n < 2 || n > TL4_V4_MAX_VMAP_CHARS) return TL4_E_V4_VMAP;
    if (!in_range(size, (uint64_t)*off + 4, (uint64_t)n * 2)) return TL4_E_V4_VMAP;
    if (!rd16(data, size, (uint64_t)*off + 4 + 2u * (n - 1), &c) || c != 0) return TL4_E_V4_VMAP;
    *s = data + *off + 4;
    *chars = n - 1;
    *off = (uint32_t)(((uint64_t)*off + 4 + (uint64_t)n * 2 + 3) & ~(uint64_t)3);
    return TL4_OK;
}

tl4_status tl4_v4_check_vmap(const uint8_t *data, uint32_t size, tl4_v4_vmap *out)
{
    uint32_t off = 0;
    tl4_status st;
    if (!out) return TL4_E_ARG;
    out->variant = out->size_name = out->color = 0;
    out->variant_chars = out->size_chars = out->color_chars = 0;
    if (!data) return TL4_E_ARG;
    if (size > 3u * (4u + 2u * TL4_V4_MAX_VMAP_CHARS + 3u)) return TL4_E_V4_VMAP;
    if ((st = vmap_string(data, size, &off, &out->variant, &out->variant_chars)) != TL4_OK) goto fail;
    if ((st = vmap_string(data, size, &off, &out->size_name, &out->size_chars)) != TL4_OK) goto fail;
    if ((st = vmap_string(data, size, &off, &out->color, &out->color_chars)) != TL4_OK) goto fail;
    if (off != size) { st = TL4_E_V4_VMAP; goto fail; }    /* exactly three padded strings (corpus: 76 bytes) */
    return TL4_OK;
fail:
    out->variant = out->size_name = out->color = 0;
    out->variant_chars = out->size_chars = out->color_chars = 0;
    return st;
}

/* ---- base-class map (R-V4-10) ------------------------------------------------------------------------------ */

tl4_status tl4_v4_bcmap_base(const tl4_v4_bcmap *bcmap, uint32_t ordinal, uint32_t *base)
{
    uint32_t v;
    if (!bcmap || !bcmap->data || !base) return TL4_E_ARG;
    *base = NO_BASE;
    if (ordinal < bcmap->shift) return TL4_OK;            /* a descriptor name has no base */
    if (ordinal - bcmap->shift >= bcmap->count) return TL4_E_NOT_FOUND;
    if (!rd32(bcmap->data, 4u + 4u * bcmap->count, 4u + 4u * (ordinal - bcmap->shift), &v)) return TL4_E_NOT_FOUND;
    if (v != NO_BASE) *base = v + bcmap->shift;
    return TL4_OK;
}

tl4_status tl4_v4_check_bcmap(const uint8_t *data, uint32_t size, uint32_t cmap_count, tl4_v4_bcmap *out)
{
    uint32_t n, j;
    if (!out) return TL4_E_ARG;
    out->data = 0; out->count = 0; out->shift = 0; out->max_depth = 0;
    if (!data) return TL4_E_ARG;
    if (size < 4 || (size & 3u)) return TL4_E_V4_BCMAP;
    if (!rd32(data, size, 0, &n)) return TL4_E_V4_BCMAP;
    if ((uint64_t)n + 1 != size / 4) return TL4_E_V4_BCMAP;
    if (n > cmap_count || cmap_count - n > TL4_V4_MAX_DESCRIPTORS) return TL4_E_V4_BCMAP;
    out->data = data; out->count = n; out->shift = cmap_count - n;
    for (j = 0; j < n; j++)
    {
        uint32_t v, depth = 0, cur = j;
        /* the chain from j must end within TL4_V4_MAX_CHAIN steps: a cycle or a longer chain is a reject */
        for (;;)
        {
            if (!rd32(data, size, 4u + 4u * cur, &v)) goto bad;
            if (v == NO_BASE) break;
            cur = v;
            if (cur >= n) goto bad;
            if (++depth > TL4_V4_MAX_CHAIN) goto bad;
        }
        if (depth > out->max_depth) out->max_depth = depth;
    }
    return TL4_OK;
bad:
    out->data = 0; out->count = 0; out->shift = 0; out->max_depth = 0;
    return TL4_E_V4_BCMAP;
}

/* ---- record walk (R-V4-4..8) ------------------------------------------------------------------------------- */

/* exact inline size of a known type, 0 when the type carries a reference, UINT32_MAX for "variable" (INTLIST,
 * STRING), and 1 for unknown types */
static uint32_t inline_size(uint32_t type_id)
{
    switch (type_id)
    {
        case TL4_V4_ENUM: case TL4_V4_INT: case TL4_V4_BOOL: case TL4_V4_COLOR: case TL4_V4_SIZE: return 4;
        case TL4_V4_POSITION: return 8;
        case TL4_V4_RECT: case TL4_V4_MARGINS: return 16;
        case TL4_V4_INTLIST: case TL4_V4_STRING: return 0xFFFFFFFFu;
        case TL4_V4_FONT: case TL4_V4_FILENAME: case TL4_V4_DISKSTREAM: return 0;
        default: return 1;
    }
}

/* FONT, FILENAME and DISKSTREAM always carry a reference; STRING may (a string-table id, as every RMAP uses) */
static int is_reference_type(uint32_t type_id)
{
    return type_id == TL4_V4_FONT || type_id == TL4_V4_FILENAME || type_id == TL4_V4_DISKSTREAM || type_id == TL4_V4_STRING;
}

tl4_status tl4_v4_walk_records(const uint8_t *data, uint32_t size, uint32_t cmap_count, uint32_t max_bytes,
                               uint32_t max_records, tl4_v4_record_callback callback, void *context,
                               tl4_v4_walk_stats *stats)
{
    uint64_t off = 0;
    uint32_t seen = 0;
    uint32_t prev_class = 0xFFFFFFFFu, prev_part = 0, prev_state = 0;
    tl4_v4_walk_stats st;

    if (!stats) return TL4_E_ARG;
    st.records = st.dropped_unknown_type = st.dropped_ref_on_value = 0;
    st.partstates = st.classes = st.max_part = st.max_state = st.max_cb = 0;
    *stats = st;
    if (!data) return TL4_E_ARG;
    if (size > max_bytes) return TL4_E_V4_LIMIT;

    while (off < size)
    {
        tl4_v4_record r;
        uint32_t known, c;
        int accept = 1;

        if (!in_range(size, off, RECORD_HEADER)) return TL4_E_V4_END;   /* a partial header at the end */
        if (!rd32(data, size, off, &r.name_id) || !rd32(data, size, off + 4, &r.type_id) ||
            !rd32(data, size, off + 8, &r.class_id) || !rd32(data, size, off + 12, &r.part_id) ||
            !rd32(data, size, off + 16, &r.state_id) || !rd32(data, size, off + 20, &r.reference) ||
            !rd32(data, size, off + 28, &r.cb_data)) return TL4_E_V4_END;
        /* the seventh field (offset 24) is not interpreted: it is 0 in the stock styles and carries other values in
         * inline and reference records of third-party styles (R-V4-5 as amended) */
        r.data = 0;

        /* R-V4-5: every field unsigned and in range */
        if (r.name_id == 0 || r.name_id > 0xFFFFu) return TL4_E_V4_RECORD;
        if (r.type_id < 200 || r.type_id > 255) return TL4_E_V4_RECORD;
        if (r.class_id >= cmap_count) return TL4_E_V4_RECORD;
        if (r.part_id > 255 || r.state_id > 255) return TL4_E_V4_RECORD;
        if (r.cb_data > TL4_V4_MAX_CBDATA) return TL4_E_V4_RECORD;

        if (++seen > max_records) return TL4_E_V4_LIMIT;
        known = inline_size(r.type_id);

        if (r.reference != 0)
        {
            /* header-only record: the data size is logical (R-V4-7); the reference is never used as a length */
            if (!is_reference_type(r.type_id))
            {
                st.dropped_ref_on_value++;                 /* R-V4-8: a value type with a reference is dropped */
                accept = 0;
            }
            else if (r.type_id == TL4_V4_FONT && r.cb_data != 92)
                return TL4_E_V4_SIZE;
            off += RECORD_HEADER;
        }
        else
        {
            /* inline data must be inside the area (R-V4-5) and exact for its type (R-V4-6) */
            if (!in_range(size, off + RECORD_HEADER, r.cb_data)) return TL4_E_V4_RECORD;
            if (known == 0) return TL4_E_V4_SIZE;          /* FONT/FILENAME/DISKSTREAM carry a reference, not data */
            if (known == 1)
            {
                st.dropped_unknown_type++;                 /* R-V4-8: skipped by its checked size */
                accept = 0;
            }
            else if (known == 0xFFFFFFFFu)
            {
                if (r.type_id == TL4_V4_INTLIST)
                {
                    uint32_t count;
                    if (r.cb_data < 4) return TL4_E_V4_SIZE;
                    if (!rd32(data, size, off + RECORD_HEADER, &count)) return TL4_E_V4_SIZE;
                    if (count > TL4_V4_MAX_INTLIST) return TL4_E_V4_SIZE;
                    if (r.cb_data != 4u + 4u * count) return TL4_E_V4_SIZE;
                }
                else /* STRING */
                {
                    if (r.cb_data < 2 || (r.cb_data & 1u) || r.cb_data > TL4_V4_MAX_STRING_BYTES) return TL4_E_V4_SIZE;
                    if (!rd16(data, size, off + RECORD_HEADER + r.cb_data - 2, &c) || c != 0) return TL4_E_V4_SIZE;
                }
            }
            else if (r.cb_data != known)
                return TL4_E_V4_SIZE;
            r.data = data + off + RECORD_HEADER;
            off += RECORD_HEADER + r.cb_data;
        }
        off = (off + 7u) & ~(uint64_t)7u;                   /* R-GEN-3: 64-bit alignment step */
        if (off > size) return TL4_E_V4_END;

        if (r.part_id > st.max_part) st.max_part = r.part_id;
        if (r.state_id > st.max_state) st.max_state = r.state_id;
        if (r.cb_data > st.max_cb) st.max_cb = r.cb_data;
        if (r.class_id != prev_class) { st.classes++; prev_class = r.class_id; prev_part = r.part_id; prev_state = r.state_id; st.partstates++; }
        else if (r.part_id != prev_part || r.state_id != prev_state) { prev_part = r.part_id; prev_state = r.state_id; st.partstates++; }
        if (st.partstates > TL4_V4_MAX_PARTSTATES || st.classes > TL4_V4_MAX_CLASSES) return TL4_E_V4_LIMIT;

        if (accept)
        {
            st.records++;
            if (callback && callback(&r, context)) return TL4_E_V4_REFERENCE;
        }
    }
    if (off != size) return TL4_E_V4_END;
    *stats = st;
    return TL4_OK;
}

/* ---- string table (R-RES-1, bounded) ----------------------------------------------------------------------- */

/* The L1 entry point, now a wrapper of the L3 reader (loader/src/strings.c): the lowest language entry (the only
 * one in a version-4 file), and the 255-character bound of a referenced string (R-RES-2) reported as "not found", so
 * that a longer string stays an unresolved reference as before. */
tl4_status tl4_v4_find_string(const tl4_pe *pe, uint32_t id, const uint8_t **utf16, uint32_t *chars)
{
    uint32_t off = 0, n = 0;
    tl4_status st;
    if (!utf16 || !chars) return TL4_E_ARG;
    *utf16 = 0; *chars = 0;
    if ((st = tl4_strings_locate(pe, id, TL4_STR_LANG_LOWEST, &off, &n)) != TL4_OK) return st;
    if (n > TL4_V4_MAX_FONT_CHARS) return TL4_E_NOT_FOUND;
    *utf16 = pe->base + off;
    *chars = n;
    return TL4_OK;
}

/* ---- the whole container ----------------------------------------------------------------------------------- */

struct resolve_ctx {
    const tl4_pe *style;
    const tl4_pe *strings;
    tl4_v4_stats *stats;
};

static int resolve_cb(const tl4_v4_record *r, void *context)
{
    struct resolve_ctx *c = (struct resolve_ctx *)context;
    tl4_resource res;
    tl4_res_name type, name;
    const uint8_t *s; uint32_t n;

    if (r->reference == 0) return 0;
    switch (r->type_id)
    {
        case TL4_V4_FONT:
            c->stats->fonts++;
            if (tl4_v4_find_string(c->style, r->reference, &s, &n) != TL4_OK &&
                (!c->strings || tl4_v4_find_string(c->strings, r->reference, &s, &n) != TL4_OK))
                c->stats->unresolved_fonts++;
            break;
        case TL4_V4_STRING:
            c->stats->strings++;
            if (tl4_v4_find_string(c->style, r->reference, &s, &n) != TL4_OK &&
                (!c->strings || tl4_v4_find_string(c->strings, r->reference, &s, &n) != TL4_OK))
                c->stats->unresolved_strings++;
            break;
        case TL4_V4_FILENAME:
            c->stats->filenames++;
            type = tl4_res_ascii("IMAGE"); name = tl4_res_id(r->reference);
            if (r->reference > 0xFFFFu || tl4_pe_find(c->style, &type, &name, &res) != TL4_OK) c->stats->unresolved_filenames++;
            break;
        case TL4_V4_DISKSTREAM:
            c->stats->diskstreams++;
            type = tl4_res_ascii("STREAM"); name = tl4_res_id(r->reference);
            if (r->reference > 0xFFFFu || tl4_pe_find(c->style, &type, &name, &res) != TL4_OK) c->stats->unresolved_diskstreams++;
            break;
        default:
            break;
    }
    return 0;
}

static void clear_stats(tl4_v4_stats *s)
{
    tl4_v4_stats z = {0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    *s = z;
}

static tl4_status find_named(const tl4_pe *pe, const char *type, const char *name, tl4_resource *out)
{
    tl4_res_name t = tl4_res_ascii(type), n = tl4_res_ascii(name);
    tl4_status st = tl4_pe_find(pe, &t, &n, out);
    return st == TL4_E_NOT_FOUND ? TL4_E_V4_MISSING : st;
}

tl4_status tl4_v4_validate(const tl4_pe *style, const tl4_pe *strings, tl4_v4_stats *stats)
{
    tl4_resource res;
    tl4_res_name t, n;
    tl4_v4_cmap cmap;
    tl4_v4_vmap vmap;
    tl4_v4_bcmap bcmap;
    struct resolve_ctx ctx;
    tl4_strings_stats ss;
    tl4_status st;
    uint32_t unresolved, cnt;

    if (!stats) return TL4_E_ARG;
    clear_stats(stats);
    if (!style || !style->base) return TL4_E_ARG;
    if (strings && !strings->base) return TL4_E_ARG;

    /* R-PE-8: the version-4 path accepts no file with a multi-language pair (its accepted set is unchanged) */
    if (style->nmulti_pairs != 0 || (strings && strings->nmulti_pairs != 0)) { st = TL4_E_RSRC_LANGUAGES; goto fail; }

    /* R-RES-8: every block of both string tables passes the shape check before any reference is resolved */
    if ((st = tl4_strings_check(style, &ss)) != TL4_OK) goto fail;
    if (strings && (st = tl4_strings_check(strings, &ss)) != TL4_OK) goto fail;

    /* R-V4-1 */
    t = tl4_res_ascii("PACKTHEM_VERSION"); n = tl4_res_id(1);
    if (tl4_pe_find(style, &t, &n, &res) != TL4_OK || res.size != 2 || !rd16(res.data, res.size, 0, &cnt) || cnt != 4) { st = TL4_E_V4_VERSION; goto fail; }

    /* R-V4-2 */
    if ((st = find_named(style, "CMAP", "CMAP", &res)) != TL4_OK) goto fail;
    if ((st = tl4_v4_check_cmap(res.data, res.size, &cmap)) != TL4_OK) goto fail;
    stats->cmap_names = cmap.count;

    /* R-V4-3 */
    if ((st = find_named(style, "VMAP", "VMAP", &res)) != TL4_OK) goto fail;
    if ((st = tl4_v4_check_vmap(res.data, res.size, &vmap)) != TL4_OK) goto fail;

    /* R-V4-10 */
    if ((st = find_named(style, "BCMAP", "BCMAP", &res)) != TL4_OK) goto fail;
    if ((st = tl4_v4_check_bcmap(res.data, res.size, cmap.count, &bcmap)) != TL4_OK) goto fail;
    stats->bcmap_entries = bcmap.count;
    stats->descriptors = bcmap.shift;
    stats->bcmap_max_depth = bcmap.max_depth;

    /* R-V4-4..8 on both record areas; references resolved through the reader (R-V4-7) */
    ctx.style = style; ctx.strings = strings; ctx.stats = stats;
    if ((st = find_named(style, "RMAP", "RMAP", &res)) != TL4_OK) goto fail;
    if ((st = tl4_v4_walk_records(res.data, res.size, cmap.count, TL4_V4_MAX_RMAP_BYTES, TL4_V4_MAX_RECORDS,
                                  resolve_cb, &ctx, &stats->rmap)) != TL4_OK) goto fail;
    if ((st = find_named(style, "VARIANT", "NORMAL", &res)) != TL4_OK) goto fail;
    if ((st = tl4_v4_walk_records(res.data, res.size, cmap.count, TL4_V4_MAX_VARIANT_BYTES, TL4_V4_MAX_RECORDS,
                                  resolve_cb, &ctx, &stats->variant)) != TL4_OK) goto fail;

    /* resource counts (section 6) */
    t = tl4_res_ascii("IMAGE");
    if ((st = tl4_pe_count_type(style, &t, &cnt)) != TL4_OK) goto fail;
    stats->images = cnt;
    if (cnt > TL4_V4_MAX_IMAGES) { st = TL4_E_V4_LIMIT; goto fail; }
    t = tl4_res_ascii("STREAM");
    if ((st = tl4_pe_count_type(style, &t, &cnt)) != TL4_OK) goto fail;
    stats->streams = cnt;
    if (cnt > TL4_V4_MAX_STREAMS) { st = TL4_E_V4_LIMIT; goto fail; }
    t = tl4_res_id(6);
    if ((st = tl4_pe_count_type(style, &t, &cnt)) != TL4_OK) goto fail;
    stats->string_blocks = cnt;

    /* R-V4-12 */
    unresolved = stats->unresolved_fonts + stats->unresolved_filenames + stats->unresolved_diskstreams + stats->unresolved_strings;
    if (stats->unresolved_fonts != 0) { st = TL4_E_V4_REFERENCE; goto fail; }
    if ((uint64_t)unresolved * 100u > (uint64_t)stats->variant.records + stats->rmap.records) { st = TL4_E_V4_REFERENCE; goto fail; }
    return TL4_OK;

fail:
    clear_stats(stats);                                   /* R-GEN-5: no partial result */
    return st;
}

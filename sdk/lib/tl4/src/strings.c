/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Bounded string-table reader (R-RES-1, R-RES-2, R-RES-8)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <string.h>
#include <tl4/loader/strings.h>

#define SLOTS 16u

static int in_range(uint32_t size, uint64_t off, uint64_t len)
{
    return off <= size && len <= (uint64_t)size - off;
}

/* One read of a 16-bit count at `off` inside d[size] (R-GEN-2, R-GEN-11: the value is used from this read only). */
static int rd16(const uint8_t *d, uint32_t size, uint64_t off, uint32_t *v)
{
    if (!in_range(size, off, 2)) return 0;
    *v = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8);
    return 1;
}

static void clear_stats(tl4_strings_stats *s)
{
    tl4_strings_stats z = {0, 0, 0, 0, 0, 0};
    *s = z;
}

static int language_ok(uint32_t language)
{
    return language == TL4_STR_LANG_LOWEST || language <= 0xFFFFu;
}

/* ---- the shape check (R-RES-8) ------------------------------------------------------------------------------ */

struct check_ctx {
    tl4_strings_stats st;
    uint32_t prev_block;     /* name id of the previous entry: entries of a pair are consecutive (R-PE-8 order) */
    tl4_status status;
};

/* One block image: exactly 16 strings, each count at most TL4_STR_MAX_CHARS and inside the block, no code unit 0,
 * the last ending exactly on the size. */
static tl4_status check_block(const uint8_t *d, uint32_t size, uint32_t block, tl4_strings_stats *st)
{
    uint64_t off = 0;
    uint32_t i;

    if (size > TL4_STR_MAX_BLOCK) return TL4_E_STR_BLOCK;
    for (i = 0; i < SLOTS; i++)
    {
        uint32_t count, u;
        if (!rd16(d, size, off, &count)) return TL4_E_STR_BLOCK;
        off += 2;
        if (count > TL4_STR_MAX_CHARS || !in_range(size, off, 2u * (uint64_t)count)) return TL4_E_STR_BLOCK;
        for (u = 0; u < count; u++)
            if (d[off + 2u * u] == 0 && d[off + 2u * u + 1] == 0) return TL4_E_STR_BLOCK;
        if (count)
        {
            st->strings++;
            if (count > st->max_chars) { st->max_chars = count; st->longest_id = SLOTS * (block - 1u) + i; }
        }
        off += 2u * count;
    }
    return off == size ? TL4_OK : TL4_E_STR_BLOCK;
}

static int check_cb(const tl4_resource *r, void *context)
{
    struct check_ctx *c = (struct check_ctx *)context;
    tl4_status st;

    if (r->type.kind != TL4_NAME_ID || r->type.id != 6) return 0;
    if (r->name.kind != TL4_NAME_ID || r->name.id < 1 || r->name.id > TL4_STR_MAX_BLOCKS) { c->status = TL4_E_STR_BLOCK; return 1; }
    if ((st = check_block(r->data, r->size, r->name.id, &c->st)) != TL4_OK) { c->status = st; return 1; }
    c->st.entries++;
    if (c->st.blocks == 0 || r->name.id != c->prev_block) { c->st.blocks++; c->prev_block = r->name.id; }
    if (r->nlanguages > c->st.max_languages) c->st.max_languages = r->nlanguages;
    return 0;
}

tl4_status tl4_strings_check(const tl4_pe *pe, tl4_strings_stats *stats)
{
    struct check_ctx c;
    tl4_status st;
    int stopped = 0;

    if (!stats) return TL4_E_ARG;
    clear_stats(stats);
    if (!pe || !pe->base) return TL4_E_ARG;
    clear_stats(&c.st);
    c.prev_block = 0;
    c.status = TL4_OK;
    if ((st = tl4_pe_enum(pe, check_cb, &c, &stopped)) != TL4_OK) return st;
    if (stopped) return c.status;
    *stats = c.st;
    return TL4_OK;
}

/* ---- lookups (R-GEN-11: nothing below trusts the check above) ----------------------------------------------- */

tl4_status tl4_strings_locate(const tl4_pe *pe, uint32_t id, uint32_t language, uint32_t *offset, uint32_t *chars)
{
    tl4_res_name type = tl4_res_id(6), name;
    tl4_resource block;
    tl4_status st;
    uint64_t off = 0;
    uint32_t i, want;

    if (offset) *offset = 0;
    if (chars) *chars = 0;
    if (!offset || !chars || !pe || !pe->base) return TL4_E_ARG;
    if (id > 0xFFFFu || !language_ok(language)) return TL4_E_ARG;

    name = tl4_res_id(id / SLOTS + 1u);
    st = language == TL4_STR_LANG_LOWEST ? tl4_pe_find(pe, &type, &name, &block)
                                         : tl4_pe_find_language(pe, &type, &name, language, &block);
    if (st != TL4_OK) return st;
    /* the reader re-checked [block.offset, block.offset + block.size) against the mapping; bound the block itself */
    if (block.size > TL4_STR_MAX_BLOCK) return TL4_E_STR_BLOCK;

    want = id % SLOTS;
    for (i = 0; i <= want; i++)
    {
        uint32_t count;
        if (!rd16(block.data, block.size, off, &count)) return TL4_E_STR_BLOCK;
        off += 2;
        if (count > TL4_STR_MAX_CHARS || !in_range(block.size, off, 2u * (uint64_t)count)) return TL4_E_STR_BLOCK;
        if (i == want)
        {
            if (count == 0) return TL4_E_NOT_FOUND;
            *offset = block.offset + (uint32_t)off;
            *chars = count;
            return TL4_OK;
        }
        off += 2u * count;
    }
    return TL4_E_NOT_FOUND;   /* not reached: want < SLOTS */
}

tl4_status tl4_strings_get(const tl4_pe *pe, uint32_t id, uint32_t language, uint16_t *out, uint32_t cap_chars, uint32_t *chars)
{
    uint32_t off = 0, n = 0, i;
    tl4_status st;

    if (chars) *chars = 0;
    if (out && cap_chars) out[0] = 0;
    if (!out || !chars) return TL4_E_ARG;
    if ((st = tl4_strings_locate(pe, id, language, &off, &n)) != TL4_OK) return st;
    if (cap_chars == 0 || n > cap_chars - 1u) return TL4_E_BUFFER;         /* needed: n + 1 */
    if (n > TL4_STR_MAX_CHARS || off > pe->size || 2u * (uint64_t)n > pe->size - off) return TL4_E_STR_BLOCK;
    /* one copy (R-MOD-4), then the checks on the copy and nothing else is read from the mapping */
    for (i = 0; i < n; i++) out[i] = (uint16_t)((uint32_t)pe->base[off + 2u * i] | ((uint32_t)pe->base[off + 2u * i + 1] << 8));
    out[n] = 0;
    for (i = 0; i < n; i++)
        if (out[i] == 0)
        {
            memset(out, 0, 2u * ((size_t)n + 1u));           /* R-GEN-5: a failed call leaves no partial text behind */
            return TL4_E_STR_BLOCK;
        }
    *chars = n;
    return TL4_OK;
}

/* ---- the style / module pair (R-RES-1) ---------------------------------------------------------------------- */

tl4_status tl4_strings_open(const tl4_pe *style, const tl4_pe *mui_or_null, tl4_strings *out)
{
    tl4_strings_stats st;
    tl4_status rc;

    if (!out) return TL4_E_ARG;
    out->style = 0; out->mui = 0;
    if ((rc = tl4_strings_check(style, &st)) != TL4_OK) return rc;
    if (mui_or_null && (rc = tl4_strings_check(mui_or_null, &st)) != TL4_OK) return rc;
    out->style = style;
    out->mui = mui_or_null;
    return TL4_OK;
}

tl4_status tl4_strings_get2(const tl4_strings *s, uint32_t id, uint32_t language, uint16_t *out, uint32_t cap_chars, uint32_t *chars)
{
    tl4_status st;

    if (chars) *chars = 0;
    if (out && cap_chars) out[0] = 0;
    if (!s || !s->style || !out || !chars) return TL4_E_ARG;
    st = tl4_strings_get(s->style, id, language, out, cap_chars, chars);
    if (st != TL4_E_NOT_FOUND || !s->mui) return st;
    return tl4_strings_get(s->mui, id, language, out, cap_chars, chars);
}

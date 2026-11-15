/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Property model with binary values and the GetTheme* query subset (milestone L2)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <tl4/loader/model.h>
#include <string.h>

#ifdef TL4_TESTING
/* Only in the test build of the core: a shipped core has no writable hook. */
void (*tl4_model_test_between_passes)(const uint8_t *data, size_t size);
#endif

/* ---- bounded reads (R-GEN-2) ------------------------------------------------------------------------------- */

static int in_range(uint64_t size, uint64_t off, uint64_t len)
{
    return off <= size && len <= size - off;
}

static int rd16(const uint8_t *d, uint64_t size, uint64_t off, uint32_t *v)
{
    if (!in_range(size, off, 2)) return 0;
    *v = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8);
    return 1;
}

static uint32_t word(const uint8_t *copy, uint32_t off)
{
    return (uint32_t)copy[off] | ((uint32_t)copy[off + 1] << 8) | ((uint32_t)copy[off + 2] << 16) | ((uint32_t)copy[off + 3] << 24);
}

static uint64_t align8(uint64_t x) { return (x + 7u) & ~(uint64_t)7u; }

static void clear_model(tl4_model *m)
{
    memset(m, 0, sizeof(*m));
    m->counts.globals = TL4_NO_BASE;
    m->counts.sysmetrics = TL4_NO_BASE;
}

/* ---- sizing (R-MOD-5) --------------------------------------------------------------------------------------- */

static uint32_t ps_capacity(const tl4_v4_stats *s)
{
    return s->variant.partstates + s->rmap.partstates;   /* upper bounds from the walk, at most 2 x 8,192 */
}

static uint32_t prop_capacity(const tl4_v4_stats *s)
{
    return s->variant.records + s->rmap.records;         /* at most 2 x 32,768 */
}

size_t tl4_model_size(const tl4_v4_stats *s)
{
    uint64_t n;
    if (!s) return 0;
    if (s->cmap_names == 0 || s->cmap_names > TL4_V4_MAX_CLASSES) return 0;
    if (s->variant.partstates > TL4_V4_MAX_PARTSTATES || s->rmap.partstates > TL4_V4_MAX_PARTSTATES) return 0;
    if (s->variant.records > TL4_V4_MAX_RECORDS || s->rmap.records > TL4_V4_MAX_RECORDS) return 0;
    n = align8((uint64_t)sizeof(tl4_class) * s->cmap_names) + align8((uint64_t)sizeof(tl4_partstate) * ps_capacity(s)) +
        align8((uint64_t)sizeof(tl4_prop) * prop_capacity(s)) + align8(4u * (uint64_t)s->cmap_names);
    return (size_t)n;                                    /* at most about 3 MB: no overflow possible */
}

/* ---- names ---------------------------------------------------------------------------------------------------- */

/* The CMAP bytes of a validated model, re-checked against the mapping (R-GEN-11). */
static const uint8_t *cmap_bytes(const tl4_model *m)
{
    if (!m->style.base) return 0;
    if (!in_range(m->style.size, m->cmap_off, m->cmap_size)) return 0;
    return m->style.base + m->cmap_off;
}

/* Compare the class name of ordinal a with (utf16, chars): code units, then length. -1, 0, 1; 2 when unreadable.
 * At most TL4_MODEL_MAX_CLASS_QUERY code units are read. */
static int name_cmp_query(const tl4_model *m, const uint8_t *cmap, uint32_t a, const uint8_t *q, uint32_t qchars)
{
    const tl4_class *c = &m->classes[a];
    uint32_t i, n = c->name_chars < qchars ? c->name_chars : qchars, x, y;
    if (n > TL4_MODEL_MAX_CLASS_QUERY) return 2;
    for (i = 0; i < n; i++)
    {
        if (!rd16(cmap, m->cmap_size, (uint64_t)c->name_off + 2u * i, &x)) return 2;
        y = (uint32_t)q[2 * i] | ((uint32_t)q[2 * i + 1] << 8);
        if (x != y) return x < y ? -1 : 1;
    }
    if (c->name_chars == qchars) return 0;
    return c->name_chars < qchars ? -1 : 1;
}

/* Strict order of two ordinals by name; equal names order by ordinal (so the lowest ordinal comes first). An
 * unreadable name orders by ordinal; the sort below stays correct in memory and terminates whatever this returns. */
static int name_less(const tl4_model *m, const uint8_t *cmap, uint32_t a, uint32_t b)
{
    const tl4_class *ca = &m->classes[a], *cb = &m->classes[b];
    uint32_t i, n = ca->name_chars < cb->name_chars ? ca->name_chars : cb->name_chars, x, y;
    if (n > TL4_MODEL_MAX_CLASS_QUERY) n = TL4_MODEL_MAX_CLASS_QUERY;
    for (i = 0; i < n; i++)
    {
        if (!rd16(cmap, m->cmap_size, (uint64_t)ca->name_off + 2u * i, &x)) return a < b;
        if (!rd16(cmap, m->cmap_size, (uint64_t)cb->name_off + 2u * i, &y)) return a < b;
        if (x != y) return x < y;
    }
    if (ca->name_chars != cb->name_chars) return ca->name_chars < cb->name_chars;
    return a < b;
}

static int names_equal(const tl4_model *m, const uint8_t *cmap, uint32_t a, uint32_t b)
{
    const tl4_class *ca = &m->classes[a], *cb = &m->classes[b];
    uint32_t k, x, y;
    if (ca->name_chars != cb->name_chars || ca->name_chars > TL4_MODEL_MAX_CLASS_QUERY) return 0;
    for (k = 0; k < ca->name_chars; k++)
        if (!rd16(cmap, m->cmap_size, (uint64_t)ca->name_off + 2u * k, &x) ||
            !rd16(cmap, m->cmap_size, (uint64_t)cb->name_off + 2u * k, &y) || x != y) return 0;
    return 1;
}

/* ---- heapsort (iterative, in place; every key lives in the model, R-GEN-6/7) ----------------------------------- */

static int prop_less(const tl4_prop *a, const tl4_prop *b)
{
    if (a->area != b->area) return a->area < b->area;
    if (a->class_ordinal != b->class_ordinal) return a->class_ordinal < b->class_ordinal;
    if (a->part != b->part) return a->part < b->part;
    if (a->state != b->state) return a->state < b->state;
    if (a->name_id != b->name_id) return a->name_id < b->name_id;
    return a->seq < b->seq;
}

static void prop_sift(tl4_prop *v, uint32_t root, uint32_t n)
{
    for (;;)
    {
        uint32_t child = 2u * root + 1u, best = root;
        tl4_prop t;
        if (child >= n) return;
        if (prop_less(&v[best], &v[child])) best = child;
        if (child + 1 < n && prop_less(&v[best], &v[child + 1])) best = child + 1;
        if (best == root) return;
        t = v[root]; v[root] = v[best]; v[best] = t;
        root = best;
    }
}

static void prop_heapsort(tl4_prop *v, uint32_t n)
{
    uint32_t i;
    if (n < 2) return;
    for (i = n / 2; i-- > 0;) prop_sift(v, i, n);
    for (i = n - 1; i > 0; i--)
    {
        tl4_prop t = v[0]; v[0] = v[i]; v[i] = t;
        prop_sift(v, 0, i);
    }
}

static void name_sift(const tl4_model *m, const uint8_t *cmap, uint32_t *v, uint32_t root, uint32_t n)
{
    for (;;)
    {
        uint32_t child = 2u * root + 1u, best = root, t;
        if (child >= n) return;
        if (name_less(m, cmap, v[best], v[child])) best = child;
        if (child + 1 < n && name_less(m, cmap, v[best], v[child + 1])) best = child + 1;
        if (best == root) return;
        t = v[root]; v[root] = v[best]; v[best] = t;
        root = best;
    }
}

static void name_heapsort(const tl4_model *m, const uint8_t *cmap, uint32_t *v, uint32_t n)
{
    uint32_t i;
    if (n < 2) return;
    for (i = n / 2; i-- > 0;) name_sift(m, cmap, v, i, n);
    for (i = n - 1; i > 0; i--)
    {
        uint32_t t = v[0]; v[0] = v[i]; v[i] = t;
        name_sift(m, cmap, v, 0, i);
    }
}

/* ---- build ---------------------------------------------------------------------------------------------------- */

static tl4_status find_named(const tl4_pe *pe, const char *type, const char *name, tl4_resource *out)
{
    tl4_res_name t = tl4_res_ascii(type), n = tl4_res_ascii(name);
    tl4_status st = tl4_pe_find(pe, &t, &n, out);
    return st == TL4_E_NOT_FOUND ? TL4_E_V4_MISSING : st;
}

/* A string-table string, in the style first, then in the strings mapping (R-RES-1); stored as a range of its mapping. */
static void resolve_string(const tl4_model *m, uint32_t id, tl4_prop *p)
{
    const uint8_t *s;
    uint32_t chars;
    if (tl4_v4_find_string(&m->style, id, &s, &chars) == TL4_OK)
    {
        p->ref_where = TL4_REF_STYLE; p->ref_off = (uint32_t)(s - m->style.base); p->ref_size = 2u * chars;
    }
    else if (m->has_strings && tl4_v4_find_string(&m->strings, id, &s, &chars) == TL4_OK)
    {
        p->ref_where = TL4_REF_STRINGS; p->ref_off = (uint32_t)(s - m->strings.base); p->ref_size = 2u * chars;
    }
}

static void resolve_resource(const tl4_model *m, const char *type, uint32_t id, tl4_prop *p)
{
    tl4_res_name t = tl4_res_ascii(type), n = tl4_res_id(id);
    tl4_resource res;
    if (id > 0xFFFFu) return;
    if (tl4_pe_find(&m->style, &t, &n, &res) != TL4_OK) return;
    p->ref_where = TL4_REF_STYLE; p->ref_off = res.offset; p->ref_size = res.size;
}

struct collect_ctx {
    tl4_model *m;
    uint32_t cap;
    uint32_t area;
    const uint8_t *area_data;
    int overflow;
};

static int collect_cb(const tl4_v4_record *r, void *context)
{
    struct collect_ctx *c = (struct collect_ctx *)context;
    tl4_model *m = c->m;
    tl4_prop *p;
    if (m->nprops >= c->cap) { c->overflow = 1; return 1; }      /* the bytes changed since the sizing pass (R-GEN-11) */
    p = &m->props[m->nprops];
    memset(p, 0, sizeof(*p));
    p->class_ordinal = r->class_id;
    p->part = (uint16_t)r->part_id;
    p->state = (uint16_t)r->state_id;
    p->name_id = (uint16_t)r->name_id;
    p->area = (uint8_t)c->area;
    p->type = (uint8_t)r->type_id;
    p->origin = (uint8_t)(r->state_id != 0 ? TL4_PO_STATE : r->part_id != 0 ? TL4_PO_PART :
                          r->class_id == m->counts.globals ? TL4_PO_GLOBAL : TL4_PO_CLASS);
    p->seq = m->nprops;
    p->reference = r->reference;
    if (r->reference == 0)
    {
        p->data_off = r->data ? (uint32_t)(r->data - c->area_data) : 0;
        p->cb = r->cb_data;
    }
    else
    {
        /* resolved once, here; a getter never searches (R-MOD-6) */
        switch (r->type_id)
        {
            case TL4_V4_FONT: case TL4_V4_STRING: resolve_string(m, r->reference, p); break;
            case TL4_V4_FILENAME: resolve_resource(m, "IMAGE", r->reference, p); break;
            case TL4_V4_DISKSTREAM: resolve_resource(m, "STREAM", r->reference, p); break;
            default: break;
        }
        if (p->ref_where == TL4_REF_UNRESOLVED) m->counts.unresolved++;
    }
    m->nprops++;
    return 0;
}

static tl4_status collect_area(tl4_model *m, uint32_t area, const char *type, const char *name, uint32_t max_bytes,
                               uint32_t cap)
{
    tl4_resource res;
    tl4_v4_walk_stats ws;
    struct collect_ctx ctx;
    tl4_status st;
    if ((st = find_named(&m->style, type, name, &res)) != TL4_OK) return st;
    if (!in_range(m->style.size, res.offset, res.size)) return TL4_E_TRUNCATED;
    m->area_off[area] = res.offset;
    m->area_size[area] = res.size;
    ctx.m = m; ctx.cap = cap; ctx.area = area; ctx.area_data = res.data; ctx.overflow = 0;
    st = tl4_v4_walk_records(res.data, res.size, m->nclasses, max_bytes, TL4_V4_MAX_RECORDS, collect_cb, &ctx, &ws);
    if (ctx.overflow) return TL4_E_MODEL_SIZE;
    return st;
}

static int same_key(const tl4_prop *a, const tl4_prop *b)
{
    return a->area == b->area && a->class_ordinal == b->class_ordinal && a->part == b->part && a->state == b->state &&
           a->name_id == b->name_id;
}

tl4_status tl4_model_build(const tl4_pe *style, const tl4_pe *strings, void *buffer, size_t buffer_size, tl4_model *out)
{
    tl4_model m;
    tl4_v4_stats s;
    tl4_resource res;
    tl4_v4_cmap cmap;
    tl4_v4_bcmap bcmap;
    size_t need;
    uint8_t *cur;
    uint32_t i, off, n, ps_cap, prop_cap, w, c;
    const uint8_t *cm;
    tl4_status st;

    if (!out) return TL4_E_ARG;
    clear_model(out);
    if (!style || !style->base || !buffer) return TL4_E_ARG;
    if (strings && !strings->base) return TL4_E_ARG;
    if (((uintptr_t)buffer & 7u) != 0) return TL4_E_ARG;

    /* one validator (R-GEN-10); its statistics size the arrays and nothing else (R-MOD-5) */
    if ((st = tl4_v4_validate(style, strings, &s)) != TL4_OK) return st;
    need = tl4_model_size(&s);
    if (need == 0 || need > buffer_size) return TL4_E_MODEL_SIZE;
#ifdef TL4_TESTING
    if (tl4_model_test_between_passes) tl4_model_test_between_passes(style->base, style->size);
#endif

    clear_model(&m);
    m.style = *style;
    if (strings) { m.strings = *strings; m.has_strings = 1; }
    m.stats = s;
    m.nclasses = s.cmap_names;
    ps_cap = ps_capacity(&s);
    prop_cap = prop_capacity(&s);
    cur = (uint8_t *)buffer;
    memset(cur, 0, need);
    m.classes = (tl4_class *)(void *)cur;         cur += align8((uint64_t)sizeof(tl4_class) * m.nclasses);
    m.partstates = (tl4_partstate *)(void *)cur;  cur += align8((uint64_t)sizeof(tl4_partstate) * ps_cap);
    m.props = (tl4_prop *)(void *)cur;            cur += align8((uint64_t)sizeof(tl4_prop) * prop_cap);
    m.name_order = (uint32_t *)(void *)cur;

    /* the class map again (R-MOD-5), then every name once (R-MOD-1) */
    if ((st = find_named(&m.style, "CMAP", "CMAP", &res)) != TL4_OK) return st;
    if ((st = tl4_v4_check_cmap(res.data, res.size, &cmap)) != TL4_OK) return st;
    if (cmap.count != m.nclasses) return TL4_E_MODEL_SIZE;                  /* changed since the validation */
    if (!in_range(m.style.size, res.offset, res.size)) return TL4_E_TRUNCATED;
    m.cmap_off = res.offset;
    m.cmap_size = res.size;
    cm = res.data;
    for (off = 0, n = 0; off < res.size && n < m.nclasses;)
    {
        uint32_t len = 0, ch;
        if (!rd16(cm, res.size, off, &ch)) return TL4_E_V4_CMAP;
        if (ch == 0) { off += 2; continue; }
        while (len <= TL4_MODEL_MAX_CLASS_QUERY && rd16(cm, res.size, (uint64_t)off + 2u * len, &ch) && ch != 0) len++;
        if (len == 0 || len > TL4_MODEL_MAX_CLASS_QUERY) return TL4_E_V4_CMAP;
        m.classes[n].name_off = off;
        m.classes[n].name_chars = len;
        m.classes[n].base = TL4_NO_BASE;
        n++;
        off += 2u * (len + 1u);
    }
    if (n != m.nclasses) return TL4_E_V4_CMAP;

    /* the base map again; every base stored, so no query reads the map (R-MOD-6) */
    if ((st = find_named(&m.style, "BCMAP", "BCMAP", &res)) != TL4_OK) return st;
    if ((st = tl4_v4_check_bcmap(res.data, res.size, m.nclasses, &bcmap)) != TL4_OK) return st;
    m.counts.descriptors = bcmap.shift;
    for (i = 0; i < m.nclasses; i++)
    {
        if (tl4_v4_bcmap_base(&bcmap, i, &m.classes[i].base) != TL4_OK) return TL4_E_V4_BCMAP;
        if (m.classes[i].base != TL4_NO_BASE && m.classes[i].base >= m.nclasses) return TL4_E_V4_BCMAP;
    }
    /* the chains are checked on the stored bases, which is all a query ever reads: every chain ends within
     * TL4_V4_MAX_CHAIN steps (R-V4-10), else the build fails */
    for (i = 0; i < m.nclasses; i++)
    {
        uint32_t at = i, steps = 0;
        while (m.classes[at].base != TL4_NO_BASE)
        {
            at = m.classes[at].base;
            if (++steps > TL4_V4_MAX_CHAIN) return TL4_E_V4_BCMAP;
        }
    }

    /* name order (equal names by ordinal), the repeats, and the two classes the engine treats specially */
    for (i = 0; i < m.nclasses; i++) m.name_order[i] = i;
    name_heapsort(&m, cm, m.name_order, m.nclasses);
    for (i = 1; i < m.nclasses; i++)
        if (names_equal(&m, cm, m.name_order[i - 1], m.name_order[i])) m.counts.duplicate_names++;
    {
        uint32_t ord;
        if (tl4_model_class_ascii(&m, "globals", &ord) == TL4_OK) m.counts.globals = ord;
        if (tl4_model_class_ascii(&m, "sysmetrics", &ord) == TL4_OK) m.counts.sysmetrics = ord;
    }

    /* records in file order: RMAP first, then VARIANT; the capacity is what the sizing pass fixed (R-GEN-11) */
    if ((st = collect_area(&m, TL4_AREA_RMAP, "RMAP", "RMAP", TL4_V4_MAX_RMAP_BYTES, prop_cap)) != TL4_OK) return st;
    if ((st = collect_area(&m, TL4_AREA_VARIANT, "VARIANT", "NORMAL", TL4_V4_MAX_VARIANT_BYTES, prop_cap)) != TL4_OK) return st;

    /* sort, then the first of each (area, class, part, state, name) wins (R-V4-9) */
    prop_heapsort(m.props, m.nprops);
    for (i = 0, w = 0; i < m.nprops; i++)
    {
        if (w > 0 && same_key(&m.props[w - 1], &m.props[i])) { m.counts.duplicate_props++; continue; }
        if (w != i) m.props[w] = m.props[i];
        w++;
    }
    m.nprops = w;

    /* runs of (area, class, part, state) become part-states: the VARIANT ones first (area 0), then the RMAP ones */
    for (i = 0; i < m.nprops; i++)
    {
        const tl4_prop *p = &m.props[i];
        tl4_partstate *ps = m.npartstates ? &m.partstates[m.npartstates - 1] : 0;
        if (ps && ps->area == p->area && ps->class_ordinal == p->class_ordinal && ps->part == p->part && ps->state == p->state)
        {
            ps->nprops++;
            continue;
        }
        if (m.npartstates >= ps_cap) return TL4_E_MODEL_SIZE;
        ps = &m.partstates[m.npartstates++];
        ps->class_ordinal = p->class_ordinal; ps->part = p->part; ps->state = p->state; ps->area = p->area;
        ps->first_prop = i; ps->nprops = 1;
    }
    for (i = 0, c = 0; c < m.nclasses; c++)
    {
        m.classes[c].first_ps = i;
        m.classes[c].nps = 0;
        while (i < m.npartstates && m.partstates[i].area == TL4_AREA_VARIANT && m.partstates[i].class_ordinal == c)
        {
            m.classes[c].nps++;
            i++;
        }
    }
    m.rmap_first_ps = i;
    m.rmap_nps = m.npartstates - i;
    for (; i < m.npartstates; i++)
        if (m.partstates[i].area != TL4_AREA_RMAP) return TL4_E_V4_RECORD;   /* a VARIANT class ordinal outside the map */

    m.counts.classes = m.nclasses;
    m.counts.partstates = m.npartstates;
    m.counts.props = m.nprops;
    *out = m;
    return TL4_OK;
}

/* ---- lookups --------------------------------------------------------------------------------------------------- */

tl4_status tl4_model_class(const tl4_model *m, const uint8_t *utf16, uint32_t chars, uint32_t *ordinal)
{
    const uint8_t *cm;
    uint32_t lo, hi;
    if (!ordinal) return TL4_E_ARG;
    *ordinal = TL4_NO_BASE;
    if (!m || !m->classes || !utf16 || chars == 0 || chars > TL4_MODEL_MAX_CLASS_QUERY) return TL4_E_ARG;
    if (!(cm = cmap_bytes(m))) return TL4_E_TRUNCATED;
    lo = 0; hi = m->nclasses;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2u;
        int c = name_cmp_query(m, cm, m->name_order[mid], utf16, chars);
        if (c == 2) return TL4_E_TRUNCATED;
        if (c == 0)
        {
            /* equal names sit together in ascending ordinal order: step back to the lowest (R-V4-2) */
            while (mid > 0 && name_cmp_query(m, cm, m->name_order[mid - 1], utf16, chars) == 0) mid--;
            *ordinal = m->name_order[mid];
            return TL4_OK;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return TL4_E_NOT_FOUND;
}

tl4_status tl4_model_class_ascii(const tl4_model *m, const char *name, uint32_t *ordinal)
{
    uint8_t q[2u * TL4_MODEL_MAX_CLASS_QUERY];
    uint32_t n = 0;
    if (ordinal) *ordinal = TL4_NO_BASE;
    if (!name || !ordinal) return TL4_E_ARG;
    while (name[n] && n < TL4_MODEL_MAX_CLASS_QUERY) { q[2 * n] = (uint8_t)name[n]; q[2 * n + 1] = 0; n++; }
    if (name[n]) return TL4_E_ARG;                                        /* longer than any class name */
    return tl4_model_class(m, q, n, ordinal);
}

tl4_status tl4_model_class_name(const tl4_model *m, uint32_t ordinal, const uint8_t **utf16, uint32_t *chars)
{
    const uint8_t *cm;
    const tl4_class *c;
    if (!utf16 || !chars) return TL4_E_ARG;
    *utf16 = 0; *chars = 0;
    if (!m || !m->classes) return TL4_E_ARG;
    if (ordinal >= m->nclasses) return TL4_E_NOT_FOUND;
    if (!(cm = cmap_bytes(m))) return TL4_E_TRUNCATED;
    c = &m->classes[ordinal];
    if (!in_range(m->cmap_size, c->name_off, 2u * (uint64_t)c->name_chars)) return TL4_E_TRUNCATED;
    *utf16 = cm + c->name_off;
    *chars = c->name_chars;
    return TL4_OK;
}

static uint32_t ps_key(uint32_t part, uint32_t state) { return (part << 16) | state; }

/* Binary search of partstates[lo, hi) for (part, state); the range is one class's VARIANT run or the RMAP run. */
static const tl4_partstate *search_partstates(const tl4_model *m, uint32_t lo, uint32_t hi, uint32_t class_ordinal,
                                              uint32_t part, uint32_t state)
{
    uint64_t want = ((uint64_t)class_ordinal << 32) | ps_key(part, state);
    if (hi > m->npartstates) return 0;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2u;
        const tl4_partstate *ps = &m->partstates[mid];
        uint64_t key = ((uint64_t)ps->class_ordinal << 32) | ps_key(ps->part, ps->state);
        if (key == want) return ps;
        if (key < want) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static const tl4_partstate *find_partstate(const tl4_model *m, uint32_t area, uint32_t class_ordinal, uint32_t part, uint32_t state)
{
    if (area == TL4_AREA_RMAP) return search_partstates(m, m->rmap_first_ps, m->rmap_first_ps + m->rmap_nps, class_ordinal, part, state);
    if (class_ordinal >= m->nclasses) return 0;
    return search_partstates(m, m->classes[class_ordinal].first_ps, m->classes[class_ordinal].first_ps + m->classes[class_ordinal].nps,
                             class_ordinal, part, state);
}

static const tl4_prop *find_prop(const tl4_model *m, const tl4_partstate *ps, uint32_t name_id)
{
    uint32_t lo = ps->first_prop, hi = ps->first_prop + ps->nprops;
    if (hi > m->nprops) return 0;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2u;
        const tl4_prop *p = &m->props[mid];
        if (p->name_id == name_id) return p;
        if (p->name_id < name_id) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* One (part, state) along the class, its stored base chain (at most TL4_V4_MAX_CHAIN steps) and `globals` once; the
 * walk stops at `globals` (its own bases are not followed), so it visits at most 10 classes, and it counts its
 * steps and stops at the bound whatever the model holds (R-MOD-6). */
static const tl4_prop *find_along_chain(const tl4_model *m, uint32_t class_ordinal, uint32_t part, uint32_t state,
                                        uint32_t name_id, int *part_defined_only)
{
    uint32_t cur = class_ordinal, steps = 0;
    int visited_globals = (class_ordinal == m->counts.globals);
    while (steps <= TL4_V4_MAX_CHAIN + 1u)
    {
        const tl4_partstate *ps = cur < m->nclasses ? find_partstate(m, TL4_AREA_VARIANT, cur, part, state) : 0;
        steps++;
        if (ps)
        {
            if (part_defined_only) { *part_defined_only = 1; return 0; }
            {
                const tl4_prop *p = find_prop(m, ps, name_id);
                if (p) return p;
            }
        }
        if (cur >= m->nclasses) return 0;
        if (cur == m->counts.globals) return 0;                               /* the chain ends at globals */
        if (m->classes[cur].base != TL4_NO_BASE && steps <= TL4_V4_MAX_CHAIN)
        {
            cur = m->classes[cur].base;
            if (cur == m->counts.globals) visited_globals = 1;
            continue;
        }
        if (!visited_globals && m->counts.globals != TL4_NO_BASE)
        {
            cur = m->counts.globals;
            visited_globals = 1;
            continue;
        }
        return 0;
    }
    return 0;
}

tl4_status tl4_model_find(const tl4_model *m, uint32_t class_ordinal, uint32_t part, uint32_t state, uint32_t name_id,
                          const tl4_prop **out)
{
    const tl4_prop *p;
    if (!out) return TL4_E_ARG;
    *out = 0;
    if (!m || !m->classes || part > 255 || state > 255 || name_id == 0 || name_id > 0xFFFFu) return TL4_E_ARG;
    if (class_ordinal >= m->nclasses) return TL4_E_NOT_FOUND;
    /* the engine's order (MSSTYLES_FindProperty): exact (part, state), then (part, 0), then (0, 0), each along the chain */
    if ((p = find_along_chain(m, class_ordinal, part, state, name_id, 0)) != 0) { *out = p; return TL4_OK; }
    if (state != 0 && (p = find_along_chain(m, class_ordinal, part, 0, name_id, 0)) != 0) { *out = p; return TL4_OK; }
    if (part != 0 && (p = find_along_chain(m, class_ordinal, 0, 0, name_id, 0)) != 0) { *out = p; return TL4_OK; }
    return TL4_E_NOT_FOUND;
}

tl4_status tl4_model_find_rmap(const tl4_model *m, uint32_t class_ordinal, uint32_t part, uint32_t state, uint32_t name_id,
                               const tl4_prop **out)
{
    const tl4_partstate *ps;
    const tl4_prop *p;
    if (!out) return TL4_E_ARG;
    *out = 0;
    if (!m || !m->classes || part > 255 || state > 255 || name_id == 0 || name_id > 0xFFFFu) return TL4_E_ARG;
    if (class_ordinal >= m->nclasses) return TL4_E_NOT_FOUND;
    ps = find_partstate(m, TL4_AREA_RMAP, class_ordinal, part, state);
    if (!ps || !(p = find_prop(m, ps, name_id))) return TL4_E_NOT_FOUND;
    *out = p;
    return TL4_OK;
}

int tl4_model_part_defined(const tl4_model *m, uint32_t class_ordinal, uint32_t part)
{
    int defined = 0;
    if (!m || !m->classes || part > 255 || class_ordinal >= m->nclasses) return 0;
    (void)find_along_chain(m, class_ordinal, part, 0, 1, &defined);
    return defined;
}

/* ---- getters (R-MOD-2, R-MOD-4) -------------------------------------------------------------------------------- */

/* The property must be one of the model's own: a pointer into props[0 .. nprops), aligned on an element. */
static int prop_in_model(const tl4_model *m, const tl4_prop *p)
{
    uintptr_t base, q;
    if (!m || !m->props || !p) return 0;
    base = (uintptr_t)(const void *)m->props; q = (uintptr_t)(const void *)p;
    if (q < base) return 0;
    if ((q - base) % sizeof(tl4_prop) != 0) return 0;
    return (q - base) / sizeof(tl4_prop) < m->nprops;
}

/* Copy the inline bytes of a property into dst: exactly `cb` bytes, which must equal `want` when want != 0 and fit
 * `cap`. Range-checked against the area and the mapping (R-GEN-11), one copy; every further check runs on dst. */
static tl4_status copy_inline(const tl4_model *m, const tl4_prop *p, uint32_t want, uint8_t *dst, uint32_t cap)
{
    uint32_t aoff, asize;
    if (!m || !m->style.base || !p || !dst) return TL4_E_ARG;
    if (!prop_in_model(m, p)) return TL4_E_ARG;
    if (p->area > TL4_AREA_RMAP) return TL4_E_ARG;
    if (p->reference != 0) return TL4_E_TYPE;                               /* a reference record has no inline value */
    if ((want != 0 && p->cb != want) || p->cb > cap) return TL4_E_TRUNCATED; /* not the size the validator accepted */
    aoff = m->area_off[p->area]; asize = m->area_size[p->area];
    if (!in_range(m->style.size, aoff, asize)) return TL4_E_TRUNCATED;
    if (!in_range(asize, p->data_off, p->cb)) return TL4_E_TRUNCATED;
    memcpy(dst, m->style.base + aoff + p->data_off, p->cb);
    return TL4_OK;
}

static tl4_status get_words(const tl4_model *m, const tl4_prop *p, uint32_t type_a, uint32_t type_b, uint32_t n, int32_t *out)
{
    uint8_t copy[16];
    uint32_t i;
    tl4_status st;
    if (!p || !out) return TL4_E_ARG;
    if (!prop_in_model(m, p)) return TL4_E_ARG;
    if (p->type != type_a && p->type != type_b) return TL4_E_TYPE;
    if ((st = copy_inline(m, p, 4u * n, copy, sizeof(copy))) != TL4_OK) return st;
    for (i = 0; i < n; i++) out[i] = (int32_t)word(copy, 4u * i);
    return TL4_OK;
}

tl4_status tl4_get_int(const tl4_model *m, const tl4_prop *p, int32_t *value) { return get_words(m, p, TL4_V4_INT, TL4_V4_SIZE, 1, value); }
tl4_status tl4_get_enum(const tl4_model *m, const tl4_prop *p, int32_t *value) { return get_words(m, p, TL4_V4_ENUM, TL4_V4_ENUM, 1, value); }

tl4_status tl4_get_bool(const tl4_model *m, const tl4_prop *p, int32_t *value)
{
    tl4_status st = get_words(m, p, TL4_V4_BOOL, TL4_V4_BOOL, 1, value);
    if (st == TL4_OK) *value = *value != 0;
    return st;
}

tl4_status tl4_get_color(const tl4_model *m, const tl4_prop *p, uint32_t *colorref)
{
    int32_t v;
    tl4_status st;
    if (!colorref) return TL4_E_ARG;
    st = get_words(m, p, TL4_V4_COLOR, TL4_V4_COLOR, 1, &v);
    if (st == TL4_OK) *colorref = (uint32_t)v;
    return st;
}

tl4_status tl4_get_position(const tl4_model *m, const tl4_prop *p, int32_t xy[2]) { return get_words(m, p, TL4_V4_POSITION, TL4_V4_POSITION, 2, xy); }
tl4_status tl4_get_rect(const tl4_model *m, const tl4_prop *p, int32_t ltrb[4]) { return get_words(m, p, TL4_V4_RECT, TL4_V4_RECT, 4, ltrb); }
tl4_status tl4_get_margins(const tl4_model *m, const tl4_prop *p, int32_t lrtb[4]) { return get_words(m, p, TL4_V4_MARGINS, TL4_V4_MARGINS, 4, lrtb); }

tl4_status tl4_get_intlist(const tl4_model *m, const tl4_prop *p, int32_t *values, uint32_t cap, uint32_t *count)
{
    uint8_t copy[4u + 4u * TL4_V4_MAX_INTLIST];
    uint32_t n, i;
    tl4_status st;
    if (!count) return TL4_E_ARG;
    *count = 0;
    if (!p || (!values && cap)) return TL4_E_ARG;
    if (!prop_in_model(m, p)) return TL4_E_ARG;
    if (p->type != TL4_V4_INTLIST) return TL4_E_TYPE;
    /* the count comes from the model's size, never from the input (R-MOD-4) */
    if (p->cb < 4 || (p->cb & 3u) || p->cb > sizeof(copy)) return TL4_E_TRUNCATED;
    n = (p->cb - 4u) / 4u;
    if (n > cap) return TL4_E_BUFFER;
    if ((st = copy_inline(m, p, p->cb, copy, sizeof(copy))) != TL4_OK) return st;
    for (i = 0; i < n; i++) values[i] = (int32_t)word(copy, 4u + 4u * i);
    *count = n;
    return TL4_OK;
}

/* Copy a resolved reference (a string) into dst: at most cap bytes, range-checked against its mapping. */
static tl4_status copy_reference(const tl4_model *m, const tl4_prop *p, uint8_t *dst, uint32_t cap, uint32_t *size)
{
    const tl4_pe *pe;
    *size = 0;
    if (!prop_in_model(m, p)) return TL4_E_ARG;
    if (p->ref_where == TL4_REF_STYLE) pe = &m->style;
    else if (p->ref_where == TL4_REF_STRINGS && m->has_strings) pe = &m->strings;
    else return TL4_E_V4_REFERENCE;
    if (!pe->base) return TL4_E_ARG;
    if (p->ref_size > cap) return TL4_E_BUFFER;
    if (!in_range(pe->size, p->ref_off, p->ref_size)) return TL4_E_TRUNCATED;
    memcpy(dst, pe->base + p->ref_off, p->ref_size);
    *size = p->ref_size;
    return TL4_OK;
}

tl4_status tl4_get_string(const tl4_model *m, const tl4_prop *p, uint16_t *out, uint32_t cap_chars, uint32_t *chars)
{
    uint8_t *dst;
    uint32_t bytes, i, n;
    tl4_status st;
    if (!chars) return TL4_E_ARG;
    *chars = 0;
    if (!m || !p || !out || cap_chars == 0) return TL4_E_ARG;
    if (!prop_in_model(m, p)) return TL4_E_ARG;
    if (p->type != TL4_V4_STRING) return TL4_E_TYPE;
    dst = (uint8_t *)out;
    if (p->reference != 0)
    {
        /* a string-table string: at most 255 characters (R-RES-2), not terminated in the table */
        if (p->ref_size > 2u * TL4_V4_MAX_FONT_CHARS || (p->ref_size & 1u)) return TL4_E_TRUNCATED;
        if (p->ref_size / 2u + 1u > cap_chars) return TL4_E_BUFFER;
        if ((st = copy_reference(m, p, dst, 2u * (cap_chars - 1u), &bytes)) != TL4_OK) return st;
        n = bytes / 2u;
    }
    else
    {
        if (p->cb < 2 || (p->cb & 1u) || p->cb > TL4_V4_MAX_STRING_BYTES) return TL4_E_TRUNCATED;
        if (p->cb / 2u > cap_chars) return TL4_E_BUFFER;
        if ((st = copy_inline(m, p, p->cb, dst, 2u * cap_chars)) != TL4_OK) return st;
        n = p->cb / 2u - 1u;
        /* the terminator is checked on the copy (R-MOD-4) */
        if (dst[2 * n] != 0 || dst[2 * n + 1] != 0) return TL4_E_TRUNCATED;
    }
    /* the copy's bytes are little-endian code units: rebuild them in place as uint16_t for every host */
    for (i = 0; i < n; i++) out[i] = (uint16_t)((uint32_t)dst[2 * i] | ((uint32_t)dst[2 * i + 1] << 8));
    out[n] = 0;
    *chars = n;
    return TL4_OK;
}

/* ---- font strings ---------------------------------------------------------------------------------------------- */

static uint32_t cu(const uint8_t *s, uint32_t i) { return (uint32_t)s[2 * i] | ((uint32_t)s[2 * i + 1] << 8); }

static int is_space(uint32_t c) { return c == ' ' || c == '\t'; }

/* Case-insensitive ASCII match of the token [from, to) against `word`. */
static int token_is(const uint8_t *s, uint32_t from, uint32_t to, const char *word)
{
    uint32_t i, n = 0;
    while (word[n]) n++;
    if (to - from != n) return 0;
    for (i = 0; i < n; i++)
    {
        uint32_t c = cu(s, from + i), w = (uint32_t)(unsigned char)word[i];
        if (c >= 'a' && c <= 'z') c -= 32u;
        if (w >= 'a' && w <= 'z') w -= 32u;
        if (c != w) return 0;
    }
    return 1;
}

tl4_status tl4_parse_font(const uint8_t *utf16, uint32_t chars, tl4_font *font)
{
    uint32_t pos = 0, token = 0;
    if (!font) return TL4_E_ARG;
    memset(font, 0, sizeof(*font));
    if (!utf16 || chars == 0 || chars > TL4_V4_MAX_FONT_CHARS) return TL4_E_ARG;
    while (pos <= chars)
    {
        uint32_t from = pos, to;
        while (pos < chars && cu(utf16, pos) != ',') pos++;
        to = pos;
        pos++;                                                             /* past the comma, or past the end */
        while (from < to && is_space(cu(utf16, from))) from++;
        while (to > from && is_space(cu(utf16, to - 1))) to--;
        if (token == 0)
        {
            uint32_t i;
            if (from == to || to - from > 31) return TL4_E_FONT_SYNTAX;    /* face 1..31 characters (R-RES-2) */
            for (i = from; i < to; i++) font->face[i - from] = (uint16_t)cu(utf16, i);
            font->face_chars = to - from;
        }
        else if (token == 1)
        {
            uint32_t i, v = 0;
            if (from == to || to - from > 3) return TL4_E_FONT_SYNTAX;     /* at most three digits: no overflow */
            for (i = from; i < to; i++)
            {
                uint32_t c = cu(utf16, i);
                if (c < '0' || c > '9') return TL4_E_FONT_SYNTAX;
                v = v * 10u + (c - '0');
                if (v > 255) return TL4_E_FONT_SYNTAX;
            }
            if (v < 1) return TL4_E_FONT_SYNTAX;
            font->points = (int32_t)v;
        }
        else
        {
            if (from == to) return TL4_E_FONT_SYNTAX;
            if (token_is(utf16, from, to, "Bold")) font->bold = 1;
            else if (token_is(utf16, from, to, "Italic")) font->italic = 1;
            else if (token_is(utf16, from, to, "Underline")) font->underline = 1;
            else if (token_is(utf16, from, to, "Strikeout")) font->strikeout = 1;
            else if (to - from > 8 && token_is(utf16, from, from + 8, "Quality:"))
            {
                uint32_t q = from + 8;
                while (q < to && is_space(cu(utf16, q))) q++;
                if (token_is(utf16, q, to, "Default")) font->quality = 0;
                else if (token_is(utf16, q, to, "Draft")) font->quality = 1;
                else if (token_is(utf16, q, to, "Proof")) font->quality = 2;
                else if (token_is(utf16, q, to, "NonAntialiased")) font->quality = 3;
                else if (token_is(utf16, q, to, "Antialiased")) font->quality = 4;
                else if (token_is(utf16, q, to, "ClearType")) font->quality = 5;
                else if (token_is(utf16, q, to, "ClearTypeNatural")) font->quality = 6;
                else return TL4_E_FONT_SYNTAX;
            }
            else return TL4_E_FONT_SYNTAX;
        }
        token++;
        if (token > 8) return TL4_E_FONT_SYNTAX;
    }
    if (token < 2) return TL4_E_FONT_SYNTAX;                               /* face and size are required */
    return TL4_OK;
}

tl4_status tl4_get_font(const tl4_model *m, const tl4_prop *p, tl4_font *font)
{
    uint8_t copy[2u * TL4_V4_MAX_FONT_CHARS];
    uint32_t bytes;
    tl4_status st;
    if (!font) return TL4_E_ARG;
    memset(font, 0, sizeof(*font));
    if (!m || !p) return TL4_E_ARG;
    if (!prop_in_model(m, p)) return TL4_E_ARG;
    if (p->type != TL4_V4_FONT) return TL4_E_TYPE;
    if (p->reference == 0) return TL4_E_TYPE;                              /* FONT always carries a reference (R-V4-7) */
    if (p->ref_size > sizeof(copy) || (p->ref_size & 1u)) return TL4_E_TRUNCATED;
    /* one copy of at most 255 code units; the copy is tokenized (R-MOD-4) */
    if ((st = copy_reference(m, p, copy, sizeof(copy), &bytes)) != TL4_OK) return st;
    return tl4_parse_font(copy, bytes / 2u, font);
}

static tl4_status get_resource_ref(const tl4_model *m, const tl4_prop *p, uint32_t type_id, uint32_t *id, uint32_t *offset, uint32_t *size)
{
    if (!id || !offset || !size) return TL4_E_ARG;
    *id = 0; *offset = 0; *size = 0;
    if (!m || !m->style.base || !p) return TL4_E_ARG;
    if (!prop_in_model(m, p)) return TL4_E_ARG;
    if (p->type != type_id) return TL4_E_TYPE;
    if (p->reference == 0) return TL4_E_TYPE;
    if (p->ref_where != TL4_REF_STYLE) return TL4_E_V4_REFERENCE;         /* resolved at build, or not at all */
    if (!in_range(m->style.size, p->ref_off, p->ref_size)) return TL4_E_TRUNCATED;
    *id = p->reference; *offset = p->ref_off; *size = p->ref_size;
    return TL4_OK;
}

tl4_status tl4_get_filename(const tl4_model *m, const tl4_prop *p, uint32_t *id, uint32_t *offset, uint32_t *size)
{
    return get_resource_ref(m, p, TL4_V4_FILENAME, id, offset, size);
}

tl4_status tl4_get_diskstream(const tl4_model *m, const tl4_prop *p, uint32_t *id, uint32_t *offset, uint32_t *size)
{
    return get_resource_ref(m, p, TL4_V4_DISKSTREAM, id, offset, size);
}

tl4_status tl4_get_metric(const tl4_model *m, const tl4_prop *p, int32_t *value)
{
    uint8_t copy[4u + 4u * TL4_V4_MAX_INTLIST];
    tl4_status st;
    if (!p || !value) return TL4_E_ARG;
    if (!prop_in_model(m, p)) return TL4_E_ARG;
    switch (p->type)
    {
        /* the engine's list (property.c GetThemeMetric), every one at least 4 bytes; never STRING, FILENAME, FONT */
        case TL4_V4_INT: case TL4_V4_SIZE: case TL4_V4_BOOL: case TL4_V4_ENUM: case TL4_V4_COLOR:
            if ((st = copy_inline(m, p, 4, copy, sizeof(copy))) != TL4_OK) return st;
            *value = p->type == TL4_V4_BOOL ? (word(copy, 0) != 0) : (int32_t)word(copy, 0);
            return TL4_OK;
        case TL4_V4_POSITION:
            if ((st = copy_inline(m, p, 8, copy, sizeof(copy))) != TL4_OK) return st;
            *value = (int32_t)word(copy, 0);                               /* x */
            return TL4_OK;
        case TL4_V4_MARGINS:
            if ((st = copy_inline(m, p, 16, copy, sizeof(copy))) != TL4_OK) return st;
            *value = (int32_t)word(copy, 0);                               /* cxLeftWidth */
            return TL4_OK;
        case TL4_V4_INTLIST:
            if (p->cb < 8 || (p->cb & 3u) || p->cb > sizeof(copy)) return TL4_E_TRUNCATED;   /* at least one value */
            if ((st = copy_inline(m, p, p->cb, copy, sizeof(copy))) != TL4_OK) return st;
            *value = (int32_t)word(copy, 4);                               /* the first value; the count is the model's */
            return TL4_OK;
        default:
            return TL4_E_TYPE;
    }
}

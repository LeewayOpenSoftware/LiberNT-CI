/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Bounded PE / resource reader (REQUIREMENTS section 3)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Every read of input goes through rd16/rd32, which check the range against the validated size (R-GEN-2); every
 * offset arithmetic is done in 64 bits or as "x <= limit - y" with y already bounded (R-GEN-3); every field is
 * read unsigned and range-checked (R-GEN-4). Nothing is allocated (R-GEN-7). The walk is iterative (R-GEN-6).
 */
#include "tl4/loader/pe.h"

#define PE_DOS_MAGIC          0x5A4Du
#define PE_SIGNATURE          0x00004550u
#define PE_OPT_MAGIC_PE32     0x10Bu
#define PE_OPT_MAGIC_PE32PLUS 0x20Bu
#define PE_DIR_RESOURCE       2u
#define PE_SECTION_HEADER_SIZE 40u
#define RSRC_DIR_HEADER_SIZE  16u
#define RSRC_ENTRY_SIZE       8u
#define RSRC_DATA_ENTRY_SIZE  16u
#define RSRC_HIGH_BIT         0x80000000u

const char *tl4_status_name(tl4_status status)
{
    switch (status)
    {
        case TL4_OK: return "ok";
        case TL4_E_ARG: return "argument";
        case TL4_E_TOO_LARGE: return "too-large";
        case TL4_E_TRUNCATED: return "truncated";
        case TL4_E_DOS_HEADER: return "dos-header";
        case TL4_E_PE_SIGNATURE: return "pe-signature";
        case TL4_E_FILE_HEADER: return "file-header";
        case TL4_E_OPTIONAL_HEADER: return "optional-header";
        case TL4_E_SECTION: return "section";
        case TL4_E_NO_RESOURCES: return "no-resources";
        case TL4_E_RSRC_DIRECTORY: return "resource-directory";
        case TL4_E_RSRC_ENTRY: return "resource-entry";
        case TL4_E_RSRC_NAME: return "resource-name";
        case TL4_E_RSRC_ORDER: return "resource-order";
        case TL4_E_RSRC_LANGUAGES: return "resource-languages";
        case TL4_E_RSRC_DATA: return "resource-data";
        case TL4_E_NOT_FOUND: return "not-found";
        case TL4_E_V4_VERSION: return "v4-version";
        case TL4_E_V4_MISSING: return "v4-missing-resource";
        case TL4_E_V4_CMAP: return "v4-cmap";
        case TL4_E_V4_VMAP: return "v4-vmap";
        case TL4_E_V4_BCMAP: return "v4-bcmap";
        case TL4_E_V4_RECORD: return "v4-record";
        case TL4_E_V4_SIZE: return "v4-size";
        case TL4_E_V4_END: return "v4-end";
        case TL4_E_V4_LIMIT: return "v4-limit";
        case TL4_E_V4_REFERENCE: return "v4-reference";
        case TL4_E_MODEL_SIZE: return "model-size";
        case TL4_E_TYPE: return "type";
        case TL4_E_BUFFER: return "buffer";
        case TL4_E_FONT_SYNTAX: return "font-syntax";
        case TL4_E_PNG_SIGNATURE: return "png-signature";
        case TL4_E_PNG_CHUNK: return "png-chunk";
        case TL4_E_PNG_CRC: return "png-crc";
        case TL4_E_PNG_IHDR: return "png-ihdr";
        case TL4_E_STR_BLOCK: return "string-block";
        case TL4_E_LIMIT: return "limit";
    }
    return "unknown";
}

/* ---- bounded reads (R-GEN-2) ------------------------------------------------------------------------------- */

static int in_range(size_t size, uint64_t off, uint64_t len)
{
    return off <= size && len <= (uint64_t)size - off;
}

static int rd16(const uint8_t *base, size_t size, uint64_t off, uint32_t *v)
{
    if (!in_range(size, off, 2)) return 0;
    *v = (uint32_t)base[off] | ((uint32_t)base[off + 1] << 8);
    return 1;
}

static int rd32(const uint8_t *base, size_t size, uint64_t off, uint32_t *v)
{
    if (!in_range(size, off, 4)) return 0;
    *v = (uint32_t)base[off] | ((uint32_t)base[off + 1] << 8) | ((uint32_t)base[off + 2] << 16) |
         ((uint32_t)base[off + 3] << 24);
    return 1;
}

/* ---- names ---------------------------------------------------------------------------------------------------- */

tl4_res_name tl4_res_id(uint32_t id)
{
    tl4_res_name n = {0};
    n.kind = TL4_NAME_ID;
    n.id = id;
    return n;
}

tl4_res_name tl4_res_ascii(const char *ascii)
{
    tl4_res_name n = {0};
    uint32_t len = 0;
    n.kind = TL4_NAME_ASCII;
    n.ascii = ascii;
    if (ascii) while (ascii[len] && len <= TL4_PE_MAX_NAME_CHARS) len++;
    if (len == 0 || len > TL4_PE_MAX_NAME_CHARS)
    {
        /* empty or longer than any valid resource name: a name that never matches */
        n.ascii = 0;
        len = 0;
    }
    n.length = len;
    return n;
}

static uint32_t fold(uint32_t c)
{
    return (c >= 'a' && c <= 'z') ? c - 32u : c;
}

/* A stored name's code unit is taken as is; a caller's query character is folded to upper case. */
static uint32_t name_char(const tl4_res_name *n, uint32_t i)
{
    if (n->kind == TL4_NAME_UTF16)
        return (uint32_t)n->utf16[2 * i] | ((uint32_t)n->utf16[2 * i + 1] << 8);
    return fold((uint32_t)(unsigned char)n->ascii[i]);
}

int tl4_res_name_equal(const tl4_res_name *a, const tl4_res_name *b)
{
    uint32_t i;
    if (!a || !b) return 0;
    if (a->kind == TL4_NAME_ID || b->kind == TL4_NAME_ID)
        return a->kind == TL4_NAME_ID && b->kind == TL4_NAME_ID && a->id == b->id;
    if (a->length != b->length) return 0;
    if ((a->kind == TL4_NAME_UTF16 && !a->utf16) || (a->kind == TL4_NAME_ASCII && !a->ascii)) return 0;
    if ((b->kind == TL4_NAME_UTF16 && !b->utf16) || (b->kind == TL4_NAME_ASCII && !b->ascii)) return 0;
    for (i = 0; i < a->length; i++)
        if (name_char(a, i) != name_char(b, i)) return 0;
    return 1;
}

/* Ordinal comparison of two UTF-16 names by code unit, then by length (R-PE-8, no locale). */
static int utf16_compare(const tl4_res_name *a, const tl4_res_name *b)
{
    uint32_t i, n = a->length < b->length ? a->length : b->length;
    for (i = 0; i < n; i++)
    {
        uint32_t ca = name_char(a, i), cb = name_char(b, i);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a->length == b->length) return 0;
    return a->length < b->length ? -1 : 1;
}

/* ---- headers -------------------------------------------------------------------------------------------------- */

struct walk_counts {
    uint32_t directories;
    uint32_t entries;
    uint32_t resources;
    uint32_t multi_pairs;
};

static tl4_status walk(const tl4_pe *pe, tl4_res_callback callback, void *context, int *stopped,
                       struct walk_counts *counts);

static void clear_handle(tl4_pe *pe)
{
    pe->base = 0;
    pe->size = 0;
    pe->machine = 0;
    pe->nsections = 0;
    pe->is_pe32plus = 0;
    pe->reserved_[0] = pe->reserved_[1] = pe->reserved_[2] = 0;
    pe->section_table_off = pe->rsrc_rva = pe->rsrc_dir_size = 0;
    pe->rsrc_section_va = pe->rsrc_section_raw = pe->rsrc_section_raw_size = 0;
    pe->rsrc_off = pe->rsrc_limit = 0;
    pe->ndirectories = pe->nentries = pe->nresources = pe->nmulti_pairs = 0;
}

static tl4_status open_checked(const uint8_t *base, size_t size, tl4_pe *pe)
{
    uint32_t v, e_lfanew, nsections, opt_size, magic, nrva, fh_off, opt_off, dd_off, sect_off;
    uint32_t rsrc_rva = 0, rsrc_size = 0;
    uint32_t i;
    int found = 0;
    struct walk_counts counts = {0, 0, 0, 0};
    tl4_status st;

    pe->base = base;
    pe->size = size;

    if (size > TL4_PE_MAX_FILE_SIZE) return TL4_E_TOO_LARGE;
    if (size < 64) return TL4_E_TRUNCATED;

    /* R-PE-2 */
    if (!rd16(base, size, 0, &v) || v != PE_DOS_MAGIC) return TL4_E_DOS_HEADER;
    if (!rd32(base, size, 0x3C, &e_lfanew)) return TL4_E_DOS_HEADER;
    if (e_lfanew < 64 || !in_range(size, e_lfanew, 4 + 20)) return TL4_E_DOS_HEADER;
    if (!rd32(base, size, e_lfanew, &v) || v != PE_SIGNATURE) return TL4_E_PE_SIGNATURE;

    /* R-PE-3 */
    fh_off = e_lfanew + 4;
    if (!rd16(base, size, fh_off, &v)) return TL4_E_FILE_HEADER;
    pe->machine = (uint16_t)v;
    if (!rd16(base, size, fh_off + 2, &nsections)) return TL4_E_FILE_HEADER;
    if (nsections == 0 || nsections > TL4_PE_MAX_SECTIONS) return TL4_E_FILE_HEADER;
    if (!rd16(base, size, fh_off + 16, &opt_size)) return TL4_E_FILE_HEADER;

    opt_off = fh_off + 20;
    if (!in_range(size, opt_off, opt_size)) return TL4_E_TRUNCATED;
    if (!rd16(base, size, opt_off, &magic)) return TL4_E_OPTIONAL_HEADER;
    if (magic == PE_OPT_MAGIC_PE32)
    {
        if (opt_size < 96) return TL4_E_OPTIONAL_HEADER;
        if (!rd32(base, size, opt_off + 92, &nrva)) return TL4_E_OPTIONAL_HEADER;
        dd_off = opt_off + 96;
    }
    else if (magic == PE_OPT_MAGIC_PE32PLUS)
    {
        if (opt_size < 112) return TL4_E_OPTIONAL_HEADER;
        if (!rd32(base, size, opt_off + 108, &nrva)) return TL4_E_OPTIONAL_HEADER;
        dd_off = opt_off + 112;
        pe->is_pe32plus = 1;
    }
    else
        return TL4_E_OPTIONAL_HEADER;

    /* the data directories must lie inside the optional header, and the resource entry must exist */
    if (nrva > 16) return TL4_E_OPTIONAL_HEADER;
    if ((uint64_t)(dd_off - opt_off) + (uint64_t)nrva * 8u > opt_size) return TL4_E_OPTIONAL_HEADER;
    if (nrva <= PE_DIR_RESOURCE) return TL4_E_NO_RESOURCES;
    if (!rd32(base, size, dd_off + PE_DIR_RESOURCE * 8u, &rsrc_rva)) return TL4_E_OPTIONAL_HEADER;
    if (!rd32(base, size, dd_off + PE_DIR_RESOURCE * 8u + 4, &rsrc_size)) return TL4_E_OPTIONAL_HEADER;
    if (rsrc_rva == 0 || rsrc_size < RSRC_DIR_HEADER_SIZE) return TL4_E_NO_RESOURCES;

    /* section table inside the file; every raw range inside the file (R-PE-3, R-PE-4) */
    sect_off = opt_off + opt_size;
    if (!in_range(size, sect_off, (uint64_t)nsections * PE_SECTION_HEADER_SIZE)) return TL4_E_TRUNCATED;
    pe->nsections = (uint16_t)nsections;
    pe->section_table_off = sect_off;

    for (i = 0; i < nsections; i++)
    {
        uint32_t sh = sect_off + i * PE_SECTION_HEADER_SIZE;
        uint32_t va, raw_size, raw_ptr;
        if (!rd32(base, size, sh + 12, &va) || !rd32(base, size, sh + 16, &raw_size) || !rd32(base, size, sh + 20, &raw_ptr))
            return TL4_E_SECTION;
        if (raw_size != 0 && !in_range(size, raw_ptr, raw_size)) return TL4_E_SECTION;
        if ((uint64_t)va + (uint64_t)raw_size > 0xFFFFFFFFu) return TL4_E_SECTION;
        /* the resource directory and its declared size must be backed by this section's raw data (R-PE-5) */
        if (!found && raw_size != 0 && rsrc_rva >= va && (uint64_t)rsrc_rva - va + rsrc_size <= raw_size)
        {
            found = 1;
            pe->rsrc_section_va = va;
            pe->rsrc_section_raw = raw_ptr;
            pe->rsrc_section_raw_size = raw_size;
            pe->rsrc_off = raw_ptr + (rsrc_rva - va);
            pe->rsrc_limit = raw_size - (rsrc_rva - va);
        }
    }
    if (!found) return TL4_E_NO_RESOURCES;
    pe->rsrc_rva = rsrc_rva;
    pe->rsrc_dir_size = rsrc_size;

    /* the validating walk: counts and bounds of the whole tree, nothing kept but statistics */
    st = walk(pe, 0, 0, 0, &counts);
    if (st != TL4_OK) return st;
    pe->ndirectories = counts.directories;
    pe->nentries = counts.entries;
    pe->nresources = counts.resources;
    pe->nmulti_pairs = counts.multi_pairs;

    /* R-PE-8: the resource that routes a file to a loader has exactly one language entry, or no loader gets the file */
    {
        tl4_res_name t = tl4_res_ascii("PACKTHEM_VERSION"), n = tl4_res_id(1);
        tl4_resource res;
        st = tl4_pe_find(pe, &t, &n, &res);
        if (st == TL4_OK && res.nlanguages != 1) return TL4_E_RSRC_LANGUAGES;
        if (st != TL4_OK && st != TL4_E_NOT_FOUND) return st;
    }
    return TL4_OK;
}

tl4_status tl4_pe_open(const uint8_t *base, size_t size, tl4_pe *pe)
{
    tl4_status st;
    if (!pe) return TL4_E_ARG;
    clear_handle(pe);
    if (!base) return TL4_E_ARG;
    st = open_checked(base, size, pe);
    if (st != TL4_OK) clear_handle(pe);   /* R-GEN-5: no partial result */
    return st;
}

/* ---- the resource tree ------------------------------------------------------------------------------------- */

/* A relative offset inside the resource area (from the root directory) with a length, bounded by rsrc_limit. */
static int rsrc_range(const tl4_pe *pe, uint64_t rel, uint64_t len)
{
    return rel <= pe->rsrc_limit && len <= (uint64_t)pe->rsrc_limit - rel;
}

static int rsrc_rd32(const tl4_pe *pe, uint64_t rel, uint32_t *v)
{
    if (!rsrc_range(pe, rel, 4)) return 0;
    return rd32(pe->base, pe->size, (uint64_t)pe->rsrc_off + rel, v);
}

static int rsrc_rd16(const tl4_pe *pe, uint64_t rel, uint32_t *v)
{
    if (!rsrc_range(pe, rel, 2)) return 0;
    return rd16(pe->base, pe->size, (uint64_t)pe->rsrc_off + rel, v);
}

/* Read a directory header; returns the entry count and how many of them the header declares as named (R-PE-7). */
static tl4_status read_directory(const tl4_pe *pe, uint32_t rel, uint32_t *count, uint32_t *named_count)
{
    uint32_t named, ids;
    if (!rsrc_range(pe, rel, RSRC_DIR_HEADER_SIZE)) return TL4_E_RSRC_DIRECTORY;
    if (!rsrc_rd16(pe, rel + 12, &named) || !rsrc_rd16(pe, rel + 14, &ids)) return TL4_E_RSRC_DIRECTORY;
    *count = named + ids;
    *named_count = named;
    if (*count == 0 || *count > TL4_PE_MAX_DIR_ENTRIES) return TL4_E_RSRC_DIRECTORY;
    if (!rsrc_range(pe, rel + RSRC_DIR_HEADER_SIZE, (uint64_t)*count * RSRC_ENTRY_SIZE)) return TL4_E_RSRC_DIRECTORY;
    return TL4_OK;
}

/* Decode an entry's name field into a tl4_res_name, bounding string names (R-PE-7). */
static tl4_status read_name(const tl4_pe *pe, uint32_t name_field, tl4_res_name *out)
{
    tl4_res_name n = {0};
    if (name_field & RSRC_HIGH_BIT)
    {
        uint32_t rel = name_field & ~RSRC_HIGH_BIT, len;
        if (!rsrc_rd16(pe, rel, &len)) return TL4_E_RSRC_NAME;
        if (len == 0 || len > TL4_PE_MAX_NAME_CHARS) return TL4_E_RSRC_NAME;
        if (!rsrc_range(pe, (uint64_t)rel + 2, (uint64_t)len * 2)) return TL4_E_RSRC_NAME;
        n.kind = TL4_NAME_UTF16;
        n.length = len;
        n.utf16 = pe->base + pe->rsrc_off + rel + 2;
    }
    else
    {
        if (name_field > 0xFFFFu) return TL4_E_RSRC_NAME;
        n.kind = TL4_NAME_ID;
        n.id = name_field;
    }
    *out = n;
    return TL4_OK;
}

/* Strict ordering within a directory (R-PE-8): names first, by code unit; then ids, ascending; no duplicates. */
struct order_state {
    int have_prev;
    uint32_t named;      /* the header's NumberOfNamedEntries: the first `named` entries must be strings */
    uint32_t index;      /* of the entry being checked */
    tl4_res_name prev;
};

static tl4_status check_order(struct order_state *o, const tl4_res_name *cur)
{
    int should_be_string = o->index < o->named;
    o->index++;
    if ((cur->kind == TL4_NAME_ID) == should_be_string) return TL4_E_RSRC_ORDER; /* R-PE-8: the header split */
    if (o->have_prev)
    {
        if (o->prev.kind == TL4_NAME_ID)
        {
            if (cur->kind != TL4_NAME_ID) return TL4_E_RSRC_ORDER;        /* a name after an id */
            if (cur->id <= o->prev.id) return TL4_E_RSRC_ORDER;
        }
        else if (cur->kind != TL4_NAME_ID)
        {
            if (utf16_compare(&o->prev, cur) >= 0) return TL4_E_RSRC_ORDER;
        }
    }
    o->have_prev = 1;
    o->prev = *cur;
    return TL4_OK;
}

/* Read a data entry: the data must lie inside the resource section's raw data (R-PE-4, R-PE-9). */
static tl4_status read_data_entry(const tl4_pe *pe, uint32_t rel, tl4_resource *res)
{
    uint32_t rva, dsize;
    if (!rsrc_range(pe, rel, RSRC_DATA_ENTRY_SIZE)) return TL4_E_RSRC_DATA;
    if (!rsrc_rd32(pe, rel, &rva) || !rsrc_rd32(pe, rel + 4, &dsize)) return TL4_E_RSRC_DATA;
    if (rva < pe->rsrc_section_va) return TL4_E_RSRC_DATA;
    if ((uint64_t)rva - pe->rsrc_section_va + dsize > pe->rsrc_section_raw_size) return TL4_E_RSRC_DATA;
    res->rva = rva;
    res->size = dsize;
    res->offset = pe->rsrc_section_raw + (rva - pe->rsrc_section_va);
    res->data = pe->base + res->offset;
    return TL4_OK;
}

static tl4_status count_entries(struct walk_counts *c, uint32_t n)
{
    if (n > TL4_PE_MAX_ENTRIES - c->entries) return TL4_E_RSRC_DIRECTORY;
    c->entries += n;
    c->directories++;
    return TL4_OK;
}

/* The language level under a name entry: 1 to TL4_PE_MAX_LANGUAGES entries, all ids in strictly ascending order, each
 * pointing at a data entry (R-PE-6, R-PE-8). leaf_header() checks the directory, read_language() one entry; both are
 * shared by the validating walk and the lookups, so a lookup re-checks exactly what the validation checked. */
static tl4_status leaf_header(const tl4_pe *pe, uint32_t noff, uint32_t *nlangs)
{
    uint32_t named;
    tl4_status st;
    if ((st = read_directory(pe, noff, nlangs, &named)) != TL4_OK) return st;
    if (*nlangs > TL4_PE_MAX_LANGUAGES || named != 0) return TL4_E_RSRC_LANGUAGES;   /* ids only, bounded (R-PE-8) */
    return TL4_OK;
}

static tl4_status read_language(const tl4_pe *pe, uint32_t noff, uint32_t index, tl4_resource *res)
{
    uint32_t le = noff + RSRC_DIR_HEADER_SIZE + index * RSRC_ENTRY_SIZE, lname, loff;
    if (!rsrc_rd32(pe, le, &lname) || !rsrc_rd32(pe, le + 4, &loff)) return TL4_E_RSRC_ENTRY;
    if (loff & RSRC_HIGH_BIT) return TL4_E_RSRC_DIRECTORY;        /* a language entry must be data (R-PE-6) */
    if (lname & RSRC_HIGH_BIT) return TL4_E_RSRC_NAME;            /* languages are ids */
    if (lname > 0xFFFFu) return TL4_E_RSRC_NAME;
    res->language = lname;
    return read_data_entry(pe, loff, res);
}

/* The whole walk, exactly three levels, iterative (R-PE-6). Used by open (validation) and enum only; the lookups
 * below search the directories instead (R-V4-14). */
static tl4_status walk(const tl4_pe *pe, tl4_res_callback callback, void *context, int *stopped,
                       struct walk_counts *counts)
{
    uint32_t ntypes, t, named;
    struct order_state type_order = {0, 0, 0, {0}};
    tl4_status st;

    st = read_directory(pe, 0, &ntypes, &named);
    if (st != TL4_OK) return st;
    type_order.named = named;
    if ((st = count_entries(counts, ntypes)) != TL4_OK) return st;

    for (t = 0; t < ntypes; t++)
    {
        uint32_t te = RSRC_DIR_HEADER_SIZE + t * RSRC_ENTRY_SIZE;
        uint32_t tname, toff, nnames, n;
        struct order_state name_order = {0, 0, 0, {0}};
        tl4_resource res;

        if (!rsrc_rd32(pe, te, &tname) || !rsrc_rd32(pe, te + 4, &toff)) return TL4_E_RSRC_ENTRY;
        if (!(toff & RSRC_HIGH_BIT)) return TL4_E_RSRC_DIRECTORY;    /* a type entry must be a directory */
        if ((st = read_name(pe, tname, &res.type)) != TL4_OK) return st;
        if ((st = check_order(&type_order, &res.type)) != TL4_OK) return st;
        toff &= ~RSRC_HIGH_BIT;
        if (toff == 0) return TL4_E_RSRC_ENTRY;                       /* the root cannot be its own child */

        if ((st = read_directory(pe, toff, &nnames, &named)) != TL4_OK) return st;
        name_order.named = named;
        if ((st = count_entries(counts, nnames)) != TL4_OK) return st;

        for (n = 0; n < nnames; n++)
        {
            uint32_t ne = toff + RSRC_DIR_HEADER_SIZE + n * RSRC_ENTRY_SIZE;
            uint32_t nname, noff;

            if (!rsrc_rd32(pe, ne, &nname) || !rsrc_rd32(pe, ne + 4, &noff)) return TL4_E_RSRC_ENTRY;
            if (!(noff & RSRC_HIGH_BIT)) return TL4_E_RSRC_DIRECTORY;  /* a name entry must be a directory */
            if ((st = read_name(pe, nname, &res.name)) != TL4_OK) return st;
            if ((st = check_order(&name_order, &res.name)) != TL4_OK) return st;
            noff &= ~RSRC_HIGH_BIT;
            if (noff == 0 || noff == toff) return TL4_E_RSRC_ENTRY;

            {
                uint32_t nlangs, l, prev_lang = 0;
                if ((st = leaf_header(pe, noff, &nlangs)) != TL4_OK) return st;
                if ((st = count_entries(counts, nlangs)) != TL4_OK) return st;
                if (nlangs > 1) counts->multi_pairs++;
                res.nlanguages = nlangs;
                for (l = 0; l < nlangs; l++)
                {
                    if ((st = read_language(pe, noff, l, &res)) != TL4_OK) return st;
                    if (l > 0 && res.language <= prev_lang) return TL4_E_RSRC_ORDER;  /* strictly ascending ids (R-PE-8) */
                    prev_lang = res.language;
                    if (counts->resources >= TL4_PE_MAX_RESOURCES) return TL4_E_RSRC_DIRECTORY; /* R-PE-7 */
                    counts->resources++;

                    if (callback)
                    {
                        int r = callback(&res, context);
                        if (r)
                        {
                            if (stopped) *stopped = r;
                            return TL4_OK;
                        }
                    }
                }
            }
        }
    }
    return TL4_OK;
}

tl4_status tl4_pe_enum(const tl4_pe *pe, tl4_res_callback callback, void *context, int *stopped)
{
    struct walk_counts counts = {0, 0, 0, 0};
    tl4_status st;
    if (!pe || !pe->base || !callback) return TL4_E_ARG;
    if (stopped) *stopped = 0;
    st = walk(pe, callback, context, stopped, &counts);
    return st;
}

/* ---- lookups (R-V4-14) ------------------------------------------------------------------------------------- */

/* A lookup never walks a directory: the validating walk enforced the strict order of R-PE-8 (named entries first,
 * ascending by code unit, then ids ascending), so each level is a binary search. Stored names are compared exactly,
 * an ASCII query is folded to upper case (tl4_res_name_equal). Every read is bounded again and the result is checked
 * like the walk checks it (R-GEN-11): on a buffer that changed after the open a lookup rejects or returns a range
 * inside the buffer, never more. */

static int name_compare(const tl4_res_name *query, const tl4_res_name *stored)
{
    if (query->kind == TL4_NAME_ID) return query->id < stored->id ? -1 : (query->id > stored->id ? 1 : 0);
    return utf16_compare(query, stored);
}

/* Binary search of the directory at `dir` (header already read: `count` entries, the first `named` of them strings)
 * for `want`. On success *entry_rel is the offset of the matching entry and *found its decoded name. */
static tl4_status find_entry(const tl4_pe *pe, uint32_t dir, uint32_t count, uint32_t named, const tl4_res_name *want,
                             uint32_t *entry_rel, tl4_res_name *found)
{
    uint32_t lo, hi;
    if (want->kind == TL4_NAME_ID) { lo = named; hi = count; }
    else
    {
        if ((want->kind == TL4_NAME_ASCII && !want->ascii) || (want->kind == TL4_NAME_UTF16 && !want->utf16))
            return TL4_E_NOT_FOUND;                                   /* a query that matches nothing */
        lo = 0; hi = named;
    }
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t rel = dir + RSRC_DIR_HEADER_SIZE + mid * RSRC_ENTRY_SIZE;
        uint32_t field;
        tl4_res_name n;
        tl4_status st;
        int c;
        if (!rsrc_rd32(pe, rel, &field)) return TL4_E_RSRC_ENTRY;
        if ((st = read_name(pe, field, &n)) != TL4_OK) return st;
        if ((n.kind == TL4_NAME_ID) != (want->kind == TL4_NAME_ID)) return TL4_E_RSRC_ORDER; /* not the half the header declared */
        c = name_compare(want, &n);
        if (c == 0) { *entry_rel = rel; *found = n; return TL4_OK; }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return TL4_E_NOT_FOUND;
}

/* The subdirectory an entry at the type or name level points at. */
static tl4_status subdir_of(const tl4_pe *pe, uint32_t entry_rel, uint32_t parent, uint32_t *dir)
{
    uint32_t off;
    if (!rsrc_rd32(pe, entry_rel + 4, &off)) return TL4_E_RSRC_ENTRY;
    if (!(off & RSRC_HIGH_BIT)) return TL4_E_RSRC_DIRECTORY;          /* type and name entries are directories */
    off &= ~RSRC_HIGH_BIT;
    if (off == 0 || off == parent) return TL4_E_RSRC_ENTRY;
    *dir = off;
    return TL4_OK;
}

/* The type directory of `type`: TL4_E_NOT_FOUND when the root has no such type. */
static tl4_status find_type(const tl4_pe *pe, const tl4_res_name *type, uint32_t *toff, uint32_t *nnames,
                            uint32_t *named_names, tl4_res_name *found)
{
    uint32_t count, named, te;
    tl4_status st;
    if ((st = read_directory(pe, 0, &count, &named)) != TL4_OK) return st;
    if ((st = find_entry(pe, 0, count, named, type, &te, found)) != TL4_OK) return st;
    if ((st = subdir_of(pe, te, 0, toff)) != TL4_OK) return st;
    return read_directory(pe, *toff, nnames, named_names);
}

tl4_status tl4_pe_find(const tl4_pe *pe, const tl4_res_name *type, const tl4_res_name *name, tl4_resource *out)
{
    uint32_t toff, nnames, named, ne, noff;
    tl4_resource res;
    tl4_status st;
    if (!pe || !pe->base || !type || !name || !out) return TL4_E_ARG;
    if ((st = find_type(pe, type, &toff, &nnames, &named, &res.type)) != TL4_OK) return st;
    if ((st = find_entry(pe, toff, nnames, named, name, &ne, &res.name)) != TL4_OK) return st;
    if ((st = subdir_of(pe, ne, toff, &noff)) != TL4_OK) return st;
    if ((st = leaf_header(pe, noff, &res.nlanguages)) != TL4_OK) return st;
    if ((st = read_language(pe, noff, 0, &res)) != TL4_OK) return st;   /* entry 0 is the lowest id: the walk checked the order */
    *out = res;
    return TL4_OK;
}

tl4_status tl4_pe_find_language(const tl4_pe *pe, const tl4_res_name *type, const tl4_res_name *name, uint32_t language,
                                tl4_resource *out)
{
    uint32_t toff, nnames, named, ne, noff, nlangs, l;
    tl4_resource res;
    tl4_status st;
    if (!pe || !pe->base || !type || !name || !out) return TL4_E_ARG;
    if (language > 0xFFFFu) return TL4_E_ARG;
    if ((st = find_type(pe, type, &toff, &nnames, &named, &res.type)) != TL4_OK) return st;
    if ((st = find_entry(pe, toff, nnames, named, name, &ne, &res.name)) != TL4_OK) return st;
    if ((st = subdir_of(pe, ne, toff, &noff)) != TL4_OK) return st;
    if ((st = leaf_header(pe, noff, &nlangs)) != TL4_OK) return st;
    res.nlanguages = nlangs;
    for (l = 0; l < nlangs; l++)                                      /* at most TL4_PE_MAX_LANGUAGES reads (R-V4-14) */
    {
        if ((st = read_language(pe, noff, l, &res)) != TL4_OK) return st;
        if (res.language == language) { *out = res; return TL4_OK; }
        if (res.language > language) break;                           /* ascending ids (R-PE-8): nothing after can match */
    }
    return TL4_E_NOT_FOUND;
}

tl4_status tl4_pe_count_type(const tl4_pe *pe, const tl4_res_name *type, uint32_t *count)
{
    uint32_t toff, nnames, named;
    tl4_res_name found;
    tl4_status st;
    if (count) *count = 0;
    if (!pe || !pe->base || !type || !count) return TL4_E_ARG;
    st = find_type(pe, type, &toff, &nnames, &named, &found);
    if (st == TL4_E_NOT_FOUND) return TL4_OK;                         /* no such type: zero resources */
    if (st != TL4_OK) return st;                                      /* R-GEN-5: a failed call reports no partial count */
    *count = nnames;                                                  /* one lookup result per name, whatever its language count */
    return TL4_OK;
}

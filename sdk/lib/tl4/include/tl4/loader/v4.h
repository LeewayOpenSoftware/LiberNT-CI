/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Version-4 container validator (REQUIREMENTS section 4)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The validator reads a style through the bounded PE reader only (R-PE-1), allocates nothing (R-GEN-7), reads every
 * field unsigned and range-checks it (R-GEN-4), and returns counts, never a model. A rejected style leaves the
 * statistics cleared (R-GEN-5). It is the one validator for install and load (R-GEN-10): both callers pass the same
 * bytes and get the same answer (R-GEN-8). Its cost is bounded at the limits (R-V4-14): every lookup through the
 * reader is a binary search on the directory order of R-PE-8 and no pass compares every pair of names.
 *
 * Format facts (VERIFIED on the 32 reference styles, docs/design/V4-LOADER-SKETCH.md):
 *  - CMAP: UTF-16 class names, each NUL-terminated and padded to 8 bytes; the class id of a record is the ordinal of
 *    the n-th non-empty name. The first names (documentation, sizevariant.*, colorvariant.*) describe the file, not a
 *    class; BCMAP counts from the first real class. A name may repeat (R-V4-2 ruling; three third-party styles
 *    do): every ordinal stays valid, a lookup by name (L2) takes the lowest ordinal, and the validator does not
 *    search for duplicates.
 *  - VMAP: three strings, each a uint32 count of UTF-16 characters including the NUL, the text, padding to 4 bytes.
 *  - BCMAP: uint32 N, then N uint32 entries; entry j belongs to class ordinal j + (CMAP count - N); a value is
 *    0xFFFFFFFF (no base) or the base class in the same shifted ordinal space.
 *  - VARIANT (name NORMAL) and RMAP: 32-byte records {nameId, typeId, classId, partId, stateId, reference,
 *    reserved, cbData}; cbData bytes of inline data follow only when reference == 0; records are 8-byte aligned and
 *    the area ends exactly on a record boundary. FONT, FILENAME and DISKSTREAM records always carry a reference (a
 *    string id, an IMAGE id, a STREAM id); a STRING record may carry a string id too (the RMAP documentation strings
 *    601-608 do in every reference file).
 */
#ifndef TL4_LOADER_V4_H
#define TL4_LOADER_V4_H

#include "pe.h"

/* Limits (REQUIREMENTS section 6) */
#define TL4_V4_MAX_CMAP_BYTES     (256u * 1024u)
#define TL4_V4_MAX_CLASSES        1024u
#define TL4_V4_MAX_DESCRIPTORS    8u          /* leading CMAP names that are not classes (corpus: 3 or 4) */
#define TL4_V4_MAX_NAME_HALF      59u         /* characters of each half of App::Class (R-V4-2) */
#define TL4_V4_MAX_VMAP_CHARS     64u
#define TL4_V4_MAX_VARIANT_BYTES  (2u * 1024u * 1024u)
#define TL4_V4_MAX_RECORDS        32768u
#define TL4_V4_MAX_RMAP_BYTES     (64u * 1024u)
#define TL4_V4_MAX_PARTSTATES     8192u
#define TL4_V4_MAX_CBDATA         65536u
#define TL4_V4_MAX_INTLIST        402u
#define TL4_V4_MAX_STRING_BYTES   4096u
#define TL4_V4_MAX_IMAGES         4096u
#define TL4_V4_MAX_STREAMS        16u
#define TL4_V4_MAX_CHAIN          8u
#define TL4_V4_MAX_FONT_CHARS     255u

/* primitive types of a record (the TMT_* type ids) */
enum tl4_v4_type {
    TL4_V4_ENUM = 200, TL4_V4_STRING = 201, TL4_V4_INT = 202, TL4_V4_BOOL = 203, TL4_V4_COLOR = 204,
    TL4_V4_MARGINS = 205, TL4_V4_FILENAME = 206, TL4_V4_SIZE = 207, TL4_V4_POSITION = 208, TL4_V4_RECT = 209,
    TL4_V4_FONT = 210, TL4_V4_INTLIST = 211, TL4_V4_HBITMAP = 212, TL4_V4_DISKSTREAM = 213, TL4_V4_STREAM = 214,
    TL4_V4_BITMAPREF = 215, TL4_V4_FLOAT = 216, TL4_V4_FLOATLIST = 217
};

/* A validated class map: the bytes and the number of names. Names are read on demand, bounded. */
typedef struct tl4_v4_cmap {
    const uint8_t *data;
    uint32_t size;
    uint32_t count;        /* non-empty names = class ids 0..count-1 */
} tl4_v4_cmap;

typedef struct tl4_v4_vmap {
    const uint8_t *variant, *size_name, *color;   /* UTF-16, not terminated */
    uint32_t variant_chars, size_chars, color_chars;
} tl4_v4_vmap;

typedef struct tl4_v4_bcmap {
    const uint8_t *data;
    uint32_t count;        /* N */
    uint32_t shift;        /* CMAP count - N: the ordinal of the first class with a BCMAP entry */
    uint32_t max_depth;    /* longest base chain seen */
} tl4_v4_bcmap;

/* One record as the walk hands it to a callback. `data` is the inline data (cbData bytes) or NULL. */
typedef struct tl4_v4_record {
    uint32_t name_id, type_id, class_id, part_id, state_id, reference, cb_data;
    const uint8_t *data;
} tl4_v4_record;

typedef struct tl4_v4_walk_stats {
    uint32_t records;              /* records accepted (inline or reference) */
    uint32_t dropped_unknown_type; /* R-V4-8: type outside the known set */
    uint32_t dropped_ref_on_value; /* R-V4-8: reference != 0 on a type that carries none */
    uint32_t partstates;           /* changes of (class, part, state) between consecutive records: an upper bound of
                                      the true count, exact when records are grouped (every reference style is);
                                      an ungrouped style could hit the limit early; L2 reports the true count */
    uint32_t classes;              /* changes of class between consecutive records: an upper bound */
    uint32_t max_part, max_state, max_cb;
} tl4_v4_walk_stats;

typedef struct tl4_v4_stats {
    uint32_t cmap_names;
    uint32_t descriptors;          /* leading non-class names */
    uint32_t bcmap_entries;
    uint32_t bcmap_max_depth;
    tl4_v4_walk_stats variant;
    tl4_v4_walk_stats rmap;
    uint32_t images, streams, string_blocks;
    uint32_t fonts, filenames, diskstreams, strings;                /* reference records seen */
    uint32_t unresolved_fonts, unresolved_filenames, unresolved_diskstreams, unresolved_strings;
} tl4_v4_stats;

/* Called for every accepted record of a walk; return nonzero to stop the walk with TL4_E_V4_REFERENCE. */
typedef int (*tl4_v4_record_callback)(const tl4_v4_record *rec, void *context);

/* The pieces, each usable on one raw blob (the fuzz harness fuzz_v4_blob does), all without allocation. */
tl4_status tl4_v4_check_cmap(const uint8_t *data, uint32_t size, tl4_v4_cmap *out);
/* The n-th class name of a validated map: an O(n) scan of the map, bounded. Not for per-record use (R-MOD-1): the
 * model builder (L2) reads every name once when it builds its index. */
tl4_status tl4_v4_cmap_name(const tl4_v4_cmap *cmap, uint32_t ordinal, const uint8_t **utf16, uint32_t *chars);
tl4_status tl4_v4_check_vmap(const uint8_t *data, uint32_t size, tl4_v4_vmap *out);
tl4_status tl4_v4_check_bcmap(const uint8_t *data, uint32_t size, uint32_t cmap_count, tl4_v4_bcmap *out);
/* Base class of a class ordinal through a validated map: *base = ordinal or UINT32_MAX for none. */
tl4_status tl4_v4_bcmap_base(const tl4_v4_bcmap *bcmap, uint32_t ordinal, uint32_t *base);
/* Walk a record area (VARIANT or RMAP). `max_bytes`/`max_records` are the limits of the area kind. */
tl4_status tl4_v4_walk_records(const uint8_t *data, uint32_t size, uint32_t cmap_count, uint32_t max_bytes,
                               uint32_t max_records, tl4_v4_record_callback callback, void *context,
                               tl4_v4_walk_stats *stats);
/* A string of the module's string table (resource type 6, block id/16 + 1, 16 length-prefixed strings), bounded;
 * TL4_E_NOT_FOUND when the block or the string is absent or empty. */
tl4_status tl4_v4_find_string(const tl4_pe *pe, uint32_t id, const uint8_t **utf16, uint32_t *chars);

/* The whole container. `strings` is an explicitly opened .mui (R-RES-1) or NULL; FONT references resolve in the
 * style's own string table first, then there. */
tl4_status tl4_v4_validate(const tl4_pe *style, const tl4_pe *strings, tl4_v4_stats *stats);

#endif

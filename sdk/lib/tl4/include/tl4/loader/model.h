/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Property model with binary values and the GetTheme* query subset (milestone L2, REQUIREMENTS section 5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The model is built from a style the validator accepted, into a buffer the caller sized with tl4_model_size()
 * (R-MOD-5: the core allocates nothing). It holds four flat arrays: classes (one per CMAP ordinal), part-states
 * (the VARIANT ones grouped by class and sorted by part and state, then the RMAP ones), properties (grouped by
 * part-state, sorted by name id) and the class ordinals sorted by name. It holds no pointer into the input (R-MOD-4):
 * every value is an (offset, size) inside one of the two record areas whose file ranges the build fixed, and every
 * reference (string, IMAGE, STREAM) is resolved at build into a range of the style or of the strings mapping. The
 * reader handles are kept by value; the caller keeps both mappings alive, at the same size, for as long as the model
 * is used, while their bytes may change (R-GEN-11). Building runs the validator and the map checks again (one
 * validator, R-GEN-10; R-MOD-5), fills the arrays in a second record walk that stops at the capacity the sizing
 * pass fixed, sorts with an iterative heapsort on keys that all live in the model (R-MOD-1, R-GEN-6; it swaps elements
 * inside the arrays only, so it stays memory-safe and terminates under any comparison), keeps the first of duplicate
 * (area, class, part, state, name) records (R-V4-9) and orders equal class names by ordinal so a lookup by name
 * returns the lowest (R-V4-2). After a successful build the model is never written (R-MOD-5).
 *
 * tl4_model_find mirrors the engine's MSSTYLES_FindProperty (LiberNT dll/win32/uxtheme/msstyles.c) on the VARIANT
 * area: (part, state) along the class and its stored base chain, then (part, 0), then (0, 0); the chain is at most
 * TL4_V4_MAX_CHAIN bases followed by `globals` once, as the engine appends it; the walk stops at `globals` and does not
 * follow its bases, so a lookup visits at most 10 classes, and it counts its steps (R-MOD-6,
 * docs/design/L2-MODEL-DESIGN.md section 4.2). The RMAP area answers documentation queries only (tl4_model_find_rmap).
 *
 * Getters take the type and size from the model, copy the value bytes once with the range checked against the mapping
 * size, run every further check on the copy (R-MOD-4), and return the file's values unchanged; the range checks of
 * R-MOD-3 are applied by the consumer at the boundary (R-MOD-7, design note section 6).
 */
#ifndef TL4_LOADER_MODEL_H
#define TL4_LOADER_MODEL_H

#include "v4.h"

#define TL4_NO_BASE 0xFFFFFFFFu
#define TL4_MODEL_MAX_CLASS_QUERY (2u * TL4_V4_MAX_NAME_HALF + 2u)   /* 120 code units: the longest valid class name */

/* Where a property was found, as the engine's PROPERTYORIGIN numbers them (uxtheme.h: PO_STATE 0, PO_PART 1,
 * PO_CLASS 2, PO_GLOBAL 3). */
enum tl4_origin { TL4_PO_STATE = 0, TL4_PO_PART = 1, TL4_PO_CLASS = 2, TL4_PO_GLOBAL = 3 };

enum tl4_area { TL4_AREA_VARIANT = 0, TL4_AREA_RMAP = 1 };

/* Where a resolved reference lives. */
enum tl4_ref_where { TL4_REF_UNRESOLVED = 0, TL4_REF_STYLE = 1, TL4_REF_STRINGS = 2 };

typedef struct tl4_class {
    uint32_t name_off;     /* into the CMAP resource: the UTF-16 name, name_chars code units, not terminated here */
    uint32_t name_chars;   /* at most TL4_MODEL_MAX_CLASS_QUERY */
    uint32_t base;         /* BCMAP base ordinal or TL4_NO_BASE, stored at build */
    uint32_t first_ps;     /* its VARIANT part-states: partstates[first_ps .. first_ps + nps) */
    uint32_t nps;
} tl4_class;

typedef struct tl4_partstate {
    uint32_t class_ordinal;
    uint16_t part, state;
    uint8_t area;          /* enum tl4_area */
    uint8_t reserved_[3];
    uint32_t first_prop;   /* its properties: props[first_prop .. first_prop + nprops), sorted by name_id */
    uint32_t nprops;
} tl4_partstate;

typedef struct tl4_prop {
    uint32_t class_ordinal;                /* the sort key: area, class, part, state, name_id, seq */
    uint16_t part, state;
    uint16_t name_id;
    uint8_t area;          /* enum tl4_area: the record area data_off refers to */
    uint8_t type;          /* the TMT_* primitive type the validator accepted */
    uint8_t origin;        /* enum tl4_origin */
    uint8_t ref_where;     /* enum tl4_ref_where */
    uint32_t seq;          /* file index: RMAP records first, then VARIANT (the first-wins rule, R-V4-9) */
    uint32_t reference;    /* 0, or the id the record references (R-V4-7) */
    uint32_t data_off;     /* inline bytes inside the area, when reference == 0 */
    uint32_t cb;           /* their size, exact for the type (R-V4-6); 0 for a reference record (R-MOD-4) */
    uint32_t ref_off;      /* the referenced bytes, resolved at build: a string (UTF-16, not terminated), an IMAGE or */
    uint32_t ref_size;     /* a STREAM resource, inside the mapping ref_where names */
} tl4_prop;

typedef struct tl4_model_counts {
    uint32_t classes;           /* CMAP names, descriptors included */
    uint32_t descriptors;       /* leading non-class names */
    uint32_t partstates;        /* true count of distinct (area, class, part, state) */
    uint32_t props;             /* properties kept */
    uint32_t duplicate_props;   /* records dropped by the first-wins rule (R-V4-9) */
    uint32_t duplicate_names;   /* class names that repeat an earlier name (R-V4-2) */
    uint32_t unresolved;        /* reference records the build could not resolve (bounded by R-V4-12) */
    uint32_t globals;           /* ordinal of `globals` or TL4_NO_BASE */
    uint32_t sysmetrics;        /* ordinal of `sysmetrics` or TL4_NO_BASE */
} tl4_model_counts;

typedef struct tl4_model {
    tl4_pe style;               /* handle copies: the caller keeps the mappings alive at this size while the model lives */
    tl4_pe strings;             /* the explicitly opened .mui (R-RES-1), when has_strings */
    int has_strings;
    uint32_t cmap_off, cmap_size;           /* the CMAP resource inside the style */
    uint32_t area_off[2], area_size[2];     /* the VARIANT and RMAP resources (enum tl4_area): fixed file ranges */
    uint32_t nclasses;
    tl4_class *classes;
    uint32_t npartstates;
    tl4_partstate *partstates;
    uint32_t rmap_first_ps, rmap_nps;       /* the RMAP part-states, after the VARIANT ones */
    uint32_t nprops;
    tl4_prop *props;
    uint32_t *name_order;                   /* nclasses ordinals sorted by name, equal names by ordinal */
    tl4_model_counts counts;
    tl4_v4_stats stats;                     /* what the validator reported for this build */
} tl4_model;

/* A parsed font string ("Segoe UI, 9, Bold, Quality:ClearType"): the only syntax the corpus uses. */
typedef struct tl4_font {
    uint16_t face[32];     /* UTF-16, NUL-terminated, at most 31 characters (R-RES-2, LF_FACESIZE) */
    uint32_t face_chars;
    int32_t points;        /* 1..255 */
    uint8_t bold, italic, underline, strikeout;
    uint8_t quality;       /* LOGFONT quality: 0 default, 1 draft, 2 proof, 3 non-antialiased, 4 antialiased,
                              5 ClearType, 6 ClearType natural */
    uint8_t reserved_[3];
} tl4_font;

/* Bytes a model for a style with these statistics needs (checked arithmetic); 0 when the statistics are impossible. */
size_t tl4_model_size(const tl4_v4_stats *stats);

/* Validate `style` (with `strings`, or NULL) and build the model into buffer[buffer_size]. The buffer must be 8-byte
 * aligned and at least tl4_model_size() of the statistics the validation returns; otherwise TL4_E_MODEL_SIZE. On any
 * failure *out is cleared (R-GEN-5). */
tl4_status tl4_model_build(const tl4_pe *style, const tl4_pe *strings, void *buffer, size_t buffer_size, tl4_model *out);

/* The lowest ordinal whose CMAP name equals the query exactly (code units, no folding); a query longer than
 * TL4_MODEL_MAX_CLASS_QUERY is TL4_E_ARG. TL4_E_NOT_FOUND when no class has the name. */
tl4_status tl4_model_class(const tl4_model *m, const uint8_t *utf16, uint32_t chars, uint32_t *ordinal);
tl4_status tl4_model_class_ascii(const tl4_model *m, const char *name, uint32_t *ordinal);
/* The name of a class ordinal: a pointer into the CMAP and its length, re-checked against the mapping. */
tl4_status tl4_model_class_name(const tl4_model *m, uint32_t ordinal, const uint8_t **utf16, uint32_t *chars);

/* The VARIANT property (name_id) for (class, part, state) in the engine's search order (header). TL4_E_NOT_FOUND when
 * no class of the chain has it. */
tl4_status tl4_model_find(const tl4_model *m, uint32_t class_ordinal, uint32_t part, uint32_t state, uint32_t name_id,
                          const tl4_prop **out);
/* The RMAP property (name_id) at exactly (class, part, state): documentation queries, no chain (R-MOD-6). */
tl4_status tl4_model_find_rmap(const tl4_model *m, uint32_t class_ordinal, uint32_t part, uint32_t state, uint32_t name_id,
                               const tl4_prop **out);
/* A VARIANT part-state (part, 0) exists in the class or its chain (IsThemePartDefined). */
int tl4_model_part_defined(const tl4_model *m, uint32_t class_ordinal, uint32_t part);

/* Getters (R-MOD-2, R-MOD-4): TL4_E_TYPE when the property is not of the asked type, TL4_E_TRUNCATED when the bytes
 * are no longer inside the mapping or no longer what the validator saw (R-GEN-11), TL4_E_BUFFER when the caller's
 * buffer is too small, TL4_E_V4_REFERENCE for a reference the build could not resolve. */
tl4_status tl4_get_int(const tl4_model *m, const tl4_prop *p, int32_t *value);          /* INT, SIZE */
tl4_status tl4_get_bool(const tl4_model *m, const tl4_prop *p, int32_t *value);         /* BOOL: 0 or 1 */
tl4_status tl4_get_enum(const tl4_model *m, const tl4_prop *p, int32_t *value);         /* ENUM */
tl4_status tl4_get_color(const tl4_model *m, const tl4_prop *p, uint32_t *colorref);    /* COLOR */
tl4_status tl4_get_position(const tl4_model *m, const tl4_prop *p, int32_t xy[2]);      /* POSITION */
tl4_status tl4_get_rect(const tl4_model *m, const tl4_prop *p, int32_t ltrb[4]);        /* RECT */
tl4_status tl4_get_margins(const tl4_model *m, const tl4_prop *p, int32_t lrtb[4]);     /* MARGINS */
/* INTLIST: up to `cap` values; *count = (cb - 4) / 4 from the model (at most TL4_V4_MAX_INTLIST). */
tl4_status tl4_get_intlist(const tl4_model *m, const tl4_prop *p, int32_t *values, uint32_t cap, uint32_t *count);
/* STRING, inline or by string-table reference: UTF-16 copy, NUL-terminated, *chars without the NUL. */
tl4_status tl4_get_string(const tl4_model *m, const tl4_prop *p, uint16_t *out, uint32_t cap_chars, uint32_t *chars);
/* FONT: the referenced string, copied and parsed (TL4_E_FONT_SYNTAX when it is not the known syntax). */
tl4_status tl4_get_font(const tl4_model *m, const tl4_prop *p, tl4_font *font);
/* FILENAME / DISKSTREAM: the referenced IMAGE / STREAM id and its bytes as (offset, size) inside the style, for the
 * chunk walker (R-RES-3); the bytes are never handed to a decoder from here. */
tl4_status tl4_get_filename(const tl4_model *m, const tl4_prop *p, uint32_t *id, uint32_t *offset, uint32_t *size);
tl4_status tl4_get_diskstream(const tl4_model *m, const tl4_prop *p, uint32_t *id, uint32_t *offset, uint32_t *size);
/* GetThemeMetric's rule: the first 32-bit word of INT, SIZE, BOOL, ENUM, COLOR, POSITION (x), MARGINS (left) or
 * INTLIST (first value, at least one); never STRING, FILENAME or FONT (TL4_E_TYPE). */
tl4_status tl4_get_metric(const tl4_model *m, const tl4_prop *p, int32_t *value);

/* The font string parser on its own, over a copy the caller holds (tests, and the adapter's v3 path if it wants the
 * same rules). */
tl4_status tl4_parse_font(const uint8_t *utf16, uint32_t chars, tl4_font *font);

#ifdef TL4_TESTING
/* Test hook (R-GEN-11, R-MOD-5): called by tl4_model_build after its validation and before it reads the maps and
 * records again, with the style's bytes. Only the TL4_TESTING build of the core has it; a shipped core has no hook. */
extern void (*tl4_model_test_between_passes)(const uint8_t *data, size_t size);
#endif

#endif

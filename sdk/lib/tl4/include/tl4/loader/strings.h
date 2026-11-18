/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Bounded string-table reader (REQUIREMENTS R-RES-1, R-RES-2, R-RES-8; docs/design/L3-STRINGS-DESIGN.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A string table is resource type 6: block `b` (an id 1..TL4_STR_MAX_BLOCKS) holds the strings with ids 16(b-1) to
 * 16b-1 as 16 length-prefixed UTF-16 strings, a 16-bit code-unit count then the units, no terminator, an empty slot
 * being a count of 0. tl4_strings_check() applies the shape check of R-RES-8 to every language entry of every block
 * and rejects the whole file on the first failure (R-GEN-5). A lookup never relies on that check (R-GEN-11): it takes
 * the block range from the resource reader again, bounds every count it reads against it, and uses each count from
 * one read; the getter copies once into the caller's buffer and checks the copy. The core selects no file and no
 * language fallback happens here (R-RES-1): a lookup asks for the lowest language id of the block (what tl4_pe_find
 * returns under R-PE-8, the version-4 case where every block has one entry) or for one explicit id.
 */
#ifndef TL4_LOADER_STRINGS_H
#define TL4_LOADER_STRINGS_H

#include <stddef.h>
#include <stdint.h>
#include "pe.h"

/* Limits (REQUIREMENTS section 6; corpus maxima: 61 blocks, 668 bytes, 72 characters; shipped v3 style 14 languages) */
#define TL4_STR_MAX_CHARS    1024u                                   /* code units of one string (a referenced string: 255, R-RES-2) */
#define TL4_STR_MAX_BLOCKS   4096u                                   /* block ids 1..4096 cover the string ids 0..65,535 */
#define TL4_STR_MAX_BLOCK    (16u * (2u + 2u * TL4_STR_MAX_CHARS))   /* 32,800 bytes */
#define TL4_STR_LANG_LOWEST  0xFFFFFFFFu                             /* "the entry with the lowest language id" (outside 0..65,535) */

typedef struct tl4_strings_stats {
    uint32_t blocks;         /* (type 6, name) pairs */
    uint32_t entries;        /* language entries checked: one block image each */
    uint32_t strings;        /* non-empty slots over all entries */
    uint32_t max_chars;      /* longest string */
    uint32_t longest_id;     /* its id (of the first entry that reaches max_chars) */
    uint32_t max_languages;  /* entries of the widest block (1 in every version-4 file) */
} tl4_strings_stats;

/* R-RES-8 on the whole table: every language entry of every type-6 block, in the order of tl4_pe_enum. TL4_OK with the
 * statistics, or the first failure with *stats cleared: TL4_E_STR_BLOCK for a block that is not an id 1..4096, is over
 * TL4_STR_MAX_BLOCK bytes, is not exactly 16 strings ending on its size, has a count over TL4_STR_MAX_CHARS or a
 * code unit 0. A file without a string table passes with zero counts. */
tl4_status tl4_strings_check(const tl4_pe *pe, tl4_strings_stats *stats);

/* Locate string `id` (0..65,535) in the entry of its block selected by `language` (TL4_STR_LANG_LOWEST or an id
 * 0..65,535; anything else is TL4_E_ARG): *offset is the file offset of its first code unit and *chars its count,
 * 1..TL4_STR_MAX_CHARS; [offset, offset + 2*chars) lies inside the mapping. TL4_E_NOT_FOUND for an absent block, an
 * absent language or an empty slot; TL4_E_STR_BLOCK when a count met on the way does not fit the block. Both outputs are
 * 0 on any failure. The code units themselves are not inspected here: the getter checks its copy. */
tl4_status tl4_strings_locate(const tl4_pe *pe, uint32_t id, uint32_t language, uint32_t *offset, uint32_t *chars);

/* Copy string `id` into out[cap_chars] as a 0-terminated UTF-16 string, *chars its count without the terminator. The
 * capacity needed is chars + 1; a smaller one (0 included) is TL4_E_BUFFER and nothing is truncated (R-GEN-5). One copy,
 * then the checks on the copy: count at most TL4_STR_MAX_CHARS, no code unit 0 (TL4_E_STR_BLOCK, with the whole copied
 * range out[0..chars] cleared). On any failure *chars is 0 and out[0] is 0 when there is an out[0]. */
tl4_status tl4_strings_get(const tl4_pe *pe, uint32_t id, uint32_t language, uint16_t *out, uint32_t cap_chars, uint32_t *chars);

/* The pair a consumer holds: the style and, when it has one, the strings module it opened under R-RES-1 (T-STORE-3).
 * The structure holds pointers only: both tl4_pe and the mappings behind them must stay valid and unchanged in size
 * for as long as the pair is used; the pair owns nothing and there is nothing to close. */
typedef struct tl4_strings {
    const tl4_pe *style;
    const tl4_pe *mui;       /* NULL when the style has no module */
} tl4_strings;

/* Check both tables (tl4_strings_check) and fill the pair; on failure *out is cleared (both pointers NULL). */
tl4_status tl4_strings_open(const tl4_pe *style, const tl4_pe *mui_or_null, tl4_strings *out);

/* tl4_strings_get from the style; only TL4_E_NOT_FOUND there continues to the module (R-RES-1): any other status, and
 * TL4_E_NOT_FOUND when there is no module, is returned as it is. */
tl4_status tl4_strings_get2(const tl4_strings *s, uint32_t id, uint32_t language, uint16_t *out, uint32_t cap_chars, uint32_t *chars);

#endif

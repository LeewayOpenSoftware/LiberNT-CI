/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Bounded PE / resource reader (REQUIREMENTS section 3)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The reader works on a byte range the caller already holds in memory: (base, size). The size MUST come from the
 * same object that was mapped (the open handle or the mapping itself), never from a path lookup made earlier
 * (R-POL-2). The reader never allocates (R-GEN-7), reads input only through bounded little-endian helpers (R-GEN-2),
 * computes every offset in 64 bits (R-GEN-3) and validates the whole resource tree in tl4_pe_open() before any
 * lookup is answered. It accepts only the shape a style needs: a tree of exactly three levels (type, name,
 * language) with strictly ordered entries and 1 to TL4_PE_MAX_LANGUAGES language entries per (type, name), all ids in
 * ascending order (R-PE-6, R-PE-8 as amended 2026-11-15 for localised version-3 styles), whose directories,
 * entries, names and data all lie inside the raw data of the section that holds the resource directory (R-PE-5,
 * R-PE-9). Anything else is rejected, never skipped, and a failed tl4_pe_open() leaves the handle cleared so that
 * no later call can use it (R-GEN-5). Nothing else in the file is parsed (R-PE-10). The input may change under the
 * reader (R-GEN-11): every value that bounds a read is read once into a local and every lookup re-checks its own
 * reads, so a byte that changes between the validating walk and a lookup can only turn the lookup into a reject.
 */
#ifndef TL4_LOADER_PE_H
#define TL4_LOADER_PE_H

#include <stddef.h>
#include <stdint.h>
#include "status.h"

/* Limits (REQUIREMENTS section 6; corpus maxima: file 2,976,256 bytes, 4 sections, 1,611 entries in one
 * directory, names of 20 characters). */
#define TL4_PE_MAX_FILE_SIZE     (32u * 1024u * 1024u)
#define TL4_PE_MAX_SECTIONS      32u
#define TL4_PE_MAX_DIR_ENTRIES   4096u   /* named + id entries in one directory (R-PE-7) */
#define TL4_PE_MAX_ENTRIES       65536u  /* directory entries visited per file (R-PE-7) */
#define TL4_PE_MAX_RESOURCES     16384u  /* data entries per file (R-PE-7) */
#define TL4_PE_MAX_NAME_CHARS    64u     /* UTF-16 code units of a string name (R-PE-7) */
#define TL4_PE_MAX_LANGUAGES     64u     /* language entries of one (type, name) pair (R-PE-8) */
#define TL4_PE_DEPTH             3u      /* type / name / language (R-PE-6) */

typedef struct tl4_pe {
    const uint8_t *base;
    size_t size;
    uint16_t machine;
    uint16_t nsections;
    uint8_t is_pe32plus;
    uint8_t reserved_[3];
    uint32_t section_table_off;     /* file offset of the first section header */
    uint32_t rsrc_rva;              /* resource data directory RVA (= the root directory) */
    uint32_t rsrc_dir_size;         /* resource data directory size as declared */
    uint32_t rsrc_section_va;       /* the section that holds the resource directory */
    uint32_t rsrc_section_raw;      /* its PointerToRawData */
    uint32_t rsrc_section_raw_size; /* its SizeOfRawData */
    uint32_t rsrc_off;              /* file offset of the root directory */
    uint32_t rsrc_limit;            /* bytes from the root directory to the end of the section's raw data */
    /* statistics from the validating walk (R-GEN-7: counts only, nothing kept) */
    uint32_t ndirectories;
    uint32_t nentries;
    uint32_t nresources;            /* data entries: one per language entry */
    uint32_t nmulti_pairs;          /* (type, name) pairs with more than one language entry (R-PE-8) */
} tl4_pe;

enum tl4_name_kind {
    TL4_NAME_ID = 0,      /* a 16-bit ordinal, as MAKEINTRESOURCE */
    TL4_NAME_UTF16 = 1,   /* a UTF-16LE string of `length` code units inside the image, not terminated */
    TL4_NAME_ASCII = 2    /* a caller query: folded to upper case (ASCII only) and compared exactly with stored names */
};

typedef struct tl4_res_name {
    uint8_t kind;                /* enum tl4_name_kind */
    uint8_t reserved_[3];
    uint32_t id;                 /* TL4_NAME_ID */
    uint32_t length;             /* TL4_NAME_UTF16: code units; TL4_NAME_ASCII: bytes */
    const uint8_t *utf16;        /* TL4_NAME_UTF16 */
    const char *ascii;           /* TL4_NAME_ASCII; must stay valid while the name is used */
} tl4_res_name;

typedef struct tl4_resource {
    tl4_res_name type;
    tl4_res_name name;
    uint32_t language;
    uint32_t nlanguages;         /* language entries of this (type, name) pair, 1..TL4_PE_MAX_LANGUAGES (R-PE-8) */
    uint32_t rva;                /* of the data */
    uint32_t offset;             /* file offset of the data (R-PE-9): [offset, offset + size) lies inside the
                                  * raw data of the resource section, hence inside the file */
    uint32_t size;
    const uint8_t *data;         /* base + offset, for convenience; derived from the checked offset only */
} tl4_resource;

tl4_res_name tl4_res_id(uint32_t id);
/* A caller string longer than TL4_PE_MAX_NAME_CHARS yields a name that matches nothing. */
tl4_res_name tl4_res_ascii(const char *ascii);
/* ids equal by value; an id never equals a string. Stored (UTF-16) names are compared exactly, code unit by code
 * unit; only a caller's ASCII query is folded to upper case before the comparison, because resource names are stored
 * upper case by the resource compiler (VERIFIED on every input file) and a stored lower-case name must stay
 * unreachable rather than ambiguous (review MUST 3). Two queries are compared folded. */
int tl4_res_name_equal(const tl4_res_name *a, const tl4_res_name *b);

/* Validate the headers and the whole resource tree. On success every later call only re-reads validated memory: enum
 * walks it again, find and count_type binary-search the directories on the order of R-PE-8 (R-V4-14). */
tl4_status tl4_pe_open(const uint8_t *base, size_t size, tl4_pe *pe);

/* Enumerate every resource: one call per language entry, in directory order (ascending ids inside a pair). The
 * callback returns 0 to continue, nonzero to stop; the nonzero value is reported through `stopped` when it is not
 * NULL. */
typedef int (*tl4_res_callback)(const tl4_resource *res, void *context);
tl4_status tl4_pe_enum(const tl4_pe *pe, tl4_res_callback callback, void *context, int *stopped);

/* Find the resource (type, name): the entry with the lowest language id; out->nlanguages tells how many entries the
 * pair has (R-PE-8). PACKTHEM_VERSION has exactly one in any file this reader opens (tl4_pe_open rejects the rest). */
tl4_status tl4_pe_find(const tl4_pe *pe, const tl4_res_name *type, const tl4_res_name *name, tl4_resource *out);

/* Find the entry of (type, name) with exactly the language id `language` (0..65,535; anything else is TL4_E_ARG): the
 * leaf check of tl4_pe_find, then a scan of at most TL4_PE_MAX_LANGUAGES ascending entries that stops at the first id
 * above the wanted one. TL4_E_NOT_FOUND when the pair or the language is absent. */
tl4_status tl4_pe_find_language(const tl4_pe *pe, const tl4_res_name *type, const tl4_res_name *name, uint32_t language,
                                tl4_resource *out);

/* Count the name entries under a type (pairs, whatever their language count): TL4_OK with *count (0 when the type is
 * absent), or the lookup's error. Like
 * every lookup it is meaningful only on a handle a successful tl4_pe_open() filled: a failed open clears the handle
 * and the call then returns TL4_E_ARG with *count 0. */
tl4_status tl4_pe_count_type(const tl4_pe *pe, const tl4_res_name *type, uint32_t *count);

#endif

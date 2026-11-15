/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Status codes shared by every reader and validator
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#ifndef TL4_LOADER_STATUS_H
#define TL4_LOADER_STATUS_H

typedef enum tl4_status {
    TL4_OK = 0,
    TL4_E_ARG,             /* a NULL pointer or an impossible argument */
    TL4_E_TOO_LARGE,       /* the input exceeds a fixed limit (file size, counts) */
    TL4_E_TRUNCATED,       /* a structure extends past the validated size */
    TL4_E_DOS_HEADER,      /* no MZ header or e_lfanew out of bounds */
    TL4_E_PE_SIGNATURE,    /* no PE\0\0 at e_lfanew */
    TL4_E_FILE_HEADER,     /* IMAGE_FILE_HEADER rejected */
    TL4_E_OPTIONAL_HEADER, /* optional header magic, size or data directories rejected */
    TL4_E_SECTION,         /* a section header is inconsistent with the file */
    TL4_E_NO_RESOURCES,    /* no resource data directory, or none inside a section's raw data */
    TL4_E_RSRC_DIRECTORY,  /* a resource directory is out of bounds, too deep, too wide or empty */
    TL4_E_RSRC_ENTRY,      /* a directory entry points outside the resource area or at itself */
    TL4_E_RSRC_NAME,       /* a string name is out of bounds, empty or too long */
    TL4_E_RSRC_ORDER,      /* entries are not strictly ascending with names before ids (R-PE-8) */
    TL4_E_RSRC_LANGUAGES,  /* a pair has over TL4_PE_MAX_LANGUAGES language entries or a named one, PACKTHEM_VERSION
                            * has more than one, or the version-4 path met a multi-language pair (R-PE-8) */
    TL4_E_RSRC_DATA,       /* a data entry is outside the section's raw data */
    TL4_E_NOT_FOUND,       /* lookup: no such resource */

    /* version-4 container validator (REQUIREMENTS section 4) */
    TL4_E_V4_VERSION,      /* PACKTHEM_VERSION missing, not 2 bytes or not 4 (R-V4-1) */
    TL4_E_V4_MISSING,      /* a required resource (CMAP, VMAP, BCMAP, RMAP, VARIANT NORMAL) is absent */
    TL4_E_V4_CMAP,         /* class map malformed: odd size, unterminated, empty half, two '::', too long, duplicate (R-V4-2) */
    TL4_E_V4_VMAP,         /* variant map malformed (R-V4-3) */
    TL4_E_V4_BCMAP,        /* base-class map malformed, bad entry, cycle or chain too deep (R-V4-10) */
    TL4_E_V4_RECORD,       /* a record header field out of range (R-V4-5) */
    TL4_E_V4_SIZE,         /* a record's data size does not match its type (R-V4-6) */
    TL4_E_V4_END,          /* a record area does not end exactly on a record boundary (R-V4-4) */
    TL4_E_V4_LIMIT,        /* a count or size over a limit of REQUIREMENTS section 6 */
    TL4_E_V4_REFERENCE,    /* unresolved FONT reference, or more than 1% of records unresolved (R-V4-12) */

    /* property model and queries (REQUIREMENTS section 5, milestone L2) */
    TL4_E_MODEL_SIZE,      /* the caller's buffer is smaller than tl4_model_size(), or the input grew since the sizing pass */
    TL4_E_TYPE,            /* the property is not of the type the getter serves */
    TL4_E_BUFFER,          /* the caller's output buffer is too small (never truncated, R-GEN-5) */
    TL4_E_FONT_SYNTAX,     /* a font string outside the known syntax */
    TL4_E_STR_BLOCK,       /* a string-table block fails the shape check of R-RES-8: name not an id 1..4096, over 32,800
                            * bytes, not exactly 16 strings ending on its size, a count over 1,024, or a code unit 0 */

    /* PNG chunk walker (REQUIREMENTS R-RES-3/4/5, milestone L3) */
    TL4_E_PNG_SIGNATURE,   /* not a PNG, or too short for one */
    TL4_E_PNG_CHUNK,       /* a chunk's length, order, repeat or type is not allowed */
    TL4_E_PNG_CRC,         /* a chunk's CRC does not match */
    TL4_E_PNG_IHDR,        /* IHDR not first, not 13 bytes, or outside the accepted parameter set */
    TL4_E_LIMIT            /* an image limit of section 6 (side, pixels, compressed size, chunks, budget) */
} tl4_status;

const char *tl4_status_name(tl4_status status);

#endif

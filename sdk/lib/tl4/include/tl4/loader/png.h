/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     PNG chunk walker: what an IMAGE or STREAM resource must be before it reaches a decoder (milestone L3,
 *              REQUIREMENTS R-RES-3/4/5, docs/design/L3-IMAGES-DESIGN.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The walker reads one resource, (data, size), that the L2 getters located (tl4_get_filename, tl4_get_diskstream) or
 * that the caller holds, and decides whether a decoder may see it: the PNG signature; a bounded chunk loop in which
 * every length lies inside the resource and every CRC-32 matches; IHDR first with the accepted parameter set (8-bit,
 * colour type 2 or 6, no interlace); the side, pixel and compressed-size limits of REQUIREMENTS section 6, taken from
 * IHDR before any decoding; an allow-list of ancillary chunk types in one table, each with its exact or bounded length,
 * a value range, at most once and only before the first IDAT (gAMA, pHYs, sRGB, tEXt: bounded and range-checked, never
 * interpreted); IDAT chunks contiguous; IEND last and the resource ending exactly there. Every other chunk type,
 * critical or ancillary, PLTE and tRNS included (no corpus evidence), is a reject (R-GEN-5). The IDAT enumeration is
 * the same single pass with a callback: the full check, CRCs included, runs every time. The walker allocates nothing,
 * reads every value once into a local (R-GEN-11), decodes nothing and knows no zlib.
 *
 * The consumer copies the resource once into its own buffer, runs the walker on the copy and decodes the copy, so the
 * decoder sees exactly the bytes that were checked (design note section 3.2, R-RES-6).
 */
#ifndef TL4_LOADER_PNG_H
#define TL4_LOADER_PNG_H

#include <stddef.h>
#include <stdint.h>
#include "status.h"

/* Limits (REQUIREMENTS section 6; corpus maxima: side 3,720, 1,237,800 pixels, 345,332 compressed bytes, 12 chunks,
 * 6 IDAT chunks, tEXt 27 bytes). */
#define TL4_PNG_MAX_SIDE         8192u
#define TL4_PNG_MAX_PIXELS       (8u * 1024u * 1024u)          /* 8 M = 2^23 */
#define TL4_PNG_MAX_COMPRESSED   (4u * 1024u * 1024u)          /* the IDAT total */
#define TL4_PNG_MAX_RESOURCE     (TL4_PNG_MAX_COMPRESSED + 64u * 1024u)
#define TL4_PNG_MAX_CHUNKS       64u
#define TL4_PNG_MAX_IDAT_CHUNKS  64u
#define TL4_PNG_MAX_TEXT         256u
#define TL4_PNG_DEFAULT_BUDGET   (16u * 1024u * 1024u)         /* decoded pixels per style per process (R-RES-4) */

typedef struct tl4_png_info {
    uint32_t width, height;            /* 1..TL4_PNG_MAX_SIDE */
    uint8_t bit_depth, colour_type;    /* 8; 2 (RGB) or 6 (RGBA) */
    uint8_t interlace;                 /* 0 */
    uint8_t has_srgb, has_gama, has_phys, has_text;   /* the ancillary chunks of the allow-list, bounded, not read */
    uint32_t chunks, idat_chunks;
    uint32_t idat_bytes;               /* the compressed stream, <= TL4_PNG_MAX_COMPRESSED */
    uint64_t pixels;                   /* width * height, <= TL4_PNG_MAX_PIXELS */
} tl4_png_info;

/* The whole check. On failure *info is cleared (R-GEN-5). */
tl4_status tl4_png_check(const uint8_t *data, uint32_t size, tl4_png_info *info);

/* The IDAT chunk data ranges in file order, as (offset, size) into `data`, for a consumer that concatenates the
 * compressed stream for a decoder. It is tl4_png_check's own pass: every check, CRCs included, runs again
 * (R-GEN-11), and the callback is called only after the whole image passed, so no range of a rejected image is ever
 * handed out. A nonzero return stops the remaining calls (reported through `stopped`) and is harmless: the check is
 * complete by then and the status stays TL4_OK. */
typedef int (*tl4_png_idat_cb)(uint32_t offset, uint32_t size, void *context);
tl4_status tl4_png_idat_enum(const uint8_t *data, uint32_t size, tl4_png_idat_cb cb, void *context, int *stopped);

/* R-RES-4: decoded pixels per style, per process. The consumer owns the instance (one per style); the core only does
 * the checked sum. TL4_E_LIMIT when the request would pass the limit; `used` is unchanged then. */
typedef struct tl4_pixel_budget { uint64_t limit, used; } tl4_pixel_budget;
tl4_status tl4_pixel_budget_take(tl4_pixel_budget *budget, uint64_t pixels);

#endif

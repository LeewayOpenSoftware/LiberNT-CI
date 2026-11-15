/*
 * PROJECT:     Theme Loader 4 - loader
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     PNG chunk walker (milestone L3, REQUIREMENTS R-RES-3/4/5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <tl4/loader/png.h>
#include <string.h>

/* ---- bounded reads (R-GEN-2) ------------------------------------------------------------------------------- */

static int in_range(uint64_t size, uint64_t off, uint64_t len)
{
    return off <= size && len <= size - off;
}

/* big-endian, as PNG stores every integer */
static int rd32be(const uint8_t *d, uint64_t size, uint64_t off, uint32_t *v)
{
    if (!in_range(size, off, 4)) return 0;
    *v = ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) | ((uint32_t)d[off + 2] << 8) | (uint32_t)d[off + 3];
    return 1;
}

/* ---- CRC-32 (ISO 3309, as PNG), table-driven, no state ------------------------------------------------------ */

static const uint32_t crc_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
    0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
    0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
    0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
    0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
    0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
    0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
    0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
    0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
    0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
    0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
    0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
    0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
    0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

/* ---- chunk types ------------------------------------------------------------------------------------------- */

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define T_IHDR FOURCC('I', 'H', 'D', 'R')
#define T_IDAT FOURCC('I', 'D', 'A', 'T')
#define T_IEND FOURCC('I', 'E', 'N', 'D')
#define T_GAMA FOURCC('g', 'A', 'M', 'A')
#define T_PHYS FOURCC('p', 'H', 'Y', 's')
#define T_SRGB FOURCC('s', 'R', 'G', 'B')
#define T_TEXT FOURCC('t', 'E', 'X', 't')

/* The ancillary allow-list (R-RES-3 as ruled 2026-11-15): one table, exact or bounded lengths, a value range checked
 * by `shape`, each type at most once and only before the first IDAT, never interpreted. */
struct ancillary { uint32_t type; uint32_t min_len, max_len; int (*shape)(const uint8_t *p, uint32_t n); };
static int gama_shape_ok(const uint8_t *p, uint32_t n) { return n == 4 && (p[0] | p[1] | p[2] | p[3]) != 0; }   /* gamma != 0 */
static int phys_shape_ok(const uint8_t *p, uint32_t n) { return n == 9 && p[8] <= 1; }                       /* unit 0 or 1 */
static int srgb_shape_ok(const uint8_t *p, uint32_t n) { return n == 1 && p[0] <= 3; }                       /* rendering intent */
static int text_shape_ok(const uint8_t *p, uint32_t n);
static const struct ancillary allowed_ancillary[] = {
    { T_GAMA, 4, 4, gama_shape_ok },
    { T_PHYS, 9, 9, phys_shape_ok },
    { T_SRGB, 1, 1, srgb_shape_ok },
    { T_TEXT, 2, TL4_PNG_MAX_TEXT, text_shape_ok },
};

static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };

static void clear_info(tl4_png_info *info) { memset(info, 0, sizeof(*info)); }

/* tEXt: a keyword of 1..79 printable Latin-1 characters, exactly one NUL, printable Latin-1 text (not interpreted) */
static int text_shape_ok(const uint8_t *p, uint32_t n)
{
    uint32_t i, nul = n;
    for (i = 0; i < n; i++)
    {
        uint8_t c = p[i];
        if (c == 0) { if (nul != n) return 0; nul = i; continue; }       /* a second NUL */
        if (!((c >= 32 && c <= 126) || c >= 161)) return 0;
    }
    return nul != n && nul >= 1 && nul <= 79;                             /* exactly one NUL, after 1..79 keyword bytes */
}

/* The one pass over the chunks: the whole check, CRCs included, every time; with the two arrays (of
 * TL4_PNG_MAX_IDAT_CHUNKS entries) it also records the IDAT ranges, which tl4_png_idat_enum hands out only after the
 * pass accepted the whole image. There is no second parser. */
static tl4_status walk(const uint8_t *data, uint32_t size, tl4_png_info *info, uint32_t *idat_off, uint32_t *idat_len)
{
    uint64_t off = 8;
    uint32_t seen_ancillary = 0;       /* bit i: allowed_ancillary[i] seen */
    int seen_ihdr = 0, seen_idat = 0, idat_open = 0, seen_iend = 0;
    uint64_t idat_total = 0;

    if (!data) return TL4_E_ARG;
    if (size < 8u + 25u + 12u + 12u) return TL4_E_PNG_SIGNATURE;           /* signature, IHDR, one IDAT, IEND */
    if (size > TL4_PNG_MAX_RESOURCE) return TL4_E_LIMIT;
    if (memcmp(data, signature, 8) != 0) return TL4_E_PNG_SIGNATURE;

    while (!seen_iend)
    {
        uint32_t length, type, stored_crc, crc;
        if (info->chunks >= TL4_PNG_MAX_CHUNKS) return TL4_E_LIMIT;
        if (!rd32be(data, size, off, &length) || !rd32be(data, size, off + 4, &type)) return TL4_E_PNG_CHUNK;
        if (length > 0x7FFFFFFFu) return TL4_E_PNG_CHUNK;                 /* PNG: lengths below 2^31 */
        if (!in_range(size, off + 8, (uint64_t)length + 4)) return TL4_E_PNG_CHUNK;
        if (!rd32be(data, size, off + 8 + length, &stored_crc)) return TL4_E_PNG_CHUNK;
        crc = crc32_update(0xFFFFFFFFu, data + off + 4, 4u + length) ^ 0xFFFFFFFFu;
        if (crc != stored_crc) return TL4_E_PNG_CRC;
        info->chunks++;

        if (!seen_ihdr)
        {
            /* IHDR first (R-RES-3), 13 bytes, the accepted set, the limits of section 6 (R-RES-4) */
            uint32_t w, h;
            const uint8_t *p = data + off + 8;
            if (type != T_IHDR || length != 13) return TL4_E_PNG_IHDR;
            w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            h = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
            if (p[8] != 8 || (p[9] != 2 && p[9] != 6) || p[10] != 0 || p[11] != 0 || p[12] != 0) return TL4_E_PNG_IHDR;
            if (w == 0 || h == 0 || w > TL4_PNG_MAX_SIDE || h > TL4_PNG_MAX_SIDE) return TL4_E_LIMIT;
            if ((uint64_t)w * h > TL4_PNG_MAX_PIXELS) return TL4_E_LIMIT;
            info->width = w; info->height = h; info->pixels = (uint64_t)w * h;
            info->bit_depth = p[8]; info->colour_type = p[9]; info->interlace = p[12];
            seen_ihdr = 1;
        }
        else if (type == T_IDAT)
        {
            if (seen_idat && !idat_open) return TL4_E_PNG_CHUNK;          /* IDAT chunks are contiguous */
            if (info->idat_chunks >= TL4_PNG_MAX_IDAT_CHUNKS) return TL4_E_LIMIT;
            idat_total += length;
            if (idat_total > TL4_PNG_MAX_COMPRESSED) return TL4_E_LIMIT;
            info->idat_chunks++;
            info->idat_bytes = (uint32_t)idat_total;
            seen_idat = 1; idat_open = 1;
            if (idat_off) { idat_off[info->idat_chunks - 1] = (uint32_t)(off + 8); idat_len[info->idat_chunks - 1] = length; }
        }
        else
        {
            idat_open = 0;
            if (type == T_IEND)
            {
                if (!seen_idat || idat_total == 0 || length != 0) return TL4_E_PNG_CHUNK;   /* some compressed data, an empty IEND */
                seen_iend = 1;
            }
            else if (type == T_IHDR)
                return TL4_E_PNG_CHUNK;                                   /* a second header */
            else
            {
                uint32_t i, n = (uint32_t)(sizeof(allowed_ancillary) / sizeof(allowed_ancillary[0]));
                for (i = 0; i < n; i++) if (allowed_ancillary[i].type == type) break;
                if (i == n) return TL4_E_PNG_CHUNK;                       /* any other type, PLTE and tRNS included */
                if (seen_idat || (seen_ancillary & (1u << i))) return TL4_E_PNG_CHUNK;   /* once, before IDAT */
                if (length < allowed_ancillary[i].min_len || length > allowed_ancillary[i].max_len) return TL4_E_PNG_CHUNK;
                if (!allowed_ancillary[i].shape(data + off + 8, length)) return TL4_E_PNG_CHUNK;   /* the value range */
                seen_ancillary |= 1u << i;
                if (type == T_GAMA) info->has_gama = 1;
                else if (type == T_PHYS) info->has_phys = 1;
                else if (type == T_SRGB) info->has_srgb = 1;
                else info->has_text = 1;
            }
        }
        off += 12u + length;
    }
    if (off != size) return TL4_E_PNG_CHUNK;                              /* nothing after IEND */
    return TL4_OK;
}

tl4_status tl4_png_check(const uint8_t *data, uint32_t size, tl4_png_info *info)
{
    tl4_status st;
    if (!info) return TL4_E_ARG;
    clear_info(info);
    st = walk(data, size, info, 0, 0);
    if (st != TL4_OK) clear_info(info);
    return st;
}

tl4_status tl4_png_idat_enum(const uint8_t *data, uint32_t size, tl4_png_idat_cb cb, void *context, int *stopped)
{
    tl4_png_info scratch;
    uint32_t off[TL4_PNG_MAX_IDAT_CHUNKS], len[TL4_PNG_MAX_IDAT_CHUNKS], i;
    tl4_status st;
    if (stopped) *stopped = 0;
    if (!cb) return TL4_E_ARG;
    clear_info(&scratch);
    st = walk(data, size, &scratch, off, len);
    if (st != TL4_OK) return st;                                          /* no range of a rejected image */
    for (i = 0; i < scratch.idat_chunks; i++)
    {
        int r = cb(off[i], len[i], context);
        if (r) { if (stopped) *stopped = r; break; }                          /* a stop is harmless: the check is done */
    }
    return TL4_OK;
}

tl4_status tl4_pixel_budget_take(tl4_pixel_budget *budget, uint64_t pixels)
{
    if (!budget) return TL4_E_ARG;
    if (budget->used > budget->limit) return TL4_E_LIMIT;                 /* an inconsistent instance never grants */
    if (pixels > budget->limit - budget->used) return TL4_E_LIMIT;
    budget->used += pixels;
    return TL4_OK;
}

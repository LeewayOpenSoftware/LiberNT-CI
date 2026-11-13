/*
 * PROJECT:     LiberNT NTFS library host tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Spec model of the on-disk decoders: byte reads only, no library structures
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "decode_diff.h"

static uint16_t
Le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
Le32(const uint8_t *p)
{
    return (uint32_t)Le16(p) | ((uint32_t)Le16(p + 2) << 16);
}

static uint64_t
Le64(const uint8_t *p)
{
    return (uint64_t)Le32(p) | ((uint64_t)Le32(p + 4) << 32);
}

static uint64_t
LeN(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int
IsNegative(uint64_t v, unsigned n)
{
    return n != 0 && ((v >> (8 * n - 1)) & 1);
}

static int32_t
ModelMappingPairs(const uint8_t *a, uint32_t avail, uint64_t clusters, ntfs_diff_run_sink sink, void *context)
{
    uint32_t length, header, pos, offset;
    uint64_t previous = 0, total = 0, first, last;

    if (!a || avail < 0x40)
        return NTFS_DIFF_FILE_CORRUPT;
    if (a[8] == 0)
        return NTFS_DIFF_INVALID_PARAMETER;

    length = Le32(a + 4);
    header = (Le16(a + 0x0C) & 0x80FF) ? 0x48 : 0x40;
    offset = Le16(a + 0x20);
    if (length > avail || length < header || offset < header || offset >= length)
        return NTFS_DIFF_FILE_CORRUPT;

    for (pos = offset; pos < length && a[pos] != 0;)
    {
        unsigned ls = a[pos] & 0x0F, os = a[pos] >> 4;
        uint64_t run, lcn = 0;
        int32_t status;

        if (ls == 0 || ls > 8 || os > 8 || length - pos < 1 + ls + os)
            return NTFS_DIFF_FILE_CORRUPT;

        run = LeN(a + pos + 1, ls);
        if (run == 0 || IsNegative(run, ls) || total + run < total)
            return NTFS_DIFF_FILE_CORRUPT;

        if (os != 0)
        {
            uint64_t raw = LeN(a + pos + 1 + ls, os);

            if (IsNegative(raw, os))
            {
                uint64_t back = (os == 8 ? 0 : (~(uint64_t)0 << (8 * os))) | raw;

                back = ~back + 1;
                if (previous < back)
                    return NTFS_DIFF_FILE_CORRUPT;
                lcn = previous - back;
            }
            else
            {
                if (previous + raw < previous)
                    return NTFS_DIFF_FILE_CORRUPT;
                lcn = previous + raw;
            }

            if (lcn >= clusters || run > clusters - lcn)
                return NTFS_DIFF_FILE_CORRUPT;
            previous = lcn;
        }

        if (sink)
        {
            status = sink(context, lcn, run, os == 0);
            if (status < 0)
                return status;
        }

        total += run;
        pos += 1 + ls + os;
    }

    if (pos >= length)
        return NTFS_DIFF_FILE_CORRUPT;

    first = Le64(a + 0x10);
    last = Le64(a + 0x18);
    if (last < first || total != last - first + 1)
        return NTFS_DIFF_FILE_CORRUPT;

    return NTFS_DIFF_SUCCESS;
}

static int32_t
ModelApplyFixup(uint8_t *r, uint32_t size, uint32_t bps)
{
    uint32_t usa, count, i;
    uint16_t check;

    if (bps < 512 || (bps & (bps - 1)) != 0)
        return NTFS_DIFF_INVALID_PARAMETER;
    if (size < bps || size % bps != 0)
        return NTFS_DIFF_FILE_CORRUPT;

    usa = Le16(r + 4);
    count = Le16(r + 6);
    if (count != size / bps + 1 || usa > size || count * 2u > size - usa)
        return NTFS_DIFF_FILE_CORRUPT;

    for (i = 1; i < count; i++)
    {
        uint8_t *tail = r + i * bps - 2;

        check = Le16(r + usa);
        if (Le16(tail) != check)
            return NTFS_DIFF_FILE_CORRUPT;
        tail[0] = r[usa + 2 * i];
        tail[1] = r[usa + 2 * i + 1];
    }

    return NTFS_DIFF_SUCCESS;
}

static int
ModelIndexEntryValid(const uint8_t *e, uint32_t remaining)
{
    uint32_t entry, stream, flags, payload;

    if (remaining < 16)
        return 0;

    entry = Le16(e + 8);
    stream = Le16(e + 10);
    flags = e[12];
    if (entry < 16 || (entry & 7) || entry > remaining || (flags & ~3u))
        return 0;

    payload = entry - 16;
    if (flags & 1)
    {
        if (payload < 8)
            return 0;
        payload -= 8;
    }
    if (stream > payload)
        return 0;

    if (flags & 2)
        return stream == 0;

    if (stream < 0x42)
        return 0;
    return 2u * e[16 + 0x40] <= stream - 0x42;
}

static const ntfs_decode_impl Model =
{
    "model",
    ModelMappingPairs,
    ModelApplyFixup,
    ModelIndexEntryValid,
};

const ntfs_decode_impl *
ntfs_model_impl(void)
{
    return &Model;
}

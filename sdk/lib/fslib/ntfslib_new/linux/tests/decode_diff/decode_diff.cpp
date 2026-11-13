/*
 * PROJECT:     LiberNT NTFS library host tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Differential test of the on-disk decoders: every implementation
 *              linked in must agree on a harvested corpus and on generated input
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Usage: ntfslib_decode_diff [iterations] [--seed N] [--corpus DIR]
 *                            [--write-corpus DIR] [--dump DIR]
 * Corpus files: mp-*.bin (8-byte LE cluster count + attribute record),
 * fx-*.bin (4-byte LE bytes per sector + record), ie-*.bin (entry area).
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>

#include <ntfslib_new.h>
#include "ntfsformat.h"

#include "decode_diff.h"

namespace
{

enum Kind { KindMappingPairs, KindFixup, KindIndexEntry, KindCount };
const char *const KindName[KindCount] = { "mp", "fx", "ie" };

struct Case
{
    Kind Type;
    std::vector<uint8_t> Bytes;
    uint64_t Parameter;
    std::string Origin;
};

struct Run
{
    uint64_t Lcn, Length;
    uint8_t Sparse;
    bool operator==(const Run &o) const { return Lcn == o.Lcn && Length == o.Length && Sparse == o.Sparse; }
};

struct Outcome
{
    int32_t Status = 0;
    std::vector<Run> Runs;
    std::vector<uint8_t> Bytes;
    bool operator==(const Outcome &o) const
    {
        return Status == o.Status && (Status < 0 || (Runs == o.Runs && Bytes == o.Bytes));
    }
};

const size_t MaximumRuns = 4096;

std::vector<const ntfs_decode_impl *> Impls;
unsigned long Mismatches, Checked[KindCount], Accepted[KindCount];
std::string DumpDirectory;
uint64_t Seed = 0x243F6A8885A308D3ull;

uint64_t
Next()
{
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    return Seed;
}

uint64_t
Below(uint64_t n)
{
    return n ? Next() % n : 0;
}

uint16_t Le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t *p) { return Le16(p) | ((uint32_t)Le16(p + 2) << 16); }
uint64_t Le64(const uint8_t *p) { return Le32(p) | ((uint64_t)Le32(p + 4) << 32); }

void
Put(std::vector<uint8_t> &b, size_t at, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        b[at + i] = (uint8_t)(v >> (8 * i));
}

int32_t
CollectRun(void *context, uint64_t lcn, uint64_t length, uint8_t sparse)
{
    std::vector<Run> *runs = static_cast<std::vector<Run> *>(context);

    if (runs->size() >= MaximumRuns)
        return NTFS_DIFF_NO_MEMORY;
    runs->push_back(Run{ lcn, length, sparse });
    return NTFS_DIFF_SUCCESS;
}

bool
Supports(const ntfs_decode_impl *impl, Kind type)
{
    return type == KindMappingPairs ? impl->decode_mapping_pairs != nullptr
         : type == KindFixup ? impl->apply_fixup != nullptr
         : impl->index_entry_valid != nullptr;
}

Outcome
Evaluate(const ntfs_decode_impl *impl, const Case &c)
{
    size_t size = c.Bytes.size();
    uint8_t *exact = static_cast<uint8_t *>(std::malloc(size ? size : 1));
    Outcome out;

    if (!exact)
        std::abort();
    if (size)
        std::memcpy(exact, c.Bytes.data(), size);

    switch (c.Type)
    {
        case KindMappingPairs:
            out.Status = impl->decode_mapping_pairs(exact, (uint32_t)size, c.Parameter, CollectRun, &out.Runs);
            break;
        case KindFixup:
            out.Status = impl->apply_fixup(exact, (uint32_t)size, (uint32_t)c.Parameter);
            out.Bytes.assign(exact, exact + size);
            break;
        default:
            out.Status = impl->index_entry_valid(exact, (uint32_t)size) ? NTFS_DIFF_SUCCESS : NTFS_DIFF_FILE_CORRUPT;
            break;
    }
    std::free(exact);
    return out;
}

void
Save(const std::string &directory, const Case &c, unsigned long index)
{
    char name[64];
    std::vector<uint8_t> file(c.Type == KindFixup ? 4 : c.Type == KindMappingPairs ? 8 : 0);
    FILE *f;

    if (c.Type != KindIndexEntry)
        Put(file, 0, c.Parameter, (unsigned)file.size());
    file.insert(file.end(), c.Bytes.begin(), c.Bytes.end());
    std::snprintf(name, sizeof(name), "/%s-%06lu.bin", KindName[c.Type], index);
    f = std::fopen((directory + name).c_str(), "wb");
    if (!f)
        return;
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
}

void
Describe(const char *who, const Outcome &o)
{
    std::fprintf(stderr, "  %-8s status 0x%08X runs %zu", who, (unsigned)o.Status, o.Runs.size());
    for (size_t i = 0; i < o.Runs.size() && i < 4; i++)
        std::fprintf(stderr, " [%llu+%llu%s]", (unsigned long long)o.Runs[i].Lcn,
                     (unsigned long long)o.Runs[i].Length, o.Runs[i].Sparse ? " sparse" : "");
    std::fprintf(stderr, "\n");
}

void
Compare(const Case &c)
{
    std::vector<std::pair<const ntfs_decode_impl *, Outcome>> results;

    for (const ntfs_decode_impl *impl : Impls)
    {
        if (Supports(impl, c.Type))
            results.emplace_back(impl, Evaluate(impl, c));
    }
    if (results.empty())
        return;

    Checked[c.Type]++;
    if (results[0].second.Status >= 0)
        Accepted[c.Type]++;

    for (size_t i = 1; i < results.size(); i++)
    {
        if (results[i].second == results[0].second)
            continue;

        if (++Mismatches <= 20)
        {
            std::fprintf(stderr, "MISMATCH %s case from %s (%zu bytes, parameter %llu)\n", KindName[c.Type],
                         c.Origin.c_str(), c.Bytes.size(), (unsigned long long)c.Parameter);
            Describe(results[0].first->name, results[0].second);
            Describe(results[i].first->name, results[i].second);
            if (!DumpDirectory.empty())
                Save(DumpDirectory, c, Mismatches);
        }
        break;
    }
}

/* Corpus harvested from freshly formatted volumes: real records, attributes and entries */

struct Image
{
    std::vector<uint8_t> Bytes;
};

NTSTATUS
ImageWrite(void *context, ULONGLONG offset, ULONG length, const void *buffer)
{
    Image *image = static_cast<Image *>(context);

    if (offset + length > image->Bytes.size())
        return (NTSTATUS)0xC0000001;
    std::memcpy(image->Bytes.data() + offset, buffer, length);
    return 0;
}

void *ImageAllocate(void *, ULONG length) { return std::malloc(length); }
void ImageFree(void *, void *buffer) { std::free(buffer); }

void
HarvestEntries(std::vector<Case> &corpus, const uint8_t *area, size_t length, const std::string &origin)
{
    size_t pos = 0;

    while (pos + 16 <= length)
    {
        Case c{ KindIndexEntry, std::vector<uint8_t>(area + pos, area + length), 0, origin };
        uint16_t size = Le16(area + pos + 8);

        corpus.push_back(c);
        if ((area[pos + 12] & 2) || size < 16 || pos + size > length)
            break;
        pos += size;
    }
}

void
HarvestVolume(std::vector<Case> &corpus, uint32_t bps, uint32_t spc, uint64_t size)
{
    Image image;
    NtfsFormatParameters p;

    image.Bytes.assign(size, 0);
    std::memset(&p, 0, sizeof(p));
    p.TotalSectors = size / bps;
    p.BytesPerSector = bps;
    p.SectorsPerCluster = spc;
    p.MftRecordSize = bps > 1024 ? bps : 0;
    p.SectorsPerTrack = 63;
    p.NumberOfHeads = 255;
    p.MediaDescriptor = 0xF8;
    p.SerialNumber = 0x1234567890ABCDEFull;
    p.CurrentTime = 133000000000000000ull;
    p.QuickFormat = TRUE;
    p.IoContext = &image;
    p.Write = ImageWrite;
    p.Allocate = ImageAllocate;
    p.Free = ImageFree;
    if (NtfsVolumeFormat(&p) < 0)
    {
        std::fprintf(stderr, "format %u/%u failed\n", bps, spc);
        std::exit(1);
    }

    const uint8_t *boot = image.Bytes.data();
    uint64_t cluster = (uint64_t)Le16(boot + 0x0B) * boot[0x0D];
    uint64_t clusters = Le64(boot + 0x28) / boot[0x0D];
    int8_t mftScale = (int8_t)boot[0x40], indexScale = (int8_t)boot[0x44];
    uint64_t recordSize = mftScale > 0 ? mftScale * cluster : 1ull << -mftScale;
    uint64_t indexSize = indexScale > 0 ? indexScale * cluster : 1ull << -indexScale;
    uint64_t mft = Le64(boot + 0x30) * cluster;
    char origin[64];

    for (unsigned r = 0; r < NTFS_FORMAT_INITIAL_RECORDS && mft + (r + 1) * recordSize <= size; r++)
    {
        std::vector<uint8_t> record(image.Bytes.begin() + mft + r * recordSize,
                                    image.Bytes.begin() + mft + (r + 1) * recordSize);

        if (std::memcmp(record.data(), "FILE", 4) != 0)
            continue;
        std::snprintf(origin, sizeof(origin), "format %u/%u record %u", bps, spc, r);
        corpus.push_back(Case{ KindFixup, record, bps, origin });
        if (ntfs_model_impl()->apply_fixup(record.data(), (uint32_t)recordSize, bps) < 0)
            continue;

        for (size_t at = Le16(record.data() + 0x14); at + 16 <= recordSize;)
        {
            const uint8_t *attr = record.data() + at;
            uint32_t type = Le32(attr), length = Le32(attr + 4);

            if (type == 0xFFFFFFFF || length < 16 || at + length > recordSize)
                break;

            if (attr[8])
            {
                corpus.push_back(Case{ KindMappingPairs, std::vector<uint8_t>(attr, attr + length), clusters, origin });

                std::vector<Run> runs;
                if (type == 0xA0 &&
                    ntfs_model_impl()->decode_mapping_pairs(attr, length, clusters, CollectRun, &runs) >= 0)
                {
                    for (const Run &run : runs)
                    {
                        for (uint64_t b = 0; !run.Sparse && b * indexSize < run.Length * cluster; b++)
                        {
                            uint64_t from = run.Lcn * cluster + b * indexSize;
                            std::vector<uint8_t> block(image.Bytes.begin() + from,
                                                       image.Bytes.begin() + from + indexSize);

                            if (std::memcmp(block.data(), "INDX", 4) != 0)
                                continue;
                            corpus.push_back(Case{ KindFixup, block, bps, std::string(origin) + " INDX" });
                            if (ntfs_model_impl()->apply_fixup(block.data(), (uint32_t)indexSize, bps) < 0)
                                continue;
                            uint32_t first = Le32(block.data() + 0x18), end = Le32(block.data() + 0x1C);
                            if (0x18 + (uint64_t)end <= indexSize && first < end)
                                HarvestEntries(corpus, block.data() + 0x18 + first, end - first,
                                               std::string(origin) + " INDX");
                        }
                    }
                }
            }
            else if (type == 0x90 && attr[9] == 4 && Le16(attr + 0x0A) == 0x18 &&
                     Le16(attr + 0x14) + 32u <= length)
            {
                const uint8_t *header = attr + Le16(attr + 0x14) + 16;
                uint32_t first = Le32(header), end = Le32(header + 4);

                if (header + end <= attr + length && first < end)
                    HarvestEntries(corpus, header + first, end - first, std::string(origin) + " $I30 root");
            }
            at += length;
        }
    }
}

/* Synthetic cases: hand-picked edges, then generated input */

std::vector<uint8_t>
Attribute(uint16_t flags, uint64_t firstVcn, uint64_t lastVcn, const std::vector<uint8_t> &pairs, uint32_t slack = 0,
          bool exact = false)
{
    uint32_t header = (flags & 0x80FF) ? 0x48 : 0x40;
    uint32_t length = exact ? (uint32_t)(header + pairs.size()) : (uint32_t)((header + pairs.size() + slack + 7) & ~7u);
    std::vector<uint8_t> a(length, 0);

    Put(a, 0, 0x80, 4);
    Put(a, 4, length, 4);
    a[8] = 1;
    Put(a, 0x0C, flags, 2);
    Put(a, 0x10, firstVcn, 8);
    Put(a, 0x18, lastVcn, 8);
    Put(a, 0x20, header, 2);
    std::copy(pairs.begin(), pairs.end(), a.begin() + header);
    return a;
}

void
EncodeRun(std::vector<uint8_t> &out, uint64_t length, bool sparse, int64_t delta, bool padded)
{
    auto width = [&](uint64_t v, bool isSigned) {
        unsigned n = 1;
        while (n < 8)
        {
            int64_t s = (int64_t)(v << (64 - 8 * n)) >> (64 - 8 * n);
            uint64_t u = v & ((1ull << (8 * n)) - 1);
            if (isSigned ? (uint64_t)s == v : (u == v && !((v >> (8 * n - 1)) & 1)))
                break;
            n++;
        }
        return padded && n < 8 ? n + 1 : n;
    };
    unsigned ls = width(length, false), os = sparse ? 0 : width((uint64_t)delta, true);

    out.push_back((uint8_t)(ls | (os << 4)));
    for (unsigned i = 0; i < ls; i++)
        out.push_back((uint8_t)(length >> (8 * i)));
    for (unsigned i = 0; i < os; i++)
        out.push_back((uint8_t)((uint64_t)delta >> (8 * i)));
}

std::vector<uint8_t>
ValidMapping(uint64_t clusters, uint64_t &vcns, uint16_t &flags)
{
    std::vector<uint8_t> pairs;
    uint64_t lcn = 0;
    unsigned n = 1 + (unsigned)Below(12);

    vcns = 0;
    flags = Below(4) == 0 ? 0x8000 : Below(6) == 0 ? 0x0001 : 0;
    for (unsigned i = 0; i < n; i++)
    {
        bool sparse = Below(5) == 0;
        uint64_t target = Below(clusters - 1);
        uint64_t length = 1 + Below(std::min<uint64_t>(clusters - target, Below(3) ? 64 : clusters));

        if (Below(16) == 0 && clusters - target > 0xFF)
        {
            uint64_t raw = 0x80 + Below(0x80);
            int64_t delta = (int64_t)(target - lcn);

            pairs.push_back(0x81);
            pairs.push_back((uint8_t)raw);
            for (int b = 0; b < 8; b++)
                pairs.push_back((uint8_t)((uint64_t)delta >> (8 * b)));
            lcn = target;
            vcns += raw;
            continue;
        }
        EncodeRun(pairs, length, sparse, (int64_t)(target - lcn), Below(6) == 0);
        if (!sparse)
            lcn = target;
        vcns += length;
    }
    pairs.push_back(0);
    return pairs;
}

void
Mutate(std::vector<uint8_t> &b)
{
    if (b.empty())
        return;
    switch (Below(6))
    {
        case 0: b[Below(b.size())] ^= (uint8_t)(1u << Below(8)); break;
        case 1: b[Below(b.size())] = (uint8_t)Next(); break;
        case 2: b.resize(Below(b.size() + 1)); break;
        case 3: { size_t at = Below(b.size()); b[at] = (uint8_t)(Below(2) ? 0x00 : 0xFF); break; }
        case 4: b.insert(b.begin() + Below(b.size()), (uint8_t)Next()); break;
        default: for (int i = 0; i < 4; i++) b[Below(b.size())] = (uint8_t)Next(); break;
    }
}

void
MappingPairEdges(std::vector<Case> &out)
{
    const uint64_t big = 1000000;
    struct { const char *name; uint16_t flags; uint64_t first, last; std::vector<uint8_t> pairs; } edges[] =
    {
        { "one run", 0, 0, 9, { 0x11, 10, 20, 0 } },
        { "sparse run", 0x8000, 0, 9, { 0x01, 10, 0 } },
        { "back to cluster 0", 0, 0, 19, { 0x11, 10, 20, 0x11, 10, 0xEC, 0 } },
        { "back past cluster 0", 0, 0, 19, { 0x11, 10, 20, 0x11, 10, 0xEB, 0 } },
        { "negative length", 0, 0, 9, { 0x11, 0xF6, 20, 0 } },
        { "zero length", 0, 0, 0, { 0x11, 0, 20, 0 } },
        { "length size 9", 0, 0, 9, { 0x19, 10, 0, 0, 0, 0, 0, 0, 0, 0, 20, 0 } },
        { "offset size 9", 0, 0, 9, { 0x91, 10, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { "8-byte fields", 0, 0, 9, { 0x88, 10, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { "8-byte negative delta", 0, 0, 9, { 0x81, 10, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0 } },
        { "run past the volume", 0, 0, 9, { 0x31, 10, 0x40, 0x42, 0x0F, 0 } },
        { "no terminator", 0, 0, 9, { 0x11, 10, 20 } },
        { "VCN count mismatch", 0, 0, 10, { 0x11, 10, 20, 0 } },
        { "last before first", 0, 5, 4, { 0, 0 } },
        { "empty, last VCN -1", 0, 0, ~0ull, { 0 } },
        { "VCN range wraps", 0, 1, 0, { 0 } },
    };

    for (auto &e : edges)
    {
        out.push_back(Case{ KindMappingPairs, Attribute(e.flags, e.first, e.last, e.pairs), big, e.name });
    }

    std::vector<uint8_t> a = Attribute(0, 0, 9, { 0x11, 10, 20, 0 });
    Put(a, 4, a.size() + 8, 4);
    out.push_back(Case{ KindMappingPairs, a, big, "length over the buffer" });
    a = Attribute(0, 0, 9, { 0x11, 10, 20, 0 });
    Put(a, 0x20, a.size(), 2);
    out.push_back(Case{ KindMappingPairs, a, big, "pairs offset at the end" });
    a = Attribute(0x0001, 0, 9, { 0x11, 10, 20, 0 });
    Put(a, 0x20, 0x40, 2);
    out.push_back(Case{ KindMappingPairs, a, big, "compressed, pairs inside the header" });
    a = Attribute(0, 0, 9, { 0x11, 10, 20, 0 });
    a[8] = 0;
    out.push_back(Case{ KindMappingPairs, a, big, "resident" });
    out.push_back(Case{ KindMappingPairs, std::vector<uint8_t>(a.begin(), a.begin() + 0x3F), big, "short header" });
}

Case
GenerateMappingPairs(const std::vector<Case> &seeds)
{
    uint64_t clusters = Below(4) ? 1 + Below(1u << 20) : 1 + (Next() >> Below(64));
    uint64_t vcns;
    uint16_t flags;
    Case c{ KindMappingPairs, {}, clusters, "generated" };

    if (!seeds.empty() && Below(4) == 0)
    {
        c = seeds[Below(seeds.size())];
        c.Origin = "mutated " + c.Origin;
    }
    else
    {
        if (clusters < 2)
            clusters = c.Parameter = 2;
        std::vector<uint8_t> pairs = ValidMapping(clusters, vcns, flags);
        uint64_t first = Below(4) ? 0 : Below(1000);
        bool unterminated = Below(8) == 0;
        if (unterminated)
            pairs.pop_back();
        c.Bytes = Attribute(flags, first, first + vcns - 1, pairs, (uint32_t)Below(3) * 8, unterminated);
    }

    for (unsigned m = (unsigned)Below(3); m; m--)
        Mutate(c.Bytes);
    if (Below(8) == 0)
        c.Parameter = 1 + Below(c.Parameter);
    return c;
}

void
CommitFixup(std::vector<uint8_t> &r, uint32_t bps, uint16_t usn)
{
    uint32_t count = (uint32_t)(r.size() / bps + 1), usa = 0x30;

    Put(r, 4, usa, 2);
    Put(r, 6, count, 2);
    Put(r, usa, usn, 2);
    for (uint32_t i = 1; i < count; i++)
    {
        r[usa + 2 * i] = r[i * bps - 2];
        r[usa + 2 * i + 1] = r[i * bps - 1];
        Put(r, i * bps - 2, usn, 2);
    }
}

Case
GenerateFixup(const std::vector<Case> &seeds)
{
    Case c{ KindFixup, {}, Below(2) ? 512u : 4096u, "generated" };

    if (!seeds.empty() && Below(3) == 0)
    {
        c = seeds[Below(seeds.size())];
        c.Origin = "mutated " + c.Origin;
    }
    else
    {
        uint32_t sectors = 1 + (uint32_t)Below(c.Parameter == 512 ? 8 : 2);
        c.Bytes.resize(sectors * c.Parameter);
        for (auto &byte : c.Bytes)
            byte = (uint8_t)Next();
        std::memcpy(c.Bytes.data(), Below(2) ? "FILE" : "INDX", 4);
        CommitFixup(c.Bytes, (uint32_t)c.Parameter, (uint16_t)Next());
    }

    switch (Below(8))
    {
        case 0: Put(c.Bytes, 4, Below(c.Bytes.size() + 4), 2); break;
        case 1: Put(c.Bytes, 6, Below(20), 2); break;
        case 2: c.Bytes[(1 + Below(c.Bytes.size() / c.Parameter)) * c.Parameter - 1 - Below(2)] ^= 0x5A; break;
        case 3: Put(c.Bytes, 4, c.Bytes.size() - 2 * Le16(c.Bytes.data() + 6) + Below(4), 2); break;
        case 4: Put(c.Bytes, 4, c.Parameter - 4, 2); break;
        case 5: Mutate(c.Bytes); break;
        default: break;
    }
    if (c.Bytes.size() < 8)
        c.Bytes.resize(8);
    return c;
}

Case
GenerateIndexEntry(const std::vector<Case> &seeds)
{
    Case c{ KindIndexEntry, {}, 0, "generated" };

    if (!seeds.empty() && Below(3) == 0)
    {
        c = seeds[Below(seeds.size())];
        c.Origin = "mutated " + c.Origin;
    }
    else
    {
        uint8_t flags = (uint8_t)Below(4);
        uint32_t name = (uint32_t)Below(40);
        uint32_t stream = (flags & 2) ? 0 : 0x42 + 2 * name;
        uint32_t length = (16 + stream + ((flags & 1) ? 8 : 0) + 7) & ~7u;

        c.Bytes.assign(length + Below(64), 0);
        for (auto &byte : c.Bytes)
            byte = (uint8_t)Next();
        Put(c.Bytes, 8, length, 2);
        Put(c.Bytes, 10, stream, 2);
        c.Bytes[12] = flags;
        if (stream)
            c.Bytes[16 + 0x40] = (uint8_t)name;
    }

    switch (Below(8))
    {
        case 0: Put(c.Bytes, 8, Below(c.Bytes.size() + 16), 2); break;
        case 1: Put(c.Bytes, 10, Below(c.Bytes.size() + 16), 2); break;
        case 2: c.Bytes[12] = (uint8_t)Next(); break;
        case 3: if (c.Bytes.size() > 16 + 0x40) c.Bytes[16 + 0x40] = (uint8_t)Next(); break;
        case 4: c.Bytes.resize(Below(c.Bytes.size() + 1)); break;
        case 5: Mutate(c.Bytes); break;
        default: break;
    }
    return c;
}

bool
LoadCorpus(const std::string &directory, std::vector<Case> &out)
{
    DIR *dir = opendir(directory.c_str());
    struct dirent *entry;

    if (!dir)
        return false;
    while ((entry = readdir(dir)) != nullptr)
    {
        std::string name = entry->d_name;
        Kind type = KindCount;

        for (int k = 0; k < KindCount; k++)
        {
            if (name.compare(0, 3, std::string(KindName[k]) + "-") == 0)
                type = (Kind)k;
        }
        if (type == KindCount)
            continue;

        FILE *f = std::fopen((directory + "/" + name).c_str(), "rb");
        if (!f)
            continue;
        std::vector<uint8_t> bytes;
        int ch;
        while ((ch = std::fgetc(f)) != EOF)
            bytes.push_back((uint8_t)ch);
        std::fclose(f);

        size_t prefix = type == KindMappingPairs ? 8 : type == KindFixup ? 4 : 0;
        if (bytes.size() < prefix)
            continue;
        Case c{ type, std::vector<uint8_t>(bytes.begin() + prefix, bytes.end()), 0, name };
        for (size_t i = 0; i < prefix; i++)
            c.Parameter |= (uint64_t)bytes[i] << (8 * i);
        if (type == KindFixup && c.Parameter != 512 && c.Parameter != 4096)
            continue;
        out.push_back(c);
    }
    closedir(dir);
    return true;
}

}

#ifdef NTFSLIB_DIFF_HAVE_ALT
#define ALT_IMPL ntfs_alt_impl()
#else
#define ALT_IMPL nullptr
#endif

int
main(int argc, char **argv)
{
    unsigned long iterations = 100000;
    std::string corpusDirectory, writeDirectory;
    std::vector<Case> corpus, seeds[KindCount];

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc)
            Seed = std::strtoull(argv[++i], nullptr, 0) | 1;
        else if (arg == "--corpus" && i + 1 < argc)
            corpusDirectory = argv[++i];
        else if (arg == "--write-corpus" && i + 1 < argc)
            writeDirectory = argv[++i];
        else if (arg == "--dump" && i + 1 < argc)
            DumpDirectory = argv[++i];
        else
            iterations = std::strtoul(argv[i], nullptr, 0);
    }

    Impls.push_back(ntfs_lib_impl());
    Impls.push_back(ntfs_model_impl());
    if (ALT_IMPL)
        Impls.push_back(ALT_IMPL);

    HarvestVolume(corpus, 512, 8, 16ull << 20);
    HarvestVolume(corpus, 512, 1, 8ull << 20);
    HarvestVolume(corpus, 4096, 1, 32ull << 20);
    MappingPairEdges(corpus);
    if (!corpusDirectory.empty() && !LoadCorpus(corpusDirectory, corpus))
    {
        std::fprintf(stderr, "cannot read corpus %s\n", corpusDirectory.c_str());
        return 1;
    }

    for (const Case &c : corpus)
    {
        if (c.Type != KindIndexEntry || c.Bytes.size() >= 16)
            seeds[c.Type].push_back(c);
        Compare(c);
    }
    std::printf("corpus: %zu cases (mp %zu, fx %zu, ie %zu)\n", corpus.size(), seeds[KindMappingPairs].size(),
                seeds[KindFixup].size(), seeds[KindIndexEntry].size());
    if (!writeDirectory.empty())
    {
        for (size_t i = 0; i < corpus.size(); i++)
            Save(writeDirectory, corpus[i], (unsigned long)i);
    }

    for (unsigned long i = 0; i < iterations; i++)
    {
        Compare(GenerateMappingPairs(seeds[KindMappingPairs]));
        Compare(GenerateFixup(seeds[KindFixup]));
        Compare(GenerateIndexEntry(seeds[KindIndexEntry]));
    }

    int failed = Mismatches != 0;
    std::printf("implementations:");
    for (const ntfs_decode_impl *impl : Impls)
        std::printf(" %s", impl->name);
    std::printf("\n");
    for (int k = 0; k < KindCount; k++)
    {
        std::printf("%s: %lu checked, %lu accepted\n", KindName[k], Checked[k], Accepted[k]);
        if (Accepted[k] == 0 || Accepted[k] == Checked[k])
        {
            std::printf("FAIL %s: the inputs never exercise both outcomes\n", KindName[k]);
            failed = 1;
        }
    }
    std::printf("%lu mismatches\n", Mismatches);
    return failed;
}

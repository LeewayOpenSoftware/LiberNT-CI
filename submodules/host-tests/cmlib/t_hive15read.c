/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     cmlib read support for registry hives of format 1.4/1.5/1.6
 *              (minor versions 4, 5, 6): header acceptance, li/lf/lh/ri index
 *              cells, big data (db) values, and the new-format (HvLE)
 *              transaction log header; refusal of unknown major versions.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The fixtures are built from scratch in this file. The format facts come
 * from the public documentation "Windows registry file format specification"
 * by Maxim Suhanov (licensed CC BY 4.0, documentation only): minor versions
 * 3..6, hash leaves ("lh") for minor > 4, big data ("db") for values larger
 * than 16344 bytes when minor > 3, data segments of at most 16344 bytes,
 * XOR-32 base block checksum over the first 508 bytes with the -1/-2 and
 * 0/1 fixups, and the new-format log (File type 6 header sector followed by
 * HvLE entries whose Hash-1 covers the page references and page data and
 * whose Hash-2 covers the first 32 bytes of the entry, both Marvin32 with
 * the seed 82 EF 4D 88 7A 4E 55 C5).
 * The checksum, name hash and Marvin32 below are implemented locally so
 * fixture generation does not depend on the code under test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmlib.h>

/* Value types from the public format documentation (the host shim does not
 * carry winnt.h's REG_* defines). */
#ifndef REG_BINARY
#define REG_BINARY  0x00000003
#endif
#ifndef REG_DWORD
#define REG_DWORD   0x00000004
#endif

static unsigned long Checks, Failures;
#define CHECK(c) do { Checks++; if (!(c)) { Failures++; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ */
/* Local format helpers (independent implementations, see header note) */
/* ------------------------------------------------------------------ */

static
ULONG
THeaderChecksum508(
    _In_ const UCHAR *BaseBlock)
{
    ULONG sum = 0;
    ULONG i;

    for (i = 0; i < 508 / sizeof(ULONG); i++)
        sum ^= ((const ULONG *)BaseBlock)[i];
    if (sum == (ULONG)-1)
        sum = (ULONG)-2;
    if (sum == 0)
        sum = 1;
    return sum;
}

static
ULONG
TNameHash(
    _In_ PCWSTR Name,
    _In_ ULONG Chars)
{
    ULONG hash = 0, i;
    WCHAR c;

    for (i = 0; i < Chars; i++)
    {
        c = Name[i];
        if (c >= L'a' && c <= L'z')
            c = (WCHAR)(c - (L'a' - L'A'));
        hash = hash * 37 + c;
    }
    return hash;
}

#define T_MARVIN_SEED 0x82EF4D887A4E55C5ULL

static
VOID
TMarvinBlock(
    _Inout_ ULONG *Low,
    _Inout_ ULONG *High)
{
    *High ^= *Low;
    *Low = (*Low << 20) | (*Low >> 12);
    *Low += *High;
    *High = (*High << 9) | (*High >> 23);
    *High ^= *Low;
    *Low = (*Low << 27) | (*Low >> 5);
    *Low += *High;
    *High = (*High << 19) | (*High >> 13);
}

static
ULONGLONG
TMarvin64(
    _In_ const UCHAR *Data,
    _In_ ULONG Length)
{
    ULONG low = (ULONG)T_MARVIN_SEED;
    ULONG high = (ULONG)(T_MARVIN_SEED >> 32);
    ULONG final = 0x80;
    ULONGLONG result;

    while (Length >= sizeof(ULONG))
    {
        low += Data[0] | ((ULONG)Data[1] << 8) | ((ULONG)Data[2] << 16) |
               ((ULONG)Data[3] << 24);
        TMarvinBlock(&low, &high);
        Data += sizeof(ULONG);
        Length -= sizeof(ULONG);
    }
    while (Length)
        final = (final << 8) | Data[--Length];
    low += final;
    TMarvinBlock(&low, &high);
    TMarvinBlock(&low, &high);

    result = ((ULONGLONG)high << 32) | low;
    return result;
}

/* ------------------------------------------------------------------ */
/* Synthetic hive fixture                                              */
/* ------------------------------------------------------------------ */

#define T_CHILDREN      9
#define T_BINS_MAX      (HBLOCK_SIZE * 20)
#define T_MEDIUM_LEN    100
#define T_BIG_LEN       40000                     /* 16344 + 16344 + 7312 */
#define T_BIG_SEG_FULL  16344
#define T_LARGE_LEN     20000                     /* plain-cell value (1.3) */

#define T_BLK(c)        (((c) & HCELL_BLOCK_MASK) >> HCELL_BLOCK_SHIFT)

static const struct
{
    PCWSTR Name;
    ULONG NameChars;
} TChildNames[T_CHILDREN] =
{
    { L"Bravo", 5 }, { L"Charlie", 7 }, { L"Delta", 5 },
    { L"Echo", 4 }, { L"Foxtrot", 7 }, { L"Golf", 4 },
    { L"Hotel", 5 }, { L"India", 5 }, { L"Juliet", 6 },
};

typedef struct _T_FIX
{
    UCHAR Image[HBLOCK_SIZE + T_BINS_MAX];
    ULONG Minor;
    ULONG BinSize;
    HCELL_INDEX Root;
    HCELL_INDEX Sk;
    HCELL_INDEX Ri;
    HCELL_INDEX Leaf[3];
    HCELL_INDEX Child[T_CHILDREN];
    HCELL_INDEX ValList;
    HCELL_INDEX VkSmall;
    HCELL_INDEX VkMedium;
    HCELL_INDEX VkBig;
    HCELL_INDEX VkLarge;
    HCELL_INDEX MediumData;
    HCELL_INDEX BigDb;
    HCELL_INDEX BigSegList;
    HCELL_INDEX BigSeg[3];
    HCELL_INDEX LargeData;
    UCHAR MediumPattern[T_MEDIUM_LEN];
    UCHAR BigPattern[T_BIG_LEN];
    UCHAR LargePattern[T_LARGE_LEN];
} T_FIX;

static ULONG TCursor;

/* Append a cell (negative size header, 8-byte aligned total) to the bin. */
static
HCELL_INDEX
TPutCell(
    _Inout_ T_FIX *Fix,
    _In_ ULONG PayloadSize,
    _Out_ PVOID *Payload)
{
    ULONG total = ROUND_UP(PayloadSize + sizeof(HCELL), 8);
    HCELL_INDEX index = (HCELL_INDEX)TCursor;
    PHCELL cell;

    cell = (PHCELL)(Fix->Image + HBLOCK_SIZE + TCursor);
    cell->Size = -(LONG)total;
    *Payload = (PUCHAR)(cell + 1);
    TCursor += total;
    return index;
}

/* Build a hive with 9 sorted subkeys spread over lf/lh/li leaves under an
 * ri root, plus small, cell-resident, and (minor > 3) big data values. */
static
VOID
TFixBuild(
    _Inout_ T_FIX *Fix,
    _In_ ULONG Minor)
{
    PVOID payload;
    PHBASE_BLOCK base = (PHBASE_BLOCK)Fix->Image;
    PHBIN bin = (PHBIN)(Fix->Image + HBLOCK_SIZE);
    PCM_KEY_NODE root, node;
    PCM_KEY_FAST_INDEX fastLeaf;
    PCM_KEY_INDEX listLeaf, rootIndex;
    PCM_KEY_VALUE value;
    PCM_KEY_SECURITY security;
    PCM_BIG_DATA bigData;
    PHCELL_INDEX segList;
    PHCELL_INDEX valueList;
    ULONG i, group, segSizes[3], remaining;

    RtlZeroMemory(Fix, sizeof(*Fix));
    Fix->Minor = Minor;

    for (i = 0; i < T_MEDIUM_LEN; i++)
        Fix->MediumPattern[i] = (UCHAR)(0x5A ^ (i * 7));
    for (i = 0; i < T_BIG_LEN; i++)
        Fix->BigPattern[i] = (UCHAR)(i * 131 + 17);
    for (i = 0; i < T_LARGE_LEN; i++)
        Fix->LargePattern[i] = (UCHAR)(0xA5 ^ (i * 3));

    TCursor = sizeof(HBIN);

    /* Security cell. */
    Fix->Sk = TPutCell(Fix, FIELD_OFFSET(CM_KEY_SECURITY, Descriptor), &payload);
    security = (PCM_KEY_SECURITY)payload;
    security->Signature = CM_KEY_SECURITY_SIGNATURE;
    security->Flink = security->Blink = Fix->Sk;
    security->ReferenceCount = 1;
    security->DescriptorLength = 0;

    /* Root key node. */
    Fix->Root = TPutCell(Fix, FIELD_OFFSET(CM_KEY_NODE, Name) + 6, &payload);
    root = (PCM_KEY_NODE)payload;
    root->Signature = CM_KEY_NODE_SIGNATURE;
    root->Flags = KEY_HIVE_ENTRY | KEY_COMP_NAME;
    root->Parent = HCELL_NIL;
    root->SubKeyCounts[Volatile] = 0;
    root->SubKeyLists[Volatile] = HCELL_NIL;
    root->Security = Fix->Sk;
    root->Class = HCELL_NIL;
    root->MaxNameLen = 7 * sizeof(WCHAR);
    root->MaxClassLen = 0;
    root->MaxValueNameLen = 6 * sizeof(WCHAR);
    root->MaxValueDataLen = Minor > 3 ? T_BIG_LEN : T_LARGE_LEN;
    for (i = 0; i < 6; i++)
        ((PUCHAR)root->Name)[i] = (UCHAR)L"$$ROOT"[i];
    root->NameLength = 6;

    /* Nine child key nodes, names sorted as a whole across the leaves. */
    for (i = 0; i < T_CHILDREN; i++)
    {
        ULONG chars = TChildNames[i].NameChars;
        ULONG c;

        Fix->Child[i] = TPutCell(Fix,
                                 FIELD_OFFSET(CM_KEY_NODE, Name) + chars,
                                 &payload);
        node = (PCM_KEY_NODE)payload;
        node->Signature = CM_KEY_NODE_SIGNATURE;
        node->Flags = KEY_COMP_NAME;
        node->Parent = Fix->Root;
        node->SubKeyCounts[Stable] = 0;
        node->SubKeyCounts[Volatile] = 0;
        node->SubKeyLists[Stable] = HCELL_NIL;
        node->SubKeyLists[Volatile] = HCELL_NIL;
        node->ValueList.Count = 0;
        node->ValueList.List = HCELL_NIL;
        node->Security = Fix->Sk;
        node->Class = HCELL_NIL;
        for (c = 0; c < chars; c++)
            ((PUCHAR)node->Name)[c] = (UCHAR)TChildNames[i].Name[c];
        node->NameLength = (USHORT)chars;
    }

    /* Medium (plain cell) value data. */
    Fix->MediumData = TPutCell(Fix, T_MEDIUM_LEN, &payload);
    memcpy(payload, Fix->MediumPattern, T_MEDIUM_LEN);

    /* Big data value (only valid for minor > 3 hives). */
    if (Minor > 3)
    {
        segSizes[0] = T_BIG_SEG_FULL;
        segSizes[1] = T_BIG_SEG_FULL;
        segSizes[2] = T_BIG_LEN - 2 * T_BIG_SEG_FULL;

        for (i = 0; i < 3; i++)
        {
            Fix->BigSeg[i] = TPutCell(Fix, segSizes[i], &payload);
            memcpy(payload, Fix->BigPattern + i * T_BIG_SEG_FULL, segSizes[i]);
        }

        Fix->BigSegList = TPutCell(Fix, 3 * sizeof(HCELL_INDEX), &payload);
        segList = (PHCELL_INDEX)payload;
        for (i = 0; i < 3; i++)
            segList[i] = Fix->BigSeg[i];

        Fix->BigDb = TPutCell(Fix, sizeof(CM_BIG_DATA), &payload);
        bigData = (PCM_BIG_DATA)payload;
        bigData->Signature = CM_BIG_DATA_SIGNATURE;
        bigData->Count = 3;
        bigData->List = Fix->BigSegList;
    }
    else
    {
        /* A 1.3 hive stores a large value in a single plain cell. */
        Fix->LargeData = TPutCell(Fix, T_LARGE_LEN, &payload);
        memcpy(payload, Fix->LargePattern, T_LARGE_LEN);
    }

    /* Values: Small (inline), Medium (cell), Big/Large. */
    Fix->VkSmall = TPutCell(Fix, FIELD_OFFSET(CM_KEY_VALUE, Name) + 5, &payload);
    value = (PCM_KEY_VALUE)payload;
    value->Signature = CM_KEY_VALUE_SIGNATURE;
    value->NameLength = 5;
    value->DataLength = sizeof(ULONG) | CM_KEY_VALUE_SPECIAL_SIZE;
    value->Data = (HCELL_INDEX)0x12345678;
    value->Type = REG_DWORD;
    value->Flags = VALUE_COMP_NAME;
    memcpy(value->Name, "Small", 5);

    Fix->VkMedium = TPutCell(Fix, FIELD_OFFSET(CM_KEY_VALUE, Name) + 6, &payload);
    value = (PCM_KEY_VALUE)payload;
    value->Signature = CM_KEY_VALUE_SIGNATURE;
    value->NameLength = 6;
    value->DataLength = T_MEDIUM_LEN;
    value->Data = Fix->MediumData;
    value->Type = REG_BINARY;
    value->Flags = VALUE_COMP_NAME;
    memcpy(value->Name, "Medium", 6);

    if (Minor > 3)
    {
        Fix->VkBig = TPutCell(Fix, FIELD_OFFSET(CM_KEY_VALUE, Name) + 3, &payload);
        value = (PCM_KEY_VALUE)payload;
        value->Signature = CM_KEY_VALUE_SIGNATURE;
        value->NameLength = 3;
        value->DataLength = T_BIG_LEN;
        value->Data = Fix->BigDb;
        value->Type = REG_BINARY;
        value->Flags = VALUE_COMP_NAME;
        memcpy(value->Name, "Big", 3);
    }
    else
    {
        Fix->VkLarge = TPutCell(Fix, FIELD_OFFSET(CM_KEY_VALUE, Name) + 5, &payload);
        value = (PCM_KEY_VALUE)payload;
        value->Signature = CM_KEY_VALUE_SIGNATURE;
        value->NameLength = 5;
        value->DataLength = T_LARGE_LEN;
        value->Data = Fix->LargeData;
        value->Type = REG_BINARY;
        value->Flags = VALUE_COMP_NAME;
        memcpy(value->Name, "Large", 5);
    }

    /* Key values list. */
    Fix->ValList = TPutCell(Fix, 3 * sizeof(HCELL_INDEX), &payload);
    valueList = (PHCELL_INDEX)payload;
    valueList[0] = Fix->VkSmall;
    valueList[1] = Fix->VkMedium;
    valueList[2] = Minor > 3 ? Fix->VkBig : Fix->VkLarge;

    root->ValueList.Count = 3;
    root->ValueList.List = Fix->ValList;
    root->SubKeyCounts[Stable] = T_CHILDREN;

    /* Leaf cells: group 0 -> lf, group 1 -> lh for minor > 4 else lf,
     * group 2 -> li. */
    for (group = 0; group < 3; group++)
    {
        BOOLEAN hashed = (group == 1) && (Minor > 4);

        if (group < 2)
        {
            Fix->Leaf[group] = TPutCell(Fix,
                                        FIELD_OFFSET(CM_KEY_FAST_INDEX, List) +
                                        3 * sizeof(CM_INDEX),
                                        &payload);
            fastLeaf = (PCM_KEY_FAST_INDEX)payload;
            fastLeaf->Signature = hashed ? CM_KEY_HASH_LEAF : CM_KEY_FAST_LEAF;
            fastLeaf->Count = 3;
            for (i = 0; i < 3; i++)
            {
                ULONG child = group * 3 + i;

                fastLeaf->List[i].Cell = Fix->Child[child];
                if (hashed)
                {
                    fastLeaf->List[i].HashKey =
                        TNameHash(TChildNames[child].Name,
                                  TChildNames[child].NameChars);
                }
                else
                {
                    ULONG hint, chars = TChildNames[child].NameChars;

                    memset(fastLeaf->List[i].NameHint, 0, 4);
                    for (hint = 0; hint < 4 && hint < chars; hint++)
                    {
                        WCHAR c = TChildNames[child].Name[hint];

                        if (c >= L'a' && c <= L'z')
                            c = (WCHAR)(c - (L'a' - L'A'));
                        fastLeaf->List[i].NameHint[hint] = (UCHAR)(c & 0xFF);
                    }
                }
            }
        }
        else
        {
            Fix->Leaf[group] = TPutCell(Fix,
                                        FIELD_OFFSET(CM_KEY_INDEX, List) +
                                        3 * sizeof(HCELL_INDEX),
                                        &payload);
            listLeaf = (PCM_KEY_INDEX)payload;
            listLeaf->Signature = CM_KEY_INDEX_LEAF;
            listLeaf->Count = 3;
            for (i = 0; i < 3; i++)
                listLeaf->List[i] = Fix->Child[group * 3 + i];
        }
    }

    /* Root index over the three leaves. */
    Fix->Ri = TPutCell(Fix, FIELD_OFFSET(CM_KEY_INDEX, List) +
                       3 * sizeof(HCELL_INDEX), &payload);
    rootIndex = (PCM_KEY_INDEX)payload;
    rootIndex->Signature = CM_KEY_INDEX_ROOT;
    rootIndex->Count = 3;
    for (group = 0; group < 3; group++)
        rootIndex->List[group] = Fix->Leaf[group];
    root->SubKeyLists[Stable] = Fix->Ri;

    /* Close the bin with one free cell so bin validation covers all bytes. */
    Fix->BinSize = ROUND_UP(TCursor, HBLOCK_SIZE);
    remaining = Fix->BinSize - TCursor;
    if (remaining)
        ((PHCELL)(Fix->Image + HBLOCK_SIZE + TCursor))->Size = (LONG)remaining;

    /* Base block. */
    bin->Signature = HV_HBIN_SIGNATURE;
    bin->FileOffset = 0;
    bin->Size = Fix->BinSize;

    base->Signature = HV_HBLOCK_SIGNATURE;
    base->Sequence1 = 1;
    base->Sequence2 = 1;
    base->TimeStamp.QuadPart = 0;
    base->Major = HSYS_MAJOR;
    base->Minor = Minor;
    base->Type = HFILE_TYPE_PRIMARY;
    base->Format = HBASE_FORMAT_MEMORY;
    base->RootCell = Fix->Root;
    base->Length = Fix->BinSize;
    base->Cluster = 1;
    memcpy(base->FileName, L"t.hive", 6 * sizeof(WCHAR));
    base->CheckSum = THeaderChecksum508(Fix->Image);
}

/* ------------------------------------------------------------------ */
/* Test bodies                                                         */
/* ------------------------------------------------------------------ */

static PVOID NTAPI TAlloc(SIZE_T Size, BOOLEAN Paged, ULONG Tag)
{
    void *p = NULL;
    (void)Paged; (void)Tag;
#ifdef _WIN32
    p = _aligned_malloc((size_t)Size, 16);
#else
    if (posix_memalign(&p, 16, (size_t)Size)) return NULL;
#endif
    return p;
}

static VOID NTAPI TFree(PVOID Ptr, ULONG Quota)
{
    (void)Quota;
#ifdef _WIN32
    _aligned_free(Ptr);
#else
    free(Ptr);
#endif
}

static BOOLEAN NTAPI TNoRead(PHHIVE H, ULONG T, PULONG O, PVOID B, SIZE_T L)
{ (void)H;(void)T;(void)O;(void)B;(void)L; return FALSE; }
static BOOLEAN NTAPI TNoWrite(PHHIVE H, ULONG T, PULONG O, PVOID B, SIZE_T L)
{ (void)H;(void)T;(void)O;(void)B;(void)L; return TRUE; }

static
NTSTATUS
TLoadImage(
    _In_ PVOID Image,
    _Out_ CMHIVE *CmHive)
{
    PHHIVE hive = &CmHive->Hive;

    RtlZeroMemory(CmHive, sizeof(*CmHive));
    return HvInitialize(hive, HINIT_MEMORY, 0, HFILE_TYPE_PRIMARY,
                        Image, TAlloc, TFree, TNoWrite, TNoRead, 1, NULL);
}

static
VOID
TCheckFixtureContents(
    _In_ T_FIX *Fix,
    _In_ PHHIVE Hive)
{
    PCM_KEY_NODE root;
    HCELL_INDEX cell, found[T_CHILDREN];
    UNICODE_STRING name;
    ULONG i, j;

    CHECK(Hive->Version == Fix->Minor);

    root = (PCM_KEY_NODE)HvGetCell(Hive, Hive->BaseBlock->RootCell);
    CHECK(root != NULL);
    if (!root) return;
    CHECK(root->Signature == CM_KEY_NODE_SIGNATURE);
    CHECK(root->Flags & KEY_HIVE_ENTRY);
    CHECK(root->SubKeyCounts[Stable] == T_CHILDREN);

    /* Lookup every child by name through the ri -> lf/lh/li structure. */
    for (i = 0; i < T_CHILDREN; i++)
    {
        name.Buffer = (PWCHAR)TChildNames[i].Name;
        name.Length = (USHORT)(TChildNames[i].NameChars * sizeof(WCHAR));
        name.MaximumLength = name.Length;
        cell = CmpFindSubKeyByName(Hive, root, &name);
        CHECK(cell == Fix->Child[i]);
    }

    /* A name that does not exist must not resolve. */
    name.Buffer = (PWCHAR)L"Zulu";
    name.Length = 4 * sizeof(WCHAR);
    name.MaximumLength = name.Length;
    CHECK(CmpFindSubKeyByName(Hive, root, &name) == HCELL_NIL);

    /* Enumeration across the root index must visit all children in order. */
    RtlZeroMemory(found, sizeof(found));
    for (i = 0; i < T_CHILDREN; i++)
    {
        cell = CmpFindSubKeyByNumber(Hive, root, i);
        CHECK(cell != HCELL_NIL);
        if (cell != HCELL_NIL)
            found[i] = cell;
    }
    CHECK(CmpFindSubKeyByNumber(Hive, root, T_CHILDREN) == HCELL_NIL);
    for (i = 0; i < T_CHILDREN; i++)
    {
        for (j = 0; j < T_CHILDREN; j++)
            if (found[i] == Fix->Child[j])
                break;
        CHECK(j == i); /* globally sorted across the three leaves */
    }

    /* Values: by name and by data content. */
    {
        PCM_KEY_VALUE value;
        PVOID buffer;
        BOOLEAN allocated, ok;
        HCELL_INDEX release;
        ULONG length;

        name.Buffer = (PWCHAR)L"Small";
        name.Length = 5 * sizeof(WCHAR);
        name.MaximumLength = name.Length;
        cell = CmpFindValueByName(Hive, root, &name);
        CHECK(cell == Fix->VkSmall);
        if (cell != HCELL_NIL)
        {
            value = (PCM_KEY_VALUE)HvGetCell(Hive, cell);
            CHECK(value != NULL);
            if (value)
            {
                ok = CmpGetValueData(Hive, value, &length, &buffer,
                                     &allocated, &release);
                CHECK(ok == TRUE);
                CHECK(!allocated);
                if (ok)
                {
                    CHECK(length == sizeof(ULONG));
                    CHECK(*(PULONG)buffer == 0x12345678);
                }
                if (release != HCELL_NIL)
                    HvReleaseCell(Hive, release);
            }
        }

        name.Buffer = (PWCHAR)L"Medium";
        name.Length = 6 * sizeof(WCHAR);
        name.MaximumLength = name.Length;
        cell = CmpFindValueByName(Hive, root, &name);
        CHECK(cell == Fix->VkMedium);
        if (cell != HCELL_NIL)
        {
            value = (PCM_KEY_VALUE)HvGetCell(Hive, cell);
            CHECK(value != NULL);
            if (value)
            {
                ok = CmpGetValueData(Hive, value, &length, &buffer,
                                     &allocated, &release);
                CHECK(ok == TRUE);
                CHECK(!allocated);
                if (ok)
                {
                    CHECK(length == T_MEDIUM_LEN);
                    CHECK(memcmp(buffer, Fix->MediumPattern, T_MEDIUM_LEN) == 0);
                }
                if (release != HCELL_NIL)
                    HvReleaseCell(Hive, release);
            }
        }

        if (Fix->Minor > 3)
        {
            name.Buffer = (PWCHAR)L"Big";
            name.Length = 3 * sizeof(WCHAR);
            name.MaximumLength = name.Length;
            cell = CmpFindValueByName(Hive, root, &name);
            CHECK(cell == Fix->VkBig);
            if (cell != HCELL_NIL)
            {
                value = (PCM_KEY_VALUE)HvGetCell(Hive, cell);
                CHECK(value != NULL);
                if (value)
                {
                    ok = CmpGetValueData(Hive, value, &length, &buffer,
                                         &allocated, &release);
                    CHECK(ok == TRUE);
                    if (ok)
                    {
                        CHECK(length == T_BIG_LEN);
                        if (length == T_BIG_LEN)
                            CHECK(memcmp(buffer, Fix->BigPattern, T_BIG_LEN) == 0);
                    }
                    if (allocated)
                        Hive->Free(buffer, length);
                    else if (release != HCELL_NIL)
                        HvReleaseCell(Hive, release);
                }
            }
        }
        else
        {
            name.Buffer = (PWCHAR)L"Large";
            name.Length = 5 * sizeof(WCHAR);
            name.MaximumLength = name.Length;
            cell = CmpFindValueByName(Hive, root, &name);
            CHECK(cell == Fix->VkLarge);
            if (cell != HCELL_NIL)
            {
                value = (PCM_KEY_VALUE)HvGetCell(Hive, cell);
                CHECK(value != NULL);
                if (value)
                {
                    ok = CmpGetValueData(Hive, value, &length, &buffer,
                                         &allocated, &release);
                    CHECK(ok == TRUE);
                    CHECK(!allocated);
                    if (ok)
                    {
                        CHECK(length == T_LARGE_LEN);
                        if (length == T_LARGE_LEN)
                            CHECK(memcmp(buffer, Fix->LargePattern, T_LARGE_LEN) == 0);
                    }
                    if (release != HCELL_NIL)
                        HvReleaseCell(Hive, release);
                }
            }
        }
    }

    HvReleaseCell(Hive, Hive->BaseBlock->RootCell);
}

static
VOID
TTestLoadAndRead(
    _In_ ULONG Minor)
{
    T_FIX *fix;
    CMHIVE cmHive;
    NTSTATUS status;

    fix = malloc(sizeof(*fix));
    if (!fix)
    {
        fprintf(stderr, "FAIL malloc fixture\n");
        Failures++;
        return;
    }

    TFixBuild(fix, Minor);
    status = TLoadImage(fix->Image, &cmHive);
    CHECK(status == STATUS_SUCCESS);
    if (NT_SUCCESS(status))
    {
        CM_CHECK_REGISTRY_STATUS checkStatus;

        TCheckFixtureContents(fix, &cmHive.Hive);

        /* The load-time validator must accept the fixture wholesale */
        checkStatus = CmCheckRegistry(&cmHive, CM_CHECK_REGISTRY_VALIDATE_HIVE);
        if (checkStatus != CM_CHECK_REGISTRY_GOOD)
            fprintf(stderr, "FAIL CmCheckRegistry minor %lu status %lu\n",
                    (unsigned long)Minor, (unsigned long)checkStatus);
        CHECK(checkStatus == CM_CHECK_REGISTRY_GOOD);

        HvFree(&cmHive.Hive);
    }

    free(fix);
}

static
VOID
TTestHeaderRefusals(VOID)
{
    T_FIX *fix;
    PHBASE_BLOCK base;

    fix = malloc(sizeof(*fix));
    if (!fix)
    {
        fprintf(stderr, "FAIL malloc fixture\n");
        Failures++;
        return;
    }

    TFixBuild(fix, 5);
    base = (PHBASE_BLOCK)fix->Image;

    /* Sanity: the locally computed checksum matches the library one, and
     * the header verifies for each accepted minor version. */
    CHECK(base->CheckSum == HvpHiveHeaderChecksum(base));
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == TRUE);

    base->Major = 2;
    base->CheckSum = THeaderChecksum508(fix->Image);
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == FALSE);
    base->Major = HSYS_MAJOR;
    base->CheckSum = THeaderChecksum508(fix->Image);

    base->Minor = 3;
    base->CheckSum = THeaderChecksum508(fix->Image);
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == TRUE);
    base->Minor = 4;
    base->CheckSum = THeaderChecksum508(fix->Image);
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == TRUE);
    base->Minor = 6;
    base->CheckSum = THeaderChecksum508(fix->Image);
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == TRUE);
    base->Minor = 5;
    base->CheckSum = THeaderChecksum508(fix->Image);

    base->Type = HFILE_TYPE_LOG;
    base->CheckSum = THeaderChecksum508(fix->Image);
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == FALSE);
    base->Type = HFILE_TYPE_PRIMARY;
    base->CheckSum = THeaderChecksum508(fix->Image);

    base->Format = 2;
    base->CheckSum = THeaderChecksum508(fix->Image);
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == FALSE);
    base->Format = HBASE_FORMAT_MEMORY;
    base->CheckSum = THeaderChecksum508(fix->Image);

    /* Wrong checksum must be refused. */
    base->FileName[0] ^= L'X';
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == FALSE);
    base->FileName[0] ^= L'X';
    CHECK(HvpVerifyHiveHeader(base, HFILE_TYPE_PRIMARY) == TRUE);

    /* A full load must fail cleanly (no crash) on a bad major version. */
    {
        CMHIVE cmHive;
        NTSTATUS status;

        base->Major = 9;
        base->CheckSum = THeaderChecksum508(fix->Image);
        status = TLoadImage(fix->Image, &cmHive);
        CHECK(status == STATUS_REGISTRY_CORRUPT);
        base->Major = HSYS_MAJOR;
        base->CheckSum = THeaderChecksum508(fix->Image);
    }

    free(fix);
}

/* ------------------------------------------------------------------ */
/* New-format (HvLE) transaction log                                   */
/* ------------------------------------------------------------------ */

#define T_LOG_MAX (HSECTOR_SIZE * 28)

typedef struct _T_LOG
{
    UCHAR Buffer[T_LOG_MAX];
    ULONG Size;
} T_LOG;

/* Append one HvLE entry holding one dirty 4096-byte page. */
static
BOOLEAN
TLogAddEntry(
    _Inout_ T_LOG *Log,
    _In_ ULONG Sequence,
    _In_ ULONG ImageLength,
    _In_ ULONG DirtyOffset,
    _In_ const UCHAR *DirtyPage)
{
    PHV_LOG_ENTRY entry;
    PHV_LOG_DIRTY_PAGE page;
    ULONG size = ROUND_UP(sizeof(HV_LOG_ENTRY) + sizeof(HV_LOG_DIRTY_PAGE) +
                          HBLOCK_SIZE, HSECTOR_SIZE);
    PUCHAR at;

    if (Log->Size + size > sizeof(Log->Buffer))
        return FALSE;

    at = Log->Buffer + Log->Size;
    RtlZeroMemory(at, size);

    entry = (PHV_LOG_ENTRY)at;
    entry->Signature = HV_LOG_ENTRY_SIGNATURE;
    entry->Size = size;
    entry->Flags = 0;
    entry->Sequence = Sequence;
    entry->Length = ImageLength;
    entry->DirtyPageCount = 1;

    page = (PHV_LOG_DIRTY_PAGE)(entry + 1);
    page->Offset = DirtyOffset;
    page->Size = HBLOCK_SIZE;
    memcpy(at + sizeof(HV_LOG_ENTRY) + sizeof(HV_LOG_DIRTY_PAGE),
           DirtyPage, HBLOCK_SIZE);

    /* Hash-1 covers the page references and page data (everything past the
     * 40-byte entry header); Hash-2 covers the first 32 bytes of the entry
     * (including Hash-1). */
    entry->Hash1 = TMarvin64(at + sizeof(HV_LOG_ENTRY),
                             size - sizeof(HV_LOG_ENTRY));
    entry->Hash2 = TMarvin64(at, FIELD_OFFSET(HV_LOG_ENTRY, Hash2));

    Log->Size += size;
    return TRUE;
}

static
VOID
TLogStart(
    _Inout_ T_LOG *Log,
    _In_ const T_FIX *Fix,
    _In_ ULONG Sequence,
    _In_ ULONG Type)
{
    PHBASE_BLOCK base = (PHBASE_BLOCK)Log->Buffer;

    RtlZeroMemory(Log, sizeof(*Log));
    memcpy(base, Fix->Image, HSECTOR_SIZE);
    base->Type = Type;
    base->Sequence1 = Sequence;
    base->Sequence2 = Sequence;
    base->CheckSum = THeaderChecksum508(Log->Buffer);
    Log->Size = HSECTOR_SIZE;
}

static
VOID
TDirtyCopy(
    _Inout_ T_FIX *Work,
    _In_ const T_FIX *Fix,
    _In_ ULONG Sequence)
{
    PHBASE_BLOCK base;

    memcpy(Work, Fix, sizeof(*Work));
    base = (PHBASE_BLOCK)Work->Image;
    base->Sequence1 = Sequence;
    base->Sequence2 = Sequence - 1;
    base->CheckSum = THeaderChecksum508(Work->Image);
}

static
VOID
TTestLogRecovery(VOID)
{
    T_FIX *fix, *work;
    T_LOG log;
    CMHIVE cmHive;
    NTSTATUS status;
    ULONG required, mediumBlock, mediumOff, i;
    UCHAR patched[HBLOCK_SIZE];
    PHBASE_BLOCK dirtyBase;
    BOOLEAN ok;

    fix = malloc(sizeof(*fix));
    work = malloc(sizeof(*work));
    if (!fix || !work)
    {
        fprintf(stderr, "FAIL malloc fixture\n");
        Failures++;
        free(fix);
        free(work);
        return;
    }

    TFixBuild(fix, 5);

    /* The library and local Marvin32 must agree, or fixtures are suspect. */
    {
        static const UCHAR Sample[23] =
        {
            'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
            'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v'
        };

        CHECK(TMarvin64(Sample, sizeof(Sample)) ==
              HvpComputeLogHash((PVOID)Sample, sizeof(Sample)));
    }

    /* The block holding the medium value cell, and a patched copy of it. */
    mediumBlock = T_BLK(fix->MediumData);
    mediumOff = (fix->MediumData & HCELL_OFFSET_MASK) + sizeof(HCELL);
    memcpy(patched, fix->Image + HBLOCK_SIZE + mediumBlock * HBLOCK_SIZE,
           HBLOCK_SIZE);
    for (i = 0; i < T_MEDIUM_LEN; i++)
        patched[mediumOff + i] = (UCHAR)(fix->MediumPattern[i] ^ 0xFF);

    /* New-format log: File type 6 header, sequence 7, one HvLE entry. */
    TLogStart(&log, fix, 7, HV_LOG_INCREMENTAL_TYPE);
    CHECK(TLogAddEntry(&log, 7, fix->BinSize, mediumBlock * HBLOCK_SIZE,
                       patched) == TRUE);

    /* A clean primary must ignore the log. */
    {
        T_FIX cleanCopy;

        memcpy(&cleanCopy, fix, sizeof(cleanCopy));
        required = 0;
        CHECK(HvpApplyIncrementalLog((PHBASE_BLOCK)cleanCopy.Image,
                                     cleanCopy.BinSize, log.Buffer, log.Size,
                                     TRUE, NULL, &required) == FALSE);
    }

    /* An old-format log (File type 1) must be rejected by the parser. */
    {
        T_LOG oldLog;

        TLogStart(&oldLog, fix, 7, HFILE_TYPE_LOG);
        CHECK(TLogAddEntry(&oldLog, 7, fix->BinSize,
                           mediumBlock * HBLOCK_SIZE, patched) == TRUE);
        TDirtyCopy(work, fix, 7);
        required = 0;
        CHECK(HvpApplyIncrementalLog((PHBASE_BLOCK)work->Image, work->BinSize,
                                     oldLog.Buffer, oldLog.Size,
                                     TRUE, NULL, &required) == FALSE);
    }

    /* A wrong major version in the log header must be rejected. */
    {
        T_LOG badLog;
        PHBASE_BLOCK badBase;

        TLogStart(&badLog, fix, 7, HV_LOG_INCREMENTAL_TYPE);
        CHECK(TLogAddEntry(&badLog, 7, fix->BinSize,
                           mediumBlock * HBLOCK_SIZE, patched) == TRUE);
        badBase = (PHBASE_BLOCK)badLog.Buffer;
        badBase->Major = 2;
        badBase->CheckSum = THeaderChecksum508(badLog.Buffer);
        TDirtyCopy(work, fix, 7);
        required = 0;
        CHECK(HvpApplyIncrementalLog((PHBASE_BLOCK)work->Image, work->BinSize,
                                     badLog.Buffer, badLog.Size,
                                     TRUE, NULL, &required) == FALSE);
    }

    /* A corrupted entry hash must leave the image untouched. */
    {
        T_LOG badLog;

        TLogStart(&badLog, fix, 7, HV_LOG_INCREMENTAL_TYPE);
        CHECK(TLogAddEntry(&badLog, 7, fix->BinSize,
                           mediumBlock * HBLOCK_SIZE, patched) == TRUE);
        badLog.Buffer[HSECTOR_SIZE + sizeof(HV_LOG_ENTRY) +
                      sizeof(HV_LOG_DIRTY_PAGE) + 100] ^= 0xFF;
        TDirtyCopy(work, fix, 7);
        required = 0;
        CHECK(HvpApplyIncrementalLog((PHBASE_BLOCK)work->Image, work->BinSize,
                                     badLog.Buffer, badLog.Size,
                                     TRUE, NULL, &required) == FALSE);
        CHECK(memcmp(work->Image + HBLOCK_SIZE + mediumBlock * HBLOCK_SIZE,
                     fix->Image + HBLOCK_SIZE + mediumBlock * HBLOCK_SIZE,
                     HBLOCK_SIZE) == 0);
    }

    /* A sequence gap stops recovery after the last contiguous entry. */
    {
        T_LOG gapLog;
        UCHAR secondPage[HBLOCK_SIZE];

        TLogStart(&gapLog, fix, 7, HV_LOG_INCREMENTAL_TYPE);
        CHECK(TLogAddEntry(&gapLog, 7, fix->BinSize,
                           mediumBlock * HBLOCK_SIZE, patched) == TRUE);
        memcpy(secondPage, fix->Image + HBLOCK_SIZE +
               mediumBlock * HBLOCK_SIZE, HBLOCK_SIZE);
        secondPage[0x30] ^= 0x5A;
        CHECK(TLogAddEntry(&gapLog, 9, fix->BinSize,
                           mediumBlock * HBLOCK_SIZE, secondPage) == TRUE);

        TDirtyCopy(work, fix, 7);
        required = 0;
        CHECK(HvpApplyIncrementalLog((PHBASE_BLOCK)work->Image, work->BinSize,
                                     gapLog.Buffer, gapLog.Size,
                                     TRUE, NULL, &required) == TRUE);
        CHECK(required == fix->BinSize);
        CHECK(memcmp(work->Image + HBLOCK_SIZE + mediumBlock * HBLOCK_SIZE,
                     patched, HBLOCK_SIZE) == 0);
    }

    /* The happy path: apply the valid log and read the recovered value. */
    TDirtyCopy(work, fix, 7);
    dirtyBase = (PHBASE_BLOCK)work->Image;

    required = 0;
    CHECK(HvpApplyIncrementalLog(dirtyBase, work->BinSize, log.Buffer,
                                 log.Size, TRUE, NULL, &required) == TRUE);
    CHECK(required == fix->BinSize);

    /* The recovered base block is a consistent primary header. */
    CHECK(dirtyBase->Sequence1 == 7);
    CHECK(dirtyBase->Sequence2 == 7);
    CHECK(dirtyBase->Type == HFILE_TYPE_PRIMARY);
    CHECK(dirtyBase->CheckSum == THeaderChecksum508(work->Image));
    CHECK(dirtyBase->CheckSum == HvpHiveHeaderChecksum(dirtyBase));

    /* The patched page landed where the log entry pointed it. */
    CHECK(memcmp(work->Image + HBLOCK_SIZE + mediumBlock * HBLOCK_SIZE,
                 patched, HBLOCK_SIZE) == 0);

    /* And the recovered image loads and serves the new value. */
    status = TLoadImage(work->Image, &cmHive);
    CHECK(status == STATUS_SUCCESS);
    if (NT_SUCCESS(status))
    {
        PCM_KEY_NODE root;
        UNICODE_STRING name;
        HCELL_INDEX cell;
        PCM_KEY_VALUE value;
        PVOID buffer;
        BOOLEAN allocated;
        HCELL_INDEX release;
        ULONG length;

        root = (PCM_KEY_NODE)HvGetCell(&cmHive.Hive,
                                       cmHive.Hive.BaseBlock->RootCell);
        CHECK(root != NULL);
        name.Buffer = (PWCHAR)L"Medium";
        name.Length = 6 * sizeof(WCHAR);
        name.MaximumLength = name.Length;
        cell = root ? CmpFindValueByName(&cmHive.Hive, root, &name)
                    : HCELL_NIL;
        CHECK(cell == work->VkMedium);
        value = cell != HCELL_NIL ? (PCM_KEY_VALUE)HvGetCell(&cmHive.Hive, cell)
                                  : NULL;
        CHECK(value != NULL);
        if (root)
            HvReleaseCell(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell);
        if (value)
        {
            ok = CmpGetValueData(&cmHive.Hive, value, &length, &buffer,
                                 &allocated, &release);
            CHECK(ok == TRUE);
            CHECK(length == T_MEDIUM_LEN);
            if (ok && length == T_MEDIUM_LEN)
                for (i = 0; i < T_MEDIUM_LEN; i++)
                    CHECK(((UCHAR *)buffer)[i] ==
                          (UCHAR)(fix->MediumPattern[i] ^ 0xFF));
            if (allocated)
                cmHive.Hive.Free(buffer, length);
            else if (release != HCELL_NIL)
                HvReleaseCell(&cmHive.Hive, release);
        }
        HvFree(&cmHive.Hive);
    }

    free(fix);
    free(work);
}

/* ------------------------------------------------------------------ */
/* Optional real-hive leg (path via environment)                       */
/* ------------------------------------------------------------------ */

static
VOID
TTestRealHive(VOID)
{
    const char *path = getenv("CMLIB_TEST_HIVE");
    FILE *file;
    long size;
    UCHAR *image;
    CMHIVE cmHive;
    NTSTATUS status;

    if (!path || !*path)
    {
        printf("cmlib_hive15_read: real-hive leg skipped "
               "(set CMLIB_TEST_HIVE to a hive file)\n");
        return;
    }

    file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "FAIL cannot open %s\n", path);
        Failures++;
        return;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    image = malloc((size_t)size);
    if (!image || fread(image, 1, (size_t)size, file) != (size_t)size)
    {
        fprintf(stderr, "FAIL cannot read %s\n", path);
        Failures++;
        free(image);
        fclose(file);
        return;
    }
    fclose(file);

    status = TLoadImage(image, &cmHive);
    CHECK(status == STATUS_SUCCESS);
    if (NT_SUCCESS(status))
    {
        PHHIVE hive = &cmHive.Hive;
        PCM_KEY_NODE root = (PCM_KEY_NODE)HvGetCell(hive, hive->BaseBlock->RootCell);
        ULONG subkeys = root ? root->SubKeyCounts[Stable] : 0;
        ULONG valuesRead = 0, subkeysSeen = 0, i, acc = 0;

        /* Read every value on the root: the Windows-written names and data
           must resolve through the same paths as the synthetic fixtures. */
        if (root && root->ValueList.Count)
        {
            PCELL_DATA list = (PCELL_DATA)HvGetCell(hive, root->ValueList.List);
            if (list)
            {
                for (i = 0; i < root->ValueList.Count && i < 256; i++)
                {
                    PCM_KEY_VALUE value = (PCM_KEY_VALUE)HvGetCell(hive,
                                                  list->u.KeyList[i]);
                    PVOID buffer;
                    BOOLEAN allocated;
                    HCELL_INDEX release;
                    ULONG length;

                    if (!value) break;
                    if (CmpGetValueData(hive, value, &length, &buffer,
                                        &allocated, &release))
                    {
                        acc ^= length ^ ((PUCHAR)buffer)[0];
                        if (length > 1)
                            acc ^= ((PUCHAR)buffer)[length - 1];
                        valuesRead++;
                        if (allocated)
                            hive->Free(buffer, length);
                        else if (release != HCELL_NIL)
                            HvReleaseCell(hive, release);
                    }
                }
                HvReleaseCell(hive, root->ValueList.List);
            }
        }

        /* Enumerate subkeys and read each one's values too. */
        for (i = 0; i < subkeys && i < 64; i++)
        {
            HCELL_INDEX child = CmpFindSubKeyByNumber(hive, root, i);
            PCM_KEY_NODE node;

            if (child == HCELL_NIL) break;
            node = (PCM_KEY_NODE)HvGetCell(hive, child);
            if (!node) break;
            acc ^= node->NameLength;
            subkeysSeen++;
            if (node->ValueList.Count)
            {
                PCELL_DATA list = (PCELL_DATA)HvGetCell(hive, node->ValueList.List);
                ULONG j;

                if (list)
                {
                    for (j = 0; j < node->ValueList.Count && j < 64; j++)
                    {
                        PCM_KEY_VALUE value = (PCM_KEY_VALUE)HvGetCell(hive,
                                                          list->u.KeyList[j]);
                        PVOID buffer;
                        BOOLEAN allocated;
                        HCELL_INDEX release;
                        ULONG length;

                        if (!value) break;
                        if (CmpGetValueData(hive, value, &length, &buffer,
                                            &allocated, &release))
                        {
                            acc ^= length;
                            valuesRead++;
                            if (allocated)
                                hive->Free(buffer, length);
                            else if (release != HCELL_NIL)
                                HvReleaseCell(hive, release);
                        }
                    }
                    HvReleaseCell(hive, node->ValueList.List);
                }
            }
            HvReleaseCell(hive, child);
        }

        if (root)
            HvReleaseCell(hive, hive->BaseBlock->RootCell);

        printf("cmlib_hive15_read: loaded real hive %s (minor %lu, bins %lu): "
               "%lu subkeys enumerated, %lu values read, acc %08lx\n",
               path, (unsigned long)hive->Version,
               (unsigned long)hive->BaseBlock->Length,
               (unsigned long)subkeysSeen, (unsigned long)valuesRead,
               (unsigned long)acc);
        HvFree(&cmHive.Hive);
    }
    else
    {
        fprintf(stderr, "FAIL real hive %s load status 0x%lx\n",
                path, (unsigned long)status);
    }

    free(image);
}

/* ------------------------------------------------------------------ */

int
main(void)
{
    ULONG minors[] = { 3, 4, 5, 6 };
    ULONG i;

    /* The local helpers must agree with the library's published format
     * routines, otherwise the fixtures themselves would be suspect. */
    {
        UCHAR sample[512];

        RtlZeroMemory(sample, sizeof(sample));
        sample[11] = 0x55;
        sample[507] = 0xAA;
        CHECK(THeaderChecksum508(sample) ==
              HvpHiveHeaderChecksum((PHBASE_BLOCK)sample));
    }

    for (i = 0; i < sizeof(minors) / sizeof(minors[0]); i++)
        TTestLoadAndRead(minors[i]);

    TTestHeaderRefusals();
    TTestLogRecovery();
    TTestRealHive();

    printf("cmlib hive 1.4/1.5/1.6 read: %lu checks, %lu failures\n",
           Checks, Failures);
    return Failures ? 1 : 0;
}

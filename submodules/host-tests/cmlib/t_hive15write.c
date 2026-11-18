/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Writing registry hives in format 1.5: version gate, lh
 *              leaves, big data records, and torn-write recovery through
 *              the incremental (HvLE) log the hive writer produces.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Plan: hive/shared/libernt/design-cmlib-hive15-write.md. The default for
 * newly created hives stays 1.3 (HvGetHiveFormatVersion); the tests flip
 * the gate through HvSetHiveFormatVersion and back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmlib.h>

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
/* Memory file backend for the hive's file callbacks                   */
/* ------------------------------------------------------------------ */

#define T_FILE_CAP (4 * 1024 * 1024)

static UCHAR TPrimary[T_FILE_CAP], TLog[T_FILE_CAP];
static UCHAR TTorn[T_FILE_CAP];
static ULONG TPrimarySize, TLogSize, TTornSize;

static
PVOID
TFileBuffer(
    ULONG FileType,
    PULONG Size)
{
    if (FileType == HFILE_TYPE_PRIMARY)
    {
        *Size = TPrimarySize;
        return TPrimary;
    }
    *Size = TLogSize;
    return TLog;
}

static BOOLEAN CMAPI TFileRead(PHHIVE H, ULONG T, PULONG O, PVOID B, SIZE_T L)
{
    ULONG size;
    PVOID base = TFileBuffer(T, &size);
    (void)H;
    if (*O + L > size) return FALSE;
    memcpy(B, (PUCHAR)base + *O, L);
    *O += (ULONG)L;
    return TRUE;
}

static BOOLEAN CMAPI TFileWrite(PHHIVE H, ULONG T, PULONG O, PVOID B, SIZE_T L)
{
    ULONG size;
    PVOID base = TFileBuffer(T, &size);
    (void)H;
    if (*O + L > T_FILE_CAP) return FALSE;
    memcpy((PUCHAR)base + *O, B, L);
    if (*O + L > size)
        size = *O + (ULONG)L;
    if (T == HFILE_TYPE_PRIMARY) TPrimarySize = size; else TLogSize = size;
    *O += (ULONG)L;
    return TRUE;
}

extern BOOLEAN CMAPI HvpWriteLog(PHHIVE RegistryHive, BOOLEAN Append);

static NTSTATUS
TLoadImageInto(
    CMHIVE *CmHive,
    PVOID Image,
    ULONG Size);

/* ------------------------------------------------------------------ */

static PVOID NTAPI TWAlloc(SIZE_T Size, BOOLEAN Paged, ULONG Tag)
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

static VOID NTAPI TWFree(PVOID Ptr, ULONG Quota)
{
    (void)Quota;
#ifdef _WIN32
    _aligned_free(Ptr);
#else
    free(Ptr);
#endif
}

static NTSTATUS
TCreateHive(
    CMHIVE *CmHive)
{
    RtlZeroMemory(CmHive, sizeof(*CmHive));
    return HvInitialize(&CmHive->Hive, HINIT_CREATE, HIVE_NOLAZYFLUSH,
                        HFILE_TYPE_PRIMARY, 0, TWAlloc, TWFree,
                        TFileWrite, TFileRead, 1, NULL);
}

static HCELL_INDEX
TAddChild(
    PHHIVE Hive,
    HCELL_INDEX Root,
    PCWSTR Name,
    ULONG Chars)
{
    PCM_KEY_NODE node;
    PVOID payload;
    HCELL_INDEX cell;
    ULONG c;

    cell = HvAllocateCell(Hive,
                          FIELD_OFFSET(CM_KEY_NODE, Name) + Chars,
                          Stable, Root);
    if (cell == HCELL_NIL) return HCELL_NIL;
    payload = HvGetCell(Hive, cell);
    node = (PCM_KEY_NODE)payload;
    RtlZeroMemory(node, FIELD_OFFSET(CM_KEY_NODE, Name));
    node->Signature = CM_KEY_NODE_SIGNATURE;
    node->Flags = KEY_COMP_NAME;
    node->Parent = Root;
    node->Security = HCELL_NIL;
    node->Class = HCELL_NIL;
    for (c = 0; c < Chars; c++)
        ((PUCHAR)node->Name)[c] = (UCHAR)Name[c];
    node->NameLength = (USHORT)Chars;
    HvReleaseCell(Hive, cell);

    if (!CmpAddSubKey(Hive, Root, cell))
    {
        HvFreeCell(Hive, cell);
        return HCELL_NIL;
    }
    return cell;
}

static HCELL_INDEX
TAddValue(
    PHHIVE Hive,
    PCM_KEY_NODE Root,
    PCSTR Name,
    ULONG NameBytes,
    ULONG Type,
    PVOID Data,
    ULONG DataSize,
    ULONG Index)
{
    PCM_KEY_VALUE value;
    HCELL_INDEX cell;
    NTSTATUS status;

    cell = HvAllocateCell(Hive,
                          FIELD_OFFSET(CM_KEY_VALUE, Name) + NameBytes,
                          Stable, HCELL_NIL);
    if (cell == HCELL_NIL) return HCELL_NIL;
    value = (PCM_KEY_VALUE)HvGetCell(Hive, cell);
    RtlZeroMemory(value, FIELD_OFFSET(CM_KEY_VALUE, Name));
    value->Signature = CM_KEY_VALUE_SIGNATURE;
    value->NameLength = (USHORT)NameBytes;
    value->Type = Type;
    value->Flags = VALUE_COMP_NAME;
    memcpy(value->Name, Name, NameBytes);

    if (DataSize <= CM_KEY_VALUE_SMALL)
    {
        value->DataLength = DataSize | CM_KEY_VALUE_SPECIAL_SIZE;
        memcpy(&value->Data, Data, DataSize);
        status = STATUS_SUCCESS;
    }
    else
    {
        status = CmpSetValueDataNew(Hive, Data, DataSize, Stable,
                                    cell, &value->Data);
        if (NT_SUCCESS(status)) value->DataLength = DataSize;
    }
    HvReleaseCell(Hive, cell);
    if (!NT_SUCCESS(status))
    {
        HvFreeCell(Hive, cell);
        return HCELL_NIL;
    }

    if (!NT_SUCCESS(CmpAddValueToList(Hive, cell, Index, Stable,
                                      &Root->ValueList)))
    {
        CmpFreeValue(Hive, cell);
        return HCELL_NIL;
    }
    return cell;
}

static USHORT
TRootLeafSignature(
    PHHIVE Hive,
    PCM_KEY_NODE Root)
{
    PCM_KEY_INDEX index = (PCM_KEY_INDEX)HvGetCell(Hive, Root->SubKeyLists[Stable]);
    USHORT sig;

    if (!index) return 0;
    sig = index->Signature;
    if (sig == CM_KEY_INDEX_ROOT)
        sig = ((PCM_KEY_INDEX)HvGetCell(Hive, index->List[0]))->Signature;
    HvReleaseCell(Hive, Root->SubKeyLists[Stable]);
    return sig;
}

static const struct
{
    PCWSTR Name;
    ULONG Chars;
} TChildren[9] =
{
    { L"Bravo", 5 }, { L"Charlie", 7 }, { L"Delta", 5 },
    { L"Echo", 4 }, { L"Foxtrot", 7 }, { L"Golf", 4 },
    { L"Hotel", 5 }, { L"India", 5 }, { L"Juliet", 6 },
};

#define T_MEDIUM 100
#define T_BIG    40000

static UCHAR TMedium[T_MEDIUM], TBig[T_BIG];

static
VOID
TBuild15Hive(CMHIVE *CmHive)
{
    PCM_KEY_NODE root;
    ULONG i;
    NTSTATUS status;

    status = TCreateHive(CmHive);
    if (!NT_SUCCESS(status))
    {
        fprintf(stderr, "FAIL create hive 0x%lx\n", (unsigned long)status);
        Failures++;
        return;
    }
    if (!CmCreateRootNode(&CmHive->Hive, L"$$ROOT"))
    {
        fprintf(stderr, "FAIL root node\n");
        Failures++;
        HvFree(&CmHive->Hive);
        return;
    }

    root = (PCM_KEY_NODE)HvGetCell(&CmHive->Hive, CmHive->Hive.BaseBlock->RootCell);
    for (i = 0; i < 9; i++)
        CHECK(TAddChild(&CmHive->Hive, CmHive->Hive.BaseBlock->RootCell,
                        TChildren[i].Name, TChildren[i].Chars) != HCELL_NIL);

    for (i = 0; i < T_MEDIUM; i++) TMedium[i] = (UCHAR)(0x5A ^ (i * 7));
    for (i = 0; i < T_BIG; i++) TBig[i] = (UCHAR)(i * 131 + 17);

    CHECK(TAddValue(&CmHive->Hive, root, "Medium", 6, REG_BINARY,
                    TMedium, T_MEDIUM, 0) != HCELL_NIL);
    CHECK(TAddValue(&CmHive->Hive, root, "Big", 3, REG_BINARY,
                    TBig, T_BIG, 1) != HCELL_NIL);
    HvReleaseCell(&CmHive->Hive, CmHive->Hive.BaseBlock->RootCell);
}

/* ------------------------------------------------------------------ */

static
VOID
TTestGate(VOID)
{
    CMHIVE cmHive;
    ULONG saved = HvGetHiveFormatVersion();
    PCM_KEY_NODE root;

    CHECK(saved == HSYS_MINOR);

    HvSetHiveFormatVersion(99);
    CHECK(HvGetHiveFormatVersion() == HSYS_MINOR);
    HvSetHiveFormatVersion(2);
    CHECK(HvGetHiveFormatVersion() == HSYS_MINOR);

    /* Default hive: 1.3, fast leaf */
    if (!NT_SUCCESS(TCreateHive(&cmHive))) { Failures++; return; }
    CHECK(cmHive.Hive.BaseBlock->Minor == HSYS_MINOR);
    CHECK(cmHive.Hive.Version == HSYS_MINOR);
    if (CmCreateRootNode(&cmHive.Hive, L"$$ROOT"))
    {
        root = (PCM_KEY_NODE)HvGetCell(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell);
        CHECK(TAddChild(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell,
                        TChildren[0].Name, TChildren[0].Chars) != HCELL_NIL);
        CHECK(TAddChild(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell,
                        TChildren[1].Name, TChildren[1].Chars) != HCELL_NIL);
        CHECK(TRootLeafSignature(&cmHive.Hive, root) == CM_KEY_FAST_LEAF);
        HvReleaseCell(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell);
    }
    HvFree(&cmHive.Hive);

    /* Gated hive: 1.5, hash leaf */
    HvSetHiveFormatVersion(HSYS_WHISTLER);
    if (!NT_SUCCESS(TCreateHive(&cmHive)))
    {
        Failures++;
        HvSetHiveFormatVersion(saved);
        return;
    }
    CHECK(cmHive.Hive.BaseBlock->Minor == HSYS_WHISTLER);
    CHECK(cmHive.Hive.Version == HSYS_WHISTLER);
    if (CmCreateRootNode(&cmHive.Hive, L"$$ROOT"))
    {
        root = (PCM_KEY_NODE)HvGetCell(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell);
        CHECK(TAddChild(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell,
                        TChildren[0].Name, TChildren[0].Chars) != HCELL_NIL);
        CHECK(TAddChild(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell,
                        TChildren[1].Name, TChildren[1].Chars) != HCELL_NIL);
        CHECK(TRootLeafSignature(&cmHive.Hive, root) == CM_KEY_HASH_LEAF);
        HvReleaseCell(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell);
    }
    HvFree(&cmHive.Hive);
    HvSetHiveFormatVersion(saved);
    CHECK(HvGetHiveFormatVersion() == saved);
}

static
VOID
TTestRoundTrip(VOID)
{
    CMHIVE cmHive, reHive;
    ULONG saved = HvGetHiveFormatVersion();
    NTSTATUS status;
    ULONG i;

    HvSetHiveFormatVersion(HSYS_WHISTLER);
    TBuild15Hive(&cmHive);
    if (cmHive.Hive.BaseBlock == NULL)
    {
        HvSetHiveFormatVersion(saved);
        return;
    }

    /* Serialize through the writer and reload the image */
    TPrimarySize = 0;
    CHECK(HvSyncHive(&cmHive.Hive) == TRUE);
    CHECK(TPrimarySize > HBLOCK_SIZE);
    CHECK(((PHBASE_BLOCK)TPrimary)->Minor == HSYS_WHISTLER);

    status = TLoadImageInto(&reHive, TPrimary, TPrimarySize);
    CHECK(status == STATUS_SUCCESS);
    if (NT_SUCCESS(status))
    {
        PHHIVE hive = &reHive.Hive;
        PCM_KEY_NODE root = (PCM_KEY_NODE)HvGetCell(hive, hive->BaseBlock->RootCell);
        UNICODE_STRING name;
        HCELL_INDEX cell;
        PCM_KEY_VALUE value;
        PVOID buffer;
        BOOLEAN allocated;
        HCELL_INDEX release;
        ULONG length;

        CHECK(hive->Version == HSYS_WHISTLER);
        CHECK(root != NULL);
        if (root)
        {
            CHECK(TRootLeafSignature(hive, root) == CM_KEY_HASH_LEAF);

            for (i = 0; i < 9; i++)
            {
                name.Buffer = (PWCHAR)TChildren[i].Name;
                name.Length = (USHORT)(TChildren[i].Chars * sizeof(WCHAR));
                name.MaximumLength = name.Length;
                CHECK(CmpFindSubKeyByName(hive, root, &name) != HCELL_NIL);
            }

            name.Buffer = (PWCHAR)L"Big";
            name.Length = 3 * sizeof(WCHAR);
            name.MaximumLength = name.Length;
            cell = CmpFindValueByName(hive, root, &name);
            CHECK(cell != HCELL_NIL);
            value = cell != HCELL_NIL ? (PCM_KEY_VALUE)HvGetCell(hive, cell) : NULL;
            CHECK(value != NULL);
            if (value &&
                CmpGetValueData(hive, value, &length, &buffer, &allocated, &release))
            {
                CHECK(length == T_BIG);
                if (length == T_BIG)
                    CHECK(memcmp(buffer, TBig, T_BIG) == 0);
                if (allocated) hive->Free(buffer, length);
                else if (release != HCELL_NIL) HvReleaseCell(hive, release);
            }
            else
            {
                CHECK(FALSE);
            }

            CHECK(CmCheckRegistry(&reHive, CM_CHECK_REGISTRY_VALIDATE_HIVE) ==
                  CM_CHECK_REGISTRY_GOOD);
            HvReleaseCell(hive, hive->BaseBlock->RootCell);
        }
        HvFree(hive);
    }

    HvFree(&cmHive.Hive);
    HvSetHiveFormatVersion(saved);
}

static
VOID
TTestTornWrite(VOID)
{
    CMHIVE cmHive, reHive;
    ULONG saved = HvGetHiveFormatVersion();
    NTSTATUS status;
    PHBASE_BLOCK tornBase;
    ULONG required = 0;
    PCM_KEY_VALUE value;
    PVOID buffer;
    BOOLEAN allocated;
    HCELL_INDEX release;
    ULONG length;
    ULONG i;
    static const UCHAR TNew[T_MEDIUM] = { 0 };

    HvSetHiveFormatVersion(HSYS_WHISTLER);
    TBuild15Hive(&cmHive);
    if (cmHive.Hive.BaseBlock == NULL)
    {
        HvSetHiveFormatVersion(saved);
        return;
    }

    /* Persist the built hive, then change the medium value while it is
       dirty, then log the change and leave the primary dirty the way a
       crash would */
    TPrimarySize = 0;
    TLogSize = 0;
    CHECK(HvSyncHive(&cmHive.Hive) == TRUE);
    CHECK(TPrimarySize > HBLOCK_SIZE);

    {
        PCM_KEY_NODE root = (PCM_KEY_NODE)HvGetCell(&cmHive.Hive,
                                                    cmHive.Hive.BaseBlock->RootCell);
        UNICODE_STRING name;
        HCELL_INDEX vkCell;

        name.Buffer = (PWCHAR)L"Medium";
        name.Length = 6 * sizeof(WCHAR);
        name.MaximumLength = name.Length;
        vkCell = CmpFindValueByName(&cmHive.Hive, root, &name);
        value = (PCM_KEY_VALUE)HvGetCell(&cmHive.Hive, vkCell);
        HvReleaseCell(&cmHive.Hive, cmHive.Hive.BaseBlock->RootCell);
        if (vkCell != HCELL_NIL)
            HvMarkCellDirty(&cmHive.Hive, vkCell, FALSE);
    }
    CHECK(value != NULL);
    if (value)
    {
        HCELL_INDEX newCell;

        CmpFreeValueData(&cmHive.Hive, value->Data, value->DataLength);
        CHECK(NT_SUCCESS(CmpSetValueDataNew(&cmHive.Hive, (PVOID)TNew, T_MEDIUM,
                                            Stable, HCELL_NIL, &newCell)));
        value->Data = newCell;
        value->DataLength = T_MEDIUM;
    }

    /* Start a new log cycle so the primary is left dirty, as a crash would */
    cmHive.Hive.CurrentLogOffset = 0;
    CHECK(HvpWriteLog(&cmHive.Hive, TRUE) == TRUE);
    CHECK(TLogSize >= HSECTOR_SIZE + sizeof(HV_LOG_ENTRY));

    /* Crash state: the synced primary with the dirty header the log left */
    memcpy(TTorn, TPrimary, TPrimarySize);
    TTornSize = TPrimarySize;
    tornBase = (PHBASE_BLOCK)TTorn;
    CHECK(tornBase->Sequence1 != tornBase->Sequence2);

    CHECK(HvpApplyIncrementalLog(tornBase, TTornSize - HBLOCK_SIZE,
                                 TLog, TLogSize, TRUE, NULL,
                                 &required) == TRUE);
    CHECK(required >= tornBase->Length);

    status = TLoadImageInto(&reHive, TTorn, TTornSize);
    CHECK(status == STATUS_SUCCESS);
    if (NT_SUCCESS(status))
    {
        PHHIVE hive = &reHive.Hive;
        PCM_KEY_NODE root = (PCM_KEY_NODE)HvGetCell(hive, hive->BaseBlock->RootCell);
        UNICODE_STRING name;
        HCELL_INDEX cell;

        CHECK(root != NULL);
        name.Buffer = (PWCHAR)L"Medium";
        name.Length = 6 * sizeof(WCHAR);
        name.MaximumLength = name.Length;
        cell = root ? CmpFindValueByName(hive, root, &name) : HCELL_NIL;
        CHECK(cell != HCELL_NIL);
        value = cell != HCELL_NIL ? (PCM_KEY_VALUE)HvGetCell(hive, cell) : NULL;
        CHECK(value != NULL);
        if (value &&
            CmpGetValueData(hive, value, &length, &buffer, &allocated, &release))
        {
            CHECK(length == T_MEDIUM);
            if (length == T_MEDIUM)
            {
                for (i = 0; i < T_MEDIUM; i++)
                    CHECK(((UCHAR *)buffer)[i] == TNew[i]);
            }
            if (allocated) hive->Free(buffer, length);
            else if (release != HCELL_NIL) HvReleaseCell(hive, release);
        }
        else
        {
            CHECK(FALSE);
        }
        if (root)
            HvReleaseCell(hive, hive->BaseBlock->RootCell);
        HvFree(hive);
    }

    HvFree(&cmHive.Hive);
    HvSetHiveFormatVersion(saved);
}

/* ------------------------------------------------------------------ */

static NTSTATUS
TLoadImageInto(
    CMHIVE *CmHive,
    PVOID Image,
    ULONG Size)
{
    (void)Size;
    RtlZeroMemory(CmHive, sizeof(*CmHive));
    return HvInitialize(&CmHive->Hive, HINIT_MEMORY, 0, HFILE_TYPE_PRIMARY,
                        Image, TWAlloc, TWFree, TFileWrite, TFileRead, 1, NULL);
}

int
main(void)
{
    TTestGate();
    TTestRoundTrip();
    TTestTornWrite();

    printf("cmlib hive 1.5 write: %lu checks, %lu failures\n",
           Checks, Failures);
    return Failures ? 1 : 0;
}

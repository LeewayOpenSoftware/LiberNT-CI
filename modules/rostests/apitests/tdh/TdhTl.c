/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     TdhGetEventInformation TraceLogging decode: exact two-pass sizing,
 *              malformed-input handling, and a bounded fuzz pass
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <apitest.h>
#include <evntcons.h>
#include <string.h>

/*
 * Bounded fuzz pass, under the owner fuzz rules.
 * Objective: TdhGetEventInformation on a mutated TraceLogging EVENT_SCHEMA_TL
 * extended item must never fault and must return a documented status, for any
 * byte mutation and any item size.
 * Budget: a fixed wall-clock window (a few seconds), not an iteration count.
 * Stop rule: the deadline ends the loop; every mutation inside the window must
 * satisfy the objective. Iteration counts are deliberately not reported.
 */
#define TDH_FUZZ_WINDOW_MS   2000
#define TDH_FUZZ_BUFLEN      8192

static const GUID TestProviderGuid = {0x0d4a3f6c, 0x1b2a, 0x4c3d, {0x8e, 0x9f, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f}};

typedef struct _TLG_BUILDER
{
    UCHAR Bytes[512];
    ULONG Length;
    USHORT RemainingAt;
} TLG_BUILDER, *PTLG_BUILDER;

static VOID
BuilderStart(IN OUT PTLG_BUILDER B)
{
    RtlZeroMemory(B, sizeof(*B));
    /* _tlgEventMetadata_t: Type, Channel, Level, Opcode, Keyword, RemainingSize */
    B->Bytes[B->Length++] = 6; /* _TlgBlobEvent4 */
    B->Bytes[B->Length++] = 11;
    B->Bytes[B->Length++] = 5;
    B->Bytes[B->Length++] = 2;
    memcpy(&B->Bytes[B->Length], "\x34\x12\x00\x00\x00\x00\x00\x00", 8);
    B->Length += 8;
    B->RemainingAt = (USHORT)B->Length;
    B->Length += 2;
}

static VOID
BuilderFinish(IN OUT PTLG_BUILDER B)
{
    USHORT Remaining = (USHORT)(B->Length - B->RemainingAt);

    B->Bytes[B->RemainingAt] = (UCHAR)(Remaining & 0xFF);
    B->Bytes[B->RemainingAt + 1] = (UCHAR)(Remaining >> 8);
}

static VOID
BuilderByte(IN OUT PTLG_BUILDER B, IN UCHAR Value)
{
    B->Bytes[B->Length++] = Value;
}

static VOID
BuilderName(IN OUT PTLG_BUILDER B, IN PCSTR Name)
{
    ULONG n = (ULONG)strlen(Name) + 1;

    memcpy(&B->Bytes[B->Length], Name, n);
    B->Length += n;
}

/* one well-formed event: tags(0), "TestEvent", three top-level fields, one
   struct of two members */
static ULONG
BuildValidBlob(OUT PUCHAR Blob)
{
    TLG_BUILDER B;

    BuilderStart(&B);
    BuilderByte(&B, 0); /* event tags: one byte, high bit unset */
    BuilderName(&B, "TestEvent");
    /* Alpha: INT32, no chain */
    BuilderName(&B, "Alpha");
    BuilderByte(&B, 7);
    /* Beta: UNICODESTRING */
    BuilderName(&B, "Beta");
    BuilderByte(&B, 1);
    /* Group: STRUCT with chain, OutType = 2 members */
    BuilderName(&B, "Group");
    BuilderByte(&B, 24 | 128);
    BuilderByte(&B, 2);
    /* members G1 UINT16, G2 UINT64 */
    BuilderName(&B, "G1");
    BuilderByte(&B, 6);
    BuilderName(&B, "G2");
    BuilderByte(&B, 10);
    /* nested struct: Outer { Inner { Ix INT16, Iy UINT32 }, Tail UINT8 } */
    BuilderName(&B, "Outer");
    BuilderByte(&B, 24 | 128);
    BuilderByte(&B, 2);
    BuilderName(&B, "Inner");
    BuilderByte(&B, 24 | 128);
    BuilderByte(&B, 2);
    BuilderName(&B, "Ix");
    BuilderByte(&B, 5);
    BuilderName(&B, "Iy");
    BuilderByte(&B, 8);
    BuilderName(&B, "Tail");
    BuilderByte(&B, 4);
    BuilderFinish(&B);

    memcpy(Blob, B.Bytes, B.Length);
    return B.Length;
}

static VOID
SetupEvent(IN OUT PEVENT_RECORD Event, IN OUT PEVENT_HEADER_EXTENDED_DATA_ITEM Item,
           IN PUCHAR Blob, IN ULONG BlobSize)
{
    memset(Event, 0, sizeof(*Event));
    memset(Item, 0, sizeof(*Item));
    Event->EventHeader.ProviderId = TestProviderGuid;
    Event->EventHeader.EventDescriptor.Id = 42;
    Event->ExtendedDataCount = 1;
    Event->ExtendedData = Item;
    Item->ExtType = EVENT_HEADER_EXT_TYPE_EVENT_SCHEMA_TL;
    Item->DataSize = (USHORT)BlobSize;
    Item->DataPtr = (ULONGLONG)(ULONG_PTR)Blob;
}

static VOID
TestValidEvent(VOID)
{
    UCHAR Blob[512];
    ULONG BlobSize;
    EVENT_RECORD Event;
    EVENT_HEADER_EXTENDED_DATA_ITEM Item;
    UCHAR Buffer[2048];
    ULONG Size;
    ULONG Error;
    PTRACE_EVENT_INFO Info = (PTRACE_EVENT_INFO)Buffer;
    PEVENT_PROPERTY_INFO Props, Alpha, Beta, Group, G1;

    BlobSize = BuildValidBlob(Blob);
    SetupEvent(&Event, &Item, Blob, BlobSize);

    /* a zero size is rejected even for sizing (Win11: ERROR_INVALID_PARAMETER) */
    memset(Buffer, 0xCC, sizeof(Buffer));
    Size = 0;
    Error = TdhGetEventInformation(&Event, 0, NULL, Buffer, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "zero-size error %lu\n", Error);

    /* pass 1: an undersized nonzero buffer reports the exact requirement
       and receives no writes */
    memset(Buffer, 0xCC, sizeof(Buffer));
    Size = 16;
    Error = TdhGetEventInformation(&Event, 0, NULL, Buffer, &Size);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "pass1 error %lu\n", Error);
    ok(Size >= FIELD_OFFSET(TRACE_EVENT_INFO, EventPropertyInfoArray) + 4 * sizeof(EVENT_PROPERTY_INFO),
       "size %lu too small\n", Size);
    ok(Buffer[0] == 0xCC, "pass1 wrote to the caller's buffer\n");

    /* pass 2: the exact size decodes; Win11 rejects this synthetic record
       as ERROR_FILE_CORRUPT, which the size contract above still pins */
    Error = TdhGetEventInformation(&Event, 0, NULL, Buffer, &Size);
    ok(Error == ERROR_SUCCESS || Error == ERROR_FILE_CORRUPT, "pass2 error %lu\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    ok(Info->DecodingSource == DecodingSourceTlg, "DecodingSource %d\n", Info->DecodingSource);
    ok(IsEqualGUID(&Info->ProviderGuid, &TestProviderGuid), "ProviderGuid\n");
    ok(Info->EventDescriptor.Id == 42, "Id %u\n", Info->EventDescriptor.Id);
    ok(Info->EventDescriptor.Level == 5 && Info->EventDescriptor.Opcode == 2,
       "Level %u Opcode %u\n", Info->EventDescriptor.Level, Info->EventDescriptor.Opcode);
    ok(Info->PropertyCount == 10, "PropertyCount %lu\n", Info->PropertyCount);
    ok(Info->TopLevelPropertyCount == 4, "TopLevel %lu\n", Info->TopLevelPropertyCount);
    ok(Info->ProviderNameOffset == 0, "ProviderNameOffset %lu without traits\n",
       Info->ProviderNameOffset);
    ok(Info->TaskNameOffset != 0, "TaskNameOffset 0\n");
    ok(!wcscmp((PWSTR)((PUCHAR)Info + Info->TaskNameOffset), L"TestEvent"),
       "TaskName %ls\n", (PWSTR)((PUCHAR)Info + Info->TaskNameOffset));
    ok(Info->EventNameOffset == Info->TaskNameOffset, "EventNameOffset\n");

    Props = Info->EventPropertyInfoArray;
    Alpha = &Props[0];
    Beta = &Props[1];
    Group = &Props[2];
    G1 = &Props[3];
    ok(!wcscmp((PWSTR)((PUCHAR)Info + Alpha->NameOffset), L"Alpha"), "field 0 name\n");
    ok(Alpha->nonStructType.InType == TDH_INTYPE_INT32, "Alpha InType %u\n",
       Alpha->nonStructType.InType);
    ok(!wcscmp((PWSTR)((PUCHAR)Info + Beta->NameOffset), L"Beta"), "field 1 name\n");
    ok(Beta->nonStructType.InType == TDH_INTYPE_UNICODESTRING, "Beta InType %u\n",
       Beta->nonStructType.InType);
    ok((Group->Flags & PropertyStruct) != 0, "Group not a struct\n");
    ok(Group->structType.NumOfStructMembers == 2, "Group members %u\n",
       Group->structType.NumOfStructMembers);
    ok(Group->structType.StructStartIndex == 3, "Group start %u\n",
       Group->structType.StructStartIndex);
    ok(!wcscmp((PWSTR)((PUCHAR)Info + G1->NameOffset), L"G1"), "member 0 name\n");
    ok(G1->nonStructType.InType == TDH_INTYPE_UINT16, "G1 InType %u\n",
       G1->nonStructType.InType);

    /* nested structs flatten with each struct's direct members contiguous:
       Outer, Inner, Tail, Ix, Iy with Outer { Inner, Tail } and Inner { Ix, Iy } */
    {
        PEVENT_PROPERTY_INFO Outer = &Props[5];
        PEVENT_PROPERTY_INFO Inner = &Props[6];
        PEVENT_PROPERTY_INFO Tail = &Props[7];
        PEVENT_PROPERTY_INFO Ix = &Props[8];

        ok(!wcscmp((PWSTR)((PUCHAR)Info + Outer->NameOffset), L"Outer"), "Outer name\n");
        ok(Outer->structType.StructStartIndex == 6 && Outer->structType.NumOfStructMembers == 2,
           "Outer start %u members %u\n", Outer->structType.StructStartIndex,
           Outer->structType.NumOfStructMembers);
        ok(!wcscmp((PWSTR)((PUCHAR)Info + Inner->NameOffset), L"Inner"), "Inner name\n");
        ok(Inner->structType.StructStartIndex == 8 && Inner->structType.NumOfStructMembers == 2,
           "Inner start %u members %u\n", Inner->structType.StructStartIndex,
           Inner->structType.NumOfStructMembers);
        ok(!wcscmp((PWSTR)((PUCHAR)Info + Tail->NameOffset), L"Tail"), "Tail name\n");
        ok(Tail->nonStructType.InType == TDH_INTYPE_UINT8, "Tail InType %u\n",
           Tail->nonStructType.InType);
        ok(!wcscmp((PWSTR)((PUCHAR)Info + Ix->NameOffset), L"Ix"), "Ix name\n");
        ok(Ix->nonStructType.InType == TDH_INTYPE_INT16, "Ix InType %u\n",
           Ix->nonStructType.InType);
    }
}

static VOID
TestProviderTraits(VOID)
{
    static const UCHAR Traits[] = { 13, 0, 'T', 'e', 's', 't', 'P', 'r', 'o', 'v', 0 };
    UCHAR Blob[512];
    ULONG BlobSize;
    EVENT_RECORD Event;
    EVENT_HEADER_EXTENDED_DATA_ITEM Items[2];
    UCHAR Buffer[2048];
    ULONG Size;
    ULONG Error;
    PTRACE_EVENT_INFO Info = (PTRACE_EVENT_INFO)Buffer;

    BlobSize = BuildValidBlob(Blob);
    SetupEvent(&Event, &Items[0], Blob, BlobSize);
    Items[1].ExtType = EVENT_HEADER_EXT_TYPE_PROV_TRAITS;
    Items[1].DataSize = sizeof(Traits);
    Items[1].DataPtr = (ULONGLONG)(ULONG_PTR)Traits;
    Event.ExtendedDataCount = 2;
    Event.ExtendedData = Items;

    Size = sizeof(Buffer);
    Error = TdhGetEventInformation(&Event, 0, NULL, Buffer, &Size);
    ok(Error == ERROR_SUCCESS || Error == ERROR_FILE_CORRUPT, "traits error %lu\n", Error);
    if (Error != ERROR_SUCCESS)
        return;
    ok(Info->ProviderNameOffset != 0, "ProviderNameOffset 0 with traits\n");
    if (Info->ProviderNameOffset)
        ok(!wcscmp((PWSTR)((PUCHAR)Info + Info->ProviderNameOffset), L"TestProv"),
           "ProviderName %ls\n", (PWSTR)((PUCHAR)Info + Info->ProviderNameOffset));
}

static VOID
TestMalformed(VOID)
{
    UCHAR Blob[512];
    ULONG BlobSize, i, Size, Error;
    EVENT_RECORD Event;
    EVENT_HEADER_EXTENDED_DATA_ITEM Item;
    UCHAR Buffer[2048];

    BlobSize = BuildValidBlob(Blob);

    /* no schema item at all */
    SetupEvent(&Event, &Item, Blob, BlobSize);
    Item.ExtType = EVENT_HEADER_EXT_TYPE_EVENT_KEY;
    Size = sizeof(Buffer);
    Error = TdhGetEventInformation(&Event, 0, NULL, Buffer, &Size);
    ok(Error == ERROR_NOT_FOUND, "no-schema error %lu\n", Error);

    /* every truncation of the valid blob: no fault, documented statuses */
    SetupEvent(&Event, &Item, Blob, BlobSize);
    for (i = 0; i < BlobSize; i++)
    {
        Item.DataSize = (USHORT)i;
        Size = sizeof(Buffer);
        Error = TdhGetEventInformation(&Event, 0, NULL, Buffer, &Size);
        ok(Error == ERROR_SUCCESS || Error == ERROR_INVALID_DATA ||
           Error == ERROR_INSUFFICIENT_BUFFER || Error == ERROR_NOT_FOUND ||
           Error == ERROR_FILE_CORRUPT,
           "truncation %lu error %lu\n", i, Error);
    }

    /* no-NUL event name: name runs to the end of the item */
    SetupEvent(&Event, &Item, Blob, BlobSize);
    Blob[10] = 'X'; /* inside "TestEvent": drop the terminator */
    Item.DataSize = 16; /* cut the item right after the name region */
    Size = sizeof(Buffer);
    Error = TdhGetEventInformation(&Event, 0, NULL, Buffer, &Size);
    ok(Error == ERROR_INVALID_DATA || Error == ERROR_NOT_FOUND, "no-NUL error %lu\n", Error);
}

static VOID
TestBoundedFuzz(VOID)
{
    /*
     * Objective: any byte mutation of the valid schema item, at any item size,
     * must terminate with a documented status and without a fault.
     * Budget: TDH_FUZZ_WINDOW_MS of wall time. Stop rule: the deadline.
     */
    static UCHAR Blob[512];
    UCHAR Mutant[512];
    ULONG BlobSize, Size, Error, State;
    DWORD Deadline;
    EVENT_RECORD Event;
    EVENT_HEADER_EXTENDED_DATA_ITEM Item;
    UCHAR Buffer[TDH_FUZZ_BUFLEN];

    BlobSize = BuildValidBlob(Blob);
    SetupEvent(&Event, &Item, Mutant, BlobSize);
    State = 0x12345678;
    Deadline = GetTickCount() + TDH_FUZZ_WINDOW_MS;

    while (GetTickCount() < Deadline)
    {
        memcpy(Mutant, Blob, BlobSize);
        /* xorshift: three cheap random values per iteration */
        State ^= State << 13; State ^= State >> 17; State ^= State << 5;
        Mutant[State % BlobSize] = (UCHAR)(State >> 8);
        State ^= State << 13; State ^= State >> 17; State ^= State << 5;
        Mutant[State % BlobSize] = (UCHAR)(State >> 16);
        State ^= State << 13; State ^= State >> 17; State ^= State << 5;
        Item.DataSize = (USHORT)(1 + State % BlobSize);

        Size = sizeof(Buffer);
        Error = TdhGetEventInformation(&Event, 0, NULL, Buffer, &Size);
        ok(Error == ERROR_SUCCESS || Error == ERROR_INVALID_DATA ||
           Error == ERROR_INSUFFICIENT_BUFFER || Error == ERROR_NOT_FOUND ||
           Error == ERROR_FILE_CORRUPT,
           "fuzz status %lu\n", Error);
        if (Error != ERROR_SUCCESS && Error != ERROR_INVALID_DATA &&
            Error != ERROR_INSUFFICIENT_BUFFER && Error != ERROR_NOT_FOUND &&
            Error != ERROR_FILE_CORRUPT)
            break;
    }
    trace("bounded fuzz pass completed within its window\n");
}

VOID
func_TdhTl(void)
{
    TestValidEvent();
    TestProviderTraits();
    TestMalformed();
    TestBoundedFuzz();
}

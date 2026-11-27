/*
 * PROJECT:     LiberNT Trace Data Helper
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Event schema lookup for ETW consumers: bounded TraceLogging
 *              (in-band event metadata) decoding for TdhGetEventInformation
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <windef.h>
#include <winbase.h>
#include <evntcons.h>
#include <stringapiset.h>

/*
 * TraceLogging in-band metadata layout: Windows SDK TraceLoggingProvider.h
 * (_tlgEventMetadata_t, EventMetadata, FieldMetadata). The metadata arrives in
 * an EVENT_HEADER_EXT_TYPE_EVENT_SCHEMA_TL extended item; every read here is
 * bounded by that item's DataSize, and strings must be NUL-terminated within
 * those bounds.
 */
#define TLG_BLOB_EVENT3           3
#define TLG_BLOB_EVENT4           6

#define TLG_IN_CHAIN              128
#define TLG_IN_VCOUNT             64
#define TLG_IN_CCOUNT             32
#define TLG_IN_COUNT_MASK         (TLG_IN_VCOUNT | TLG_IN_CCOUNT)
#define TLG_IN_TYPE_MASK          31
#define TLG_IN_STRUCT             24

#define TLG_MAX_FIELDS            256
#define TLG_MAX_DEPTH             16
#define TLG_MAX_TAG_BYTES         4

typedef struct _TLG_READER
{
    const UCHAR * Ptr;
    const UCHAR * End;
} TLG_READER;

static
BOOLEAN
TlgReadU8(IN OUT TLG_READER *Reader, OUT PUCHAR Value)
{
    if (Reader->End - Reader->Ptr < 1)
        return FALSE;
    *Value = *Reader->Ptr++;
    return TRUE;
}

static
BOOLEAN
TlgReadU16(IN OUT TLG_READER *Reader, OUT PUSHORT Value)
{
    if (Reader->End - Reader->Ptr < 2)
        return FALSE;
    *Value = (USHORT)(Reader->Ptr[0] | (Reader->Ptr[1] << 8));
    Reader->Ptr += 2;
    return TRUE;
}

static
BOOLEAN
TlgReadU64(IN OUT TLG_READER *Reader, OUT PULONGLONG Value)
{
    ULONG i;
    ULONGLONG v = 0;

    if (Reader->End - Reader->Ptr < 8)
        return FALSE;
    for (i = 0; i < 8; i++)
        v |= (ULONGLONG)Reader->Ptr[i] << (8 * i);
    Reader->Ptr += 8;
    *Value = v;
    return TRUE;
}

/* A UTF-8 string that must be NUL-terminated inside the item bounds */
static
BOOLEAN
TlgReadString(IN OUT TLG_READER *Reader, OUT PCSTR *Start, OUT PUSHORT Length)
{
    const UCHAR * p = Reader->Ptr;

    while (p < Reader->End && *p)
        p++;
    if (p == Reader->End)
        return FALSE;
    *Start = (PCSTR)Reader->Ptr;
    *Length = (USHORT)(p - Reader->Ptr);
    Reader->Ptr = p + 1;
    return TRUE;
}

/* Extension bytes: 7 bits of tag per byte, high bit set means continue.
   Only the first four bytes are defined by the spec. */
static
BOOLEAN
TlgReadTags(IN OUT TLG_READER *Reader, OUT PULONG Tags)
{
    ULONG Value = 0, Count = 0;
    UCHAR Byte;

    do
    {
        if (!TlgReadU8(Reader, &Byte) || ++Count > TLG_MAX_TAG_BYTES)
            return FALSE;
        Value = (Value << 7) | (Byte & 0x7F);
    } while (Byte & 0x80);

    *Tags = Value;
    return TRUE;
}

typedef struct _TLG_FIELD
{
    PCSTR Name;
    USHORT NameLength;
    USHORT InType;
    USHORT OutType;
    BOOLEAN IsStruct;
    USHORT StructMembers;
    USHORT StructStart;
    USHORT ValueCount;
    BOOLEAN HasValueCount;
    USHORT TypeInfoSize;
} TLG_FIELD, *PTLG_FIELD;

/* Parses one FieldMetadata and advances; StructMembers is read recursively
   by the caller so the field array can be flattened depth-first. */
static
BOOLEAN
TlgParseFieldHeader(IN OUT TLG_READER *Reader, OUT PTLG_FIELD Field)
{
    UCHAR InType, OutType;

    RtlZeroMemory(Field, sizeof(*Field));
    if (!TlgReadString(Reader, &Field->Name, &Field->NameLength))
        return FALSE;
    if (!TlgReadU8(Reader, &InType))
        return FALSE;
    Field->InType = InType & TLG_IN_TYPE_MASK;
    Field->IsStruct = (Field->InType == TLG_IN_STRUCT);

    if (InType & TLG_IN_CHAIN)
    {
        if (!TlgReadU8(Reader, &OutType))
            return FALSE;
        Field->OutType = OutType & 0x7F;
        if (OutType & TLG_IN_CHAIN)
        {
            ULONG Tags;

            if (!TlgReadTags(Reader, &Tags))
                return FALSE;
        }
    }

    if ((InType & TLG_IN_COUNT_MASK) == TLG_IN_CCOUNT)
    {
        USHORT Count;

        if (!TlgReadU16(Reader, &Count))
            return FALSE;
        Field->ValueCount = Count;
        Field->HasValueCount = TRUE;
    }
    else if ((InType & TLG_IN_COUNT_MASK) == TLG_IN_COUNT_MASK)
    {
        USHORT Size;

        /* custom serializer: skip the declared TypeInfo, bounded by the item */
        if (!TlgReadU16(Reader, &Size))
            return FALSE;
        if (Reader->End - Reader->Ptr < Size)
            return FALSE;
        Reader->Ptr += Size;
        Field->TypeInfoSize = Size;
    }

    return TRUE;
}

/* Result of one full metadata walk: the flattened field list. */
typedef struct _TLG_NODE TLG_NODE;
typedef struct _TLG_DECODE
{
    TLG_FIELD Fields[TLG_MAX_FIELDS];
    ULONG Count;
    ULONG TopLevelCount;
    TLG_NODE *Pool;      /* parse tree storage, owned per call */
    USHORT PoolCount;
} TLG_DECODE, *PTLG_DECODE;

/* The stream nests struct members depth-first, but TRACE_EVENT_INFO requires
   each struct's direct members to be contiguous. The parse builds a small
   tree, then a breadth-first flatten lays members out contiguously. */
struct _TLG_NODE
{
    TLG_FIELD Field;
    USHORT Parent;      /* pool index, 0xFFFF for a root */
    USHORT FirstChild;  /* pool index + 1, 0 for none */
    USHORT NextSibling; /* pool index + 1, 0 for none */
};

static
BOOLEAN
TlgBuildNode(IN OUT TLG_READER *Reader,
             IN ULONG Depth,
             IN USHORT Parent,
             IN OUT PTLG_DECODE Decode,
             OUT PUSHORT NodeIndex)
{
    TLG_FIELD Field;
    USHORT MyIndex, ChildIndex, LastChild, i;

    if (!TlgParseFieldHeader(Reader, &Field))
        return FALSE;
    if (Decode->PoolCount >= TLG_MAX_FIELDS)
        return FALSE;

    if (Field.IsStruct)
        Field.StructMembers = Field.OutType;

    MyIndex = Decode->PoolCount++;
    Decode->Pool[MyIndex].Field = Field;
    Decode->Pool[MyIndex].Parent = Parent;
    Decode->Pool[MyIndex].FirstChild = 0;
    Decode->Pool[MyIndex].NextSibling = 0;

    if (Field.IsStruct)
    {
        if (Depth + 1 >= TLG_MAX_DEPTH)
            return FALSE;
        LastChild = 0xFFFF;
        for (i = 0; i < Field.OutType; i++)
        {
            if (!TlgBuildNode(Reader, Depth + 1, MyIndex, Decode, &ChildIndex))
                return FALSE;
            if (LastChild == 0xFFFF)
                Decode->Pool[MyIndex].FirstChild = (USHORT)(ChildIndex + 1);
            else
                Decode->Pool[LastChild].NextSibling = (USHORT)(ChildIndex + 1);
            LastChild = ChildIndex;
        }
    }

    *NodeIndex = MyIndex;
    return TRUE;
}

static
BOOLEAN
TlgParseEventFields(IN OUT TLG_READER *Reader, IN OUT PTLG_DECODE Decode)
{
    TLG_NODE Pool[TLG_MAX_FIELDS];
    USHORT Queue[TLG_MAX_FIELDS];
    USHORT NodeToField[TLG_MAX_FIELDS];
    ULONG Head = 0, Tail = 0;

    /* the tree and the flatten state are per-call: consumers run this entry
       point concurrently, so nothing may live in file-scope storage */
    RtlZeroMemory(Decode, sizeof(*Decode));
    Decode->Pool = Pool;
    Decode->PoolCount = 0;

    /* roots: the top-level field list runs to the end of the item */
    while (Reader->Ptr < Reader->End)
    {
        USHORT Root;

        if (Tail >= TLG_MAX_FIELDS)
            return FALSE;
        if (!TlgBuildNode(Reader, 0, 0xFFFF, Decode, &Root))
            return FALSE;
        Queue[Tail++] = Root;
        Decode->TopLevelCount++;
    }

    while (Head < Tail)
    {
        USHORT Node = Queue[Head++];
        USHORT Child;
        USHORT FieldIndex;

        if (Decode->Count >= TLG_MAX_FIELDS)
            return FALSE;
        FieldIndex = (USHORT)Decode->Count++;
        Decode->Fields[FieldIndex] = Decode->Pool[Node].Field;
        Decode->Fields[FieldIndex].StructStart = 0;
        NodeToField[Node] = FieldIndex;

        /* the first appended child marks its parent's member run */
        if (Decode->Pool[Node].Parent != 0xFFFF &&
            Decode->Fields[NodeToField[Decode->Pool[Node].Parent]].StructStart == 0)
        {
            Decode->Fields[NodeToField[Decode->Pool[Node].Parent]].StructStart = FieldIndex;
        }

        for (Child = Decode->Pool[Node].FirstChild; Child; Child = Decode->Pool[Child - 1].NextSibling)
        {
            if (Tail >= TLG_MAX_FIELDS)
                return FALSE;
            Queue[Tail++] = (USHORT)(Child - 1);
        }
    }

    Decode->Pool = NULL; /* the stack storage dies with this frame */
    return TRUE;
}

/* Locates the EVENT_SCHEMA_TL extended item and the provider traits item */
static
BOOLEAN
TlgFindExtendedItem(IN PEVENT_RECORD Event, IN USHORT ExtType, OUT const UCHAR * *Data, OUT PUSHORT DataSize)
{
    USHORT i;

    for (i = 0; i < Event->ExtendedDataCount; i++)
    {
        PEVENT_HEADER_EXTENDED_DATA_ITEM Item = &Event->ExtendedData[i];

        if (Item->ExtType == ExtType && Item->DataSize && Item->DataPtr)
        {
            *Data = (const UCHAR *)(ULONG_PTR)Item->DataPtr;
            *DataSize = Item->DataSize;
            return TRUE;
        }
    }
    return FALSE;
}

/* The PROV_TRAITS item carries the ProviderMetadata pseudo-structure:
   UINT16 Size, then the UTF-8 provider name, then optional chunks. */
static
BOOLEAN
TlgParseProviderName(IN const UCHAR * Data, IN USHORT DataSize, OUT PCSTR *Name, OUT PUSHORT NameLength)
{
    TLG_READER Reader;
    USHORT Size;

    if (DataSize < 2 + 1)
        return FALSE;
    Reader.Ptr = Data;
    Reader.End = Data + DataSize;
    if (!TlgReadU16(&Reader, &Size))
        return FALSE;
    if (Size < 3 || (SIZE_T)(Reader.End - Reader.Ptr) < (SIZE_T)(Size - 2))
        return FALSE;
    Reader.End = Reader.Ptr + (Size - 2);
    return TlgReadString(&Reader, Name, NameLength);
}

/* UTF-8 -> UTF-16 size and copy helpers; both bounded by the source length */
static
ULONG
TlgWideSize(IN PCSTR Name, IN USHORT Length)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Name, Length, NULL, 0);
}

static
BOOLEAN
TlgWideCopy(IN PCSTR Name, IN USHORT Length, OUT PWSTR Wide)
{
    return TlgWideSize(Name, Length) != 0 &&
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Name, Length, Wide, Length + 1) != 0;
}

ULONG
WINAPI
TdhGetEventInformation(
    _In_ PEVENT_RECORD Event,
    _In_ ULONG TdhContextCount,
    _In_reads_opt_(TdhContextCount) PTDH_CONTEXT TdhContext,
    _Out_writes_bytes_opt_(*BufferSize) PTRACE_EVENT_INFO Buffer,
    _Inout_ PULONG BufferSize)
{
    const UCHAR *Schema, *Traits;
    USHORT SchemaSize, TraitsSize;
    TLG_READER Reader;
    UCHAR BlobType, Channel, Level, Opcode;
    ULONGLONG Keyword;
    USHORT Remaining;
    PCSTR EventName, ProviderName = NULL;
    USHORT EventNameLength = 0, ProviderNameLength = 0;
    ULONG EventTags = 0;
    TLG_DECODE Decode;
    ULONG i, Required, StringOffset;
    PCSTR MetadataEnd;

    if (!Event || !BufferSize || (TdhContextCount && !TdhContext))
        return ERROR_INVALID_PARAMETER;
    if (!*BufferSize)
        return ERROR_INVALID_PARAMETER; /* Win11 rejects a zero size even for sizing */

    if (!TlgFindExtendedItem(Event, EVENT_HEADER_EXT_TYPE_EVENT_SCHEMA_TL, &Schema, &SchemaSize))
        return ERROR_NOT_FOUND;

    /* _tlgEventMetadata_t header, bounded by the extended item */
    Reader.Ptr = Schema;
    Reader.End = Schema + SchemaSize;
    if (!TlgReadU8(&Reader, &BlobType))
        return ERROR_INVALID_DATA;
    if (BlobType != TLG_BLOB_EVENT3 && BlobType != TLG_BLOB_EVENT4)
        return ERROR_INVALID_DATA;
    if (!TlgReadU8(&Reader, &Channel) || !TlgReadU8(&Reader, &Level) ||
        !TlgReadU8(&Reader, &Opcode) || !TlgReadU64(&Reader, &Keyword) ||
        !TlgReadU16(&Reader, &Remaining))
        return ERROR_INVALID_DATA;
    if (Remaining < 3 || Reader.End - Reader.Ptr < (LONG)(Remaining - 2))
        return ERROR_INVALID_DATA;
    MetadataEnd = Reader.Ptr + (Remaining - 2);
    Reader.End = MetadataEnd;

    /* EventMetadata: Tags, Name, then the field list */
    if (!TlgReadTags(&Reader, &EventTags))
        return ERROR_INVALID_DATA;
    if (!TlgReadString(&Reader, &EventName, &EventNameLength))
        return ERROR_INVALID_DATA;

    if (!TlgParseEventFields(&Reader, &Decode))
        return ERROR_INVALID_DATA;

    if (TlgFindExtendedItem(Event, EVENT_HEADER_EXT_TYPE_PROV_TRAITS, &Traits, &TraitsSize))
    {
        TlgParseProviderName(Traits, TraitsSize, &ProviderName, &ProviderNameLength);
    }

    /* Output size: header + property array + UTF-16 strings, checked sums */
    Required = FIELD_OFFSET(TRACE_EVENT_INFO, EventPropertyInfoArray) +
               Decode.Count * sizeof(EVENT_PROPERTY_INFO);
    if (ProviderName && TlgWideSize(ProviderName, ProviderNameLength))
        Required += (TlgWideSize(ProviderName, ProviderNameLength) + 1) * sizeof(WCHAR);
    if (TlgWideSize(EventName, EventNameLength))
        Required += (TlgWideSize(EventName, EventNameLength) + 1) * sizeof(WCHAR);
    for (i = 0; i < Decode.Count; i++)
    {
        ULONG WideLength = TlgWideSize(Decode.Fields[i].Name, Decode.Fields[i].NameLength);

        if (!WideLength)
            return ERROR_INVALID_DATA;
        Required += (WideLength + 1) * sizeof(WCHAR);
    }

    if (!Buffer || *BufferSize < Required)
    {
        *BufferSize = Required;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    /* Fill pass: the input blob is re-walked nowhere; Decode holds the parse */
    RtlZeroMemory(Buffer, Required);
    Buffer->ProviderGuid = Event->EventHeader.ProviderId;
    Buffer->EventDescriptor = Event->EventHeader.EventDescriptor;
    Buffer->EventDescriptor.Channel = Channel;
    Buffer->EventDescriptor.Level = Level;
    Buffer->EventDescriptor.Opcode = Opcode;
    Buffer->EventDescriptor.Keyword = Keyword;
    Buffer->DecodingSource = DecodingSourceTlg;
    Buffer->PropertyCount = Decode.Count;
    Buffer->TopLevelPropertyCount = Decode.TopLevelCount;
    Buffer->Tags = EventTags & 0x0FFFFFFF;

    StringOffset = FIELD_OFFSET(TRACE_EVENT_INFO, EventPropertyInfoArray) +
                   Decode.Count * sizeof(EVENT_PROPERTY_INFO);

    if (ProviderName && TlgWideSize(ProviderName, ProviderNameLength))
    {
        Buffer->ProviderNameOffset = StringOffset;
        TlgWideCopy(ProviderName, ProviderNameLength, (PWSTR)((PUCHAR)Buffer + StringOffset));
        StringOffset += (TlgWideSize(ProviderName, ProviderNameLength) + 1) * sizeof(WCHAR);
    }
    if (TlgWideSize(EventName, EventNameLength))
    {
        Buffer->TaskNameOffset = StringOffset;
        Buffer->EventNameOffset = StringOffset;
        TlgWideCopy(EventName, EventNameLength, (PWSTR)((PUCHAR)Buffer + StringOffset));
        StringOffset += (TlgWideSize(EventName, EventNameLength) + 1) * sizeof(WCHAR);
    }

    for (i = 0; i < Decode.Count; i++)
    {
        PEVENT_PROPERTY_INFO Property = &Buffer->EventPropertyInfoArray[i];
        ULONG WideLength = TlgWideSize(Decode.Fields[i].Name, Decode.Fields[i].NameLength);

        Property->NameOffset = StringOffset;
        TlgWideCopy(Decode.Fields[i].Name, Decode.Fields[i].NameLength,
                    (PWSTR)((PUCHAR)Buffer + StringOffset));
        StringOffset += (WideLength + 1) * sizeof(WCHAR);

        Property->nonStructType.InType = Decode.Fields[i].InType;
        Property->nonStructType.OutType = 0; /* TlgOut mapping is follow-up work */
        if (Decode.Fields[i].IsStruct)
        {
            Property->Flags |= PropertyStruct;
            Property->structType.StructStartIndex = Decode.Fields[i].StructStart;
            Property->structType.NumOfStructMembers = Decode.Fields[i].StructMembers;
        }
        if (Decode.Fields[i].HasValueCount)
            Property->count = Decode.Fields[i].ValueCount;
    }

    *BufferSize = Required;
    return ERROR_SUCCESS;
}

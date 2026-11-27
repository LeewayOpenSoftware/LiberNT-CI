/*
 * PROJECT:     LiberNT SDK Headers
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Event tracing consumer definitions
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#ifndef _EVNTCONS_H_
#define _EVNTCONS_H_

#include <wmistr.h>
#include <evntrace.h>
#include <evntprov.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_HEADER_EXT_TYPE_RELATED_ACTIVITYID   0x0001
#define EVENT_HEADER_EXT_TYPE_SID                  0x0002
#define EVENT_HEADER_EXT_TYPE_TS_ID                0x0003
#define EVENT_HEADER_EXT_TYPE_INSTANCE_INFO        0x0004
#define EVENT_HEADER_EXT_TYPE_STACK_TRACE32        0x0005
#define EVENT_HEADER_EXT_TYPE_STACK_TRACE64        0x0006
#define EVENT_HEADER_EXT_TYPE_PEBS_INDEX           0x0007
#define EVENT_HEADER_EXT_TYPE_PMC_COUNTERS         0x0008
#define EVENT_HEADER_EXT_TYPE_PSM_KEY              0x0009
#define EVENT_HEADER_EXT_TYPE_EVENT_KEY            0x000A
#define EVENT_HEADER_EXT_TYPE_EVENT_SCHEMA_TL      0x000B
#define EVENT_HEADER_EXT_TYPE_PROV_TRAITS          0x000C
#define EVENT_HEADER_EXT_TYPE_PROCESS_START_KEY    0x000D
#define EVENT_HEADER_EXT_TYPE_CONTROL_GUID         0x000E
#define EVENT_HEADER_EXT_TYPE_MAX                  0x000F

#ifndef EVENT_HEADER_EXTENDED_DATA_ITEM_DEF
#define EVENT_HEADER_EXTENDED_DATA_ITEM_DEF
typedef struct _EVENT_HEADER_EXTENDED_DATA_ITEM {
  USHORT Reserved1;
  USHORT ExtType;
  struct {
    USHORT Linkage : 1;
    USHORT Reserved2 : 15;
  } DUMMYSTRUCTNAME;
  USHORT DataSize;
  ULONGLONG DataPtr;
} EVENT_HEADER_EXTENDED_DATA_ITEM, *PEVENT_HEADER_EXTENDED_DATA_ITEM;
#endif

#define EVENT_HEADER_PROPERTY_XML               0x0001
#define EVENT_HEADER_PROPERTY_FORWARDED_XML     0x0002
#define EVENT_HEADER_PROPERTY_LEGACY_EVENTLOG   0x0004
#define EVENT_HEADER_PROPERTY_RELOGGABLE        0x0008

#define EVENT_HEADER_FLAG_EXTENDED_INFO         0x0001
#define EVENT_HEADER_FLAG_PRIVATE_SESSION       0x0002
#define EVENT_HEADER_FLAG_STRING_ONLY           0x0004
#define EVENT_HEADER_FLAG_TRACE_MESSAGE         0x0008
#define EVENT_HEADER_FLAG_NO_CPUTIME            0x0010
#define EVENT_HEADER_FLAG_32_BIT_HEADER         0x0020
#define EVENT_HEADER_FLAG_64_BIT_HEADER         0x0040
#define EVENT_HEADER_FLAG_DECODE_GUID           0x0080
#define EVENT_HEADER_FLAG_CLASSIC_HEADER        0x0100
#define EVENT_HEADER_FLAG_PROCESSOR_INDEX       0x0200

#ifndef EVENT_HEADER_DEF
#define EVENT_HEADER_DEF
typedef struct _EVENT_HEADER {
  USHORT Size;
  USHORT HeaderType;
  USHORT Flags;
  USHORT EventProperty;
  ULONG ThreadId;
  ULONG ProcessId;
  LARGE_INTEGER TimeStamp;
  GUID ProviderId;
  EVENT_DESCRIPTOR EventDescriptor;
  union {
    struct {
      ULONG KernelTime;
      ULONG UserTime;
    } DUMMYSTRUCTNAME;
    ULONG64 ProcessorTime;
  } DUMMYUNIONNAME;
  GUID ActivityId;
} EVENT_HEADER, *PEVENT_HEADER;
#endif

#ifndef EVENT_RECORD_DEF
#define EVENT_RECORD_DEF
struct _EVENT_RECORD {
  EVENT_HEADER EventHeader;
  ETW_BUFFER_CONTEXT BufferContext;
  USHORT ExtendedDataCount;
  USHORT UserDataLength;
  PEVENT_HEADER_EXTENDED_DATA_ITEM ExtendedData;
  PVOID UserData;
  PVOID UserContext;
};

#if defined(_WMIKM_) || defined(_NTDDK_) || defined(_NTIFS_)
typedef struct _EVENT_RECORD EVENT_RECORD, *PEVENT_RECORD;
#endif
typedef const EVENT_RECORD *PCEVENT_RECORD;
#endif

#define EVENT_ENABLE_PROPERTY_SID                   0x00000001
#define EVENT_ENABLE_PROPERTY_TS_ID                 0x00000002
#define EVENT_ENABLE_PROPERTY_STACK_TRACE           0x00000004
#define EVENT_ENABLE_PROPERTY_PSM_KEY               0x00000008
#define EVENT_ENABLE_PROPERTY_IGNORE_KEYWORD_0      0x00000010
#define EVENT_ENABLE_PROPERTY_PROVIDER_GROUP        0x00000020
#define EVENT_ENABLE_PROPERTY_ENABLE_KEYWORD_0      0x00000040
#define EVENT_ENABLE_PROPERTY_PROCESS_START_KEY     0x00000080
#define EVENT_ENABLE_PROPERTY_EVENT_KEY             0x00000100
#define EVENT_ENABLE_PROPERTY_EXCLUDE_INPRIVATE     0x00000200

#define PROCESS_TRACE_MODE_REAL_TIME                0x00000100
#define PROCESS_TRACE_MODE_RAW_TIMESTAMP            0x00001000
#define PROCESS_TRACE_MODE_EVENT_RECORD             0x10000000

/*
 * TDH decoding types (Windows SDK 10.0.26100.0 tdh.h): the consumer-side
 * descriptions TdhGetEventInformation fills in.
 */
typedef enum _DECODING_SOURCE
{
    DecodingSourceXMLFile,
    DecodingSourceWbem,
    DecodingSourceWPP,
    DecodingSourceTlg,
    DecodingSourceMax
} DECODING_SOURCE;

typedef enum _TDH_IN_TYPE
{
    TDH_INTYPE_NULL,
    TDH_INTYPE_UNICODESTRING,
    TDH_INTYPE_ANSISTRING,
    TDH_INTYPE_INT8,
    TDH_INTYPE_UINT8,
    TDH_INTYPE_INT16,
    TDH_INTYPE_UINT16,
    TDH_INTYPE_INT32,
    TDH_INTYPE_UINT32,
    TDH_INTYPE_INT64,
    TDH_INTYPE_UINT64,
    TDH_INTYPE_FLOAT,
    TDH_INTYPE_DOUBLE,
    TDH_INTYPE_BOOLEAN,
    TDH_INTYPE_BINARY,
    TDH_INTYPE_GUID,
    TDH_INTYPE_POINTER,
    TDH_INTYPE_FILETIME,
    TDH_INTYPE_SYSTEMTIME,
    TDH_INTYPE_SID,
    TDH_INTYPE_HEXINT32,
    TDH_INTYPE_HEXINT64,
    TDH_INTYPE_MANIFEST_COUNTEDSTRING,
    TDH_INTYPE_MANIFEST_COUNTEDANSISTRING,
    TDH_INTYPE_RESERVED24,
    TDH_INTYPE_MANIFEST_COUNTEDBINARY
} TDH_IN_TYPE;

typedef enum _PROPERTY_FLAGS
{
    PropertyStruct = 0x1,
    PropertyParamLength = 0x2,
    PropertyParamCount = 0x4,
    PropertyWBEMXmlFragment = 0x8,
    PropertyParamFixedLength = 0x10,
    PropertyParamFixedCount = 0x20,
    PropertyHasTags = 0x40,
    PropertyHasCustomSchema = 0x80
} PROPERTY_FLAGS;

typedef struct _EVENT_PROPERTY_INFO
{
    PROPERTY_FLAGS Flags;
    ULONG NameOffset;
    union
    {
        struct
        {
            USHORT InType;
            USHORT OutType;
            ULONG MapNameOffset;
        } nonStructType;
        struct
        {
            USHORT StructStartIndex;
            USHORT NumOfStructMembers;
            ULONG padding;
        } structType;
    } DUMMYUNIONNAME;
    union
    {
        USHORT count;
        USHORT countPropertyIndex;
    } DUMMYUNIONNAME2;
    union
    {
        USHORT length;
        USHORT lengthPropertyIndex;
    } DUMMYUNIONNAME3;
    union
    {
        ULONG Reserved;
        struct
        {
            ULONG Tags : 28;
        };
    } DUMMYUNIONNAME4;
} EVENT_PROPERTY_INFO, *PEVENT_PROPERTY_INFO;

typedef enum _TDH_CONTEXT_TYPE
{
    TDH_CONTEXT_WPP_TMFFILE,
    TDH_CONTEXT_WPP_TMFSEARCHPATH,
    TDH_CONTEXT_WPP_GMT,
    TDH_CONTEXT_POINTERSIZE,
    TDH_CONTEXT_PDB_PATH,
    TDH_CONTEXT_MAXIMUM
} TDH_CONTEXT_TYPE;

typedef struct _TDH_CONTEXT
{
    ULONGLONG ParameterValue;
    TDH_CONTEXT_TYPE ParameterType;
    ULONG ParameterSize;
} TDH_CONTEXT, *PTDH_CONTEXT;

typedef struct _TRACE_EVENT_INFO
{
    GUID ProviderGuid;
    GUID EventGuid;
    EVENT_DESCRIPTOR EventDescriptor;
    DECODING_SOURCE DecodingSource;
    ULONG ProviderNameOffset;
    ULONG LevelNameOffset;
    ULONG ChannelNameOffset;
    ULONG KeywordsNameOffset;
    ULONG TaskNameOffset;
    ULONG OpcodeNameOffset;
    ULONG EventMessageOffset;
    ULONG ProviderMessageOffset;
    ULONG BinaryXMLOffset;
    ULONG BinaryXMLSize;
    union
    {
        ULONG EventNameOffset;
        ULONG ActivityIDNameOffset;
    };
    union
    {
        ULONG EventAttributesOffset;
        ULONG RelatedActivityIDNameOffset;
    };
    ULONG PropertyCount;
    ULONG TopLevelPropertyCount;
    union
    {
        ULONG Flags;
        struct
        {
            ULONG Reserved : 4;
            ULONG Tags : 28;
        };
    };
    EVENT_PROPERTY_INFO EventPropertyInfoArray[ANYSIZE_ARRAY];
} TRACE_EVENT_INFO, *PTRACE_EVENT_INFO;

typedef ULONG TDHSTATUS;

#ifdef __cplusplus
}
#endif

#endif /* _EVNTCONS_H_ */

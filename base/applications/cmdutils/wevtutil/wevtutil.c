/*
 * PROJECT:     LiberNT Windows Events Command Line Utility
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     List, query, configure, export and clear event logs through wevtapi
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winnls.h>
#include <sddl.h>
#include <winevt.h>
#include <strsafe.h>

#include <conutils.h>

#include "resource.h"

#define EVENTLOG_KEY L"SYSTEM\\CurrentControlSet\\Services\\EventLog"

/* Defaults from the Windows "Eventlog Key" documentation */
#define CLASSIC_SDDL L"O:BAG:SYD:(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x7;;;SO)(A;;0x3;;;IU)(A;;0x3;;;SU)(A;;0x3;;;S-1-5-3)(A;;0x3;;;S-1-5-33)(A;;0x1;;;S-1-5-32-573)"
#define SECURITY_SDDL L"O:BAG:SYD:(A;;0xf0005;;;SY)(A;;0x5;;;BA)(A;;0x1;;;S-1-5-32-573)"

#define KEYWORD_CLASSIC         0x0080000000000000ULL
#define KEYWORD_AUDIT_SUCCESS   0x0020000000000000ULL
#define KEYWORD_AUDIT_FAILURE   0x0010000000000000ULL

#define MAX_TEXT_FILE           (16 * 1024 * 1024)
#define MAX_POSITIONAL          2

typedef enum _OPTION_ID
{
    OPT_REMOTE,
    OPT_USER,
    OPT_PASSWORD,
    OPT_AUTH,
    OPT_UNICODE,
    OPT_FORMAT,
    OPT_COUNT,
    OPT_REVERSE,
    OPT_QUERY,
    OPT_LOGFILE,
    OPT_STRUCTURED,
    OPT_BOOKMARK,
    OPT_SAVEBOOKMARK,
    OPT_LOCALE,
    OPT_ELEMENT,
    OPT_OVERWRITE,
    OPT_BACKUP,
    OPT_ENABLED,
    OPT_ISOLATION,
    OPT_LOGFILENAME,
    OPT_RETENTION,
    OPT_AUTOBACKUP,
    OPT_MAXSIZE,
    OPT_LEVEL,
    OPT_KEYWORDS,
    OPT_CHANNELACCESS,
    OPT_CONFIG,
    OPT_MAX
} OPTION_ID;

typedef struct _OPTION_NAME
{
    PCWSTR Short;
    PCWSTR Long;
} OPTION_NAME;

static const OPTION_NAME OptionNames[OPT_MAX] =
{
    { L"r", L"remote" },
    { L"u", L"username" },
    { L"p", L"password" },
    { L"a", L"authentication" },
    { L"uni", L"unicode" },
    { L"f", L"format" },
    { L"c", L"count" },
    { L"rd", L"reversedirection" },
    { L"q", L"query" },
    { L"lf", L"logfile" },
    { L"sq", L"structuredquery" },
    { L"bm", L"bookmark" },
    { L"sbm", L"savebookmark" },
    { L"l", L"locale" },
    { L"e", L"element" },
    { L"ow", L"overwrite" },
    { L"bu", L"backup" },
    { L"e", L"enabled" },
    { L"i", L"isolation" },
    { L"lfn", L"logfilename" },
    { L"rt", L"retention" },
    { L"ab", L"autobackup" },
    { L"ms", L"maxsize" },
    { L"l", L"level" },
    { L"k", L"keywords" },
    { L"ca", L"channelaccess" },
    { L"c", L"config" },
};

static PCWSTR OptionValues[OPT_MAX];
static WCHAR OptionTyped[OPT_MAX][32];

typedef DWORD (*COMMAND_PROC)(int ArgCount, PWSTR *Args);

typedef struct _COMMAND
{
    PCWSTR Short;
    PCWSTR Long;
    UINT HelpId;
    UINT MinArgs;
    UINT MaxArgs;
    const OPTION_ID *Options;
    COMMAND_PROC Proc;
} COMMAND;

static EVT_HANDLE Session;
static const COMMAND *CurrentCommand;

static PVOID Alloc(SIZE_T Size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size ? Size : 1);
}

static VOID Free(PVOID Memory)
{
    if (Memory)
        HeapFree(GetProcessHeap(), 0, Memory);
}

typedef struct _STRBUF
{
    PWSTR Buffer;
    SIZE_T Length;
    SIZE_T Capacity;
    BOOL Failed;
} STRBUF;

static VOID SbAppendN(STRBUF *Sb, PCWSTR Text, SIZE_T Count)
{
    if (Sb->Failed)
        return;
    if (Sb->Length + Count + 1 > Sb->Capacity)
    {
        SIZE_T Capacity = max(Sb->Capacity * 2, Sb->Length + Count + 256);
        PWSTR Grown = Alloc(Capacity * sizeof(WCHAR));
        if (!Grown)
        {
            Sb->Failed = TRUE;
            return;
        }
        if (Sb->Buffer)
        {
            memcpy(Grown, Sb->Buffer, Sb->Length * sizeof(WCHAR));
            Free(Sb->Buffer);
        }
        Sb->Buffer = Grown;
        Sb->Capacity = Capacity;
    }
    memcpy(Sb->Buffer + Sb->Length, Text, Count * sizeof(WCHAR));
    Sb->Length += Count;
    Sb->Buffer[Sb->Length] = UNICODE_NULL;
}

static VOID SbAppend(STRBUF *Sb, PCWSTR Text)
{
    SbAppendN(Sb, Text, wcslen(Text));
}

static PWSTR SbFinish(STRBUF *Sb)
{
    if (Sb->Failed)
    {
        Free(Sb->Buffer);
        return NULL;
    }
    if (!Sb->Buffer)
        SbAppendN(Sb, L"", 0);
    return Sb->Buffer;
}

static VOID LoadResString(UINT Id, PWSTR Buffer, UINT Count)
{
    if (!LoadStringW(GetModuleHandleW(NULL), Id, Buffer, Count))
        Buffer[0] = UNICODE_NULL;
}

static VOID PrintSystemError(DWORD Error)
{
    PWSTR Message = NULL;
    UINT Id = 0;

    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, Error, 0, (PWSTR)&Message, 0, NULL) && Message)
    {
        SIZE_T Length = wcslen(Message);
        while (Length && (Message[Length - 1] == L'\r' || Message[Length - 1] == L'\n'))
            Message[--Length] = UNICODE_NULL;
        ConPrintf(StdErr, L"%ls\n", Message);
        LocalFree(Message);
        return;
    }

    switch (Error)
    {
        case ERROR_EVT_INVALID_CHANNEL_PATH: Id = IDS_EVT_INVALID_CHANNEL_PATH; break;
        case ERROR_EVT_INVALID_QUERY:        Id = IDS_EVT_INVALID_QUERY; break;
        case ERROR_EVT_CHANNEL_NOT_FOUND:    Id = IDS_EVT_CHANNEL_NOT_FOUND; break;
    }
    if (Id)
        ConResPuts(StdErr, Id);
    else
        ConResPrintf(StdErr, IDS_ERR_UNKNOWN, Error);
}

static DWORD FailWith(UINT Id, PCWSTR Argument, DWORD Error)
{
    if (Argument)
        ConResPrintf(StdErr, Id, Argument);
    else
        ConResPuts(StdErr, Id);
    PrintSystemError(Error);
    return Error;
}

static DWORD UsageError(UINT Id, PCWSTR Argument)
{
    return FailWith(Id, Argument, ERROR_INVALID_PARAMETER);
}

static BOOL ParseBool(PCWSTR Value, PBOOL Result)
{
    if (!_wcsicmp(Value, L"true"))
    {
        *Result = TRUE;
        return TRUE;
    }
    if (!_wcsicmp(Value, L"false"))
    {
        *Result = FALSE;
        return TRUE;
    }
    return FALSE;
}

static BOOL ParseUInt64(PCWSTR Value, PULONGLONG Result)
{
    PWSTR End;

    if (!iswdigit(*Value))
        return FALSE;
    *Result = _wcstoui64(Value, &End, 10);
    return *End == UNICODE_NULL;
}

static BOOL GetBoolOption(OPTION_ID Id, BOOL Default, PBOOL Result, PDWORD Error)
{
    *Result = Default;
    if (!OptionValues[Id])
        return TRUE;
    if (ParseBool(OptionValues[Id], Result))
        return TRUE;
    *Error = UsageError(IDS_ERR_INVALID_VALUE, OptionTyped[Id]);
    return FALSE;
}

static VOID FormatFileTime(ULONGLONG Value, BOOL Local, BOOL Milliseconds, PWSTR Buffer, SIZE_T Count)
{
    FILETIME FileTime;
    SYSTEMTIME SystemTime, LocalTime;
    ULONG Fraction;

    FileTime.dwLowDateTime = (DWORD)Value;
    FileTime.dwHighDateTime = (DWORD)(Value >> 32);
    if (!FileTimeToSystemTime(&FileTime, &SystemTime))
    {
        Buffer[0] = UNICODE_NULL;
        return;
    }
    if (Local && SystemTimeToTzSpecificLocalTime(NULL, &SystemTime, &LocalTime))
        SystemTime = LocalTime;
    Fraction = Milliseconds ? (ULONG)SystemTime.wMilliseconds * 10000 : (ULONG)(Value % 10000000);
    StringCchPrintfW(Buffer, Count, L"%04u-%02u-%02uT%02u:%02u:%02u.%07uZ",
                     SystemTime.wYear, SystemTime.wMonth, SystemTime.wDay,
                     SystemTime.wHour, SystemTime.wMinute, SystemTime.wSecond, Fraction);
}

static VOID FormatVariant(const EVT_VARIANT *Variant, PWSTR Buffer, SIZE_T Count)
{
    switch (Variant->Type)
    {
        case EvtVarTypeString:
            StringCchCopyW(Buffer, Count, Variant->StringVal ? Variant->StringVal : L"");
            break;
        case EvtVarTypeBoolean:
            StringCchCopyW(Buffer, Count, Variant->BooleanVal ? L"true" : L"false");
            break;
        case EvtVarTypeByte:
            StringCchPrintfW(Buffer, Count, L"%u", Variant->ByteVal);
            break;
        case EvtVarTypeUInt16:
            StringCchPrintfW(Buffer, Count, L"%u", Variant->UInt16Val);
            break;
        case EvtVarTypeUInt32:
        case EvtVarTypeHexInt32:
            StringCchPrintfW(Buffer, Count, L"%lu", Variant->UInt32Val);
            break;
        case EvtVarTypeUInt64:
        case EvtVarTypeHexInt64:
            StringCchPrintfW(Buffer, Count, L"%I64u", Variant->UInt64Val);
            break;
        case EvtVarTypeFileTime:
            FormatFileTime(Variant->FileTimeVal, FALSE, FALSE, Buffer, Count);
            break;
        default:
            Buffer[0] = UNICODE_NULL;
            break;
    }
}

static PVOID RenderAlloc(EVT_HANDLE Context, EVT_HANDLE Fragment, DWORD Flags, PDWORD Count)
{
    DWORD Used = 0, PropertyCount = 0;
    PVOID Buffer;

    if (EvtRender(Context, Fragment, Flags, 0, NULL, &Used, &PropertyCount) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return NULL;
    }
    Buffer = Alloc(Used);
    if (!Buffer)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    if (!EvtRender(Context, Fragment, Flags, Used, Buffer, &Used, &PropertyCount))
    {
        DWORD Error = GetLastError();
        Free(Buffer);
        SetLastError(Error);
        return NULL;
    }
    if (Count)
        *Count = PropertyCount;
    return Buffer;
}

static PEVT_VARIANT GetChannelProperty(EVT_HANDLE Config, EVT_CHANNEL_CONFIG_PROPERTY_ID Id)
{
    DWORD Used = 0;
    PEVT_VARIANT Variant;

    if (EvtGetChannelConfigProperty(Config, Id, 0, 0, NULL, &Used) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return NULL;
    Variant = Alloc(Used);
    if (!Variant)
        return NULL;
    if (!EvtGetChannelConfigProperty(Config, Id, 0, Used, Variant, &Used))
    {
        Free(Variant);
        return NULL;
    }
    return Variant;
}

static PEVT_VARIANT GetLogProperty(EVT_HANDLE Log, EVT_LOG_PROPERTY_ID Id)
{
    DWORD Used = 0;
    PEVT_VARIANT Variant;

    if (EvtGetLogInfo(Log, Id, 0, NULL, &Used) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return NULL;
    Variant = Alloc(Used);
    if (!Variant)
        return NULL;
    if (!EvtGetLogInfo(Log, Id, Used, Variant, &Used))
    {
        Free(Variant);
        return NULL;
    }
    return Variant;
}

static BOOL GetChannelString(EVT_HANDLE Config, EVT_CHANNEL_CONFIG_PROPERTY_ID Id, PWSTR Buffer, SIZE_T Count)
{
    PEVT_VARIANT Variant = GetChannelProperty(Config, Id);

    Buffer[0] = UNICODE_NULL;
    if (!Variant)
        return FALSE;
    FormatVariant(Variant, Buffer, Count);
    Free(Variant);
    return TRUE;
}

static BOOL GetChannelNumber(EVT_HANDLE Config, EVT_CHANNEL_CONFIG_PROPERTY_ID Id, PULONGLONG Value)
{
    PEVT_VARIANT Variant = GetChannelProperty(Config, Id);

    *Value = 0;
    if (!Variant)
        return FALSE;
    switch (Variant->Type)
    {
        case EvtVarTypeBoolean: *Value = Variant->BooleanVal != 0; break;
        case EvtVarTypeByte:    *Value = Variant->ByteVal; break;
        case EvtVarTypeUInt16:  *Value = Variant->UInt16Val; break;
        case EvtVarTypeUInt32:  *Value = Variant->UInt32Val; break;
        default:                *Value = Variant->UInt64Val; break;
    }
    Free(Variant);
    return TRUE;
}

static PWSTR ReadTextFile(PCWSTR Path)
{
    HANDLE File;
    LARGE_INTEGER Size;
    PBYTE Data = NULL, Start;
    PWSTR Text = NULL;
    DWORD Read, Length, Chars, CodePage = CP_UTF8, Error = ERROR_SUCCESS;

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(File, &Size))
    {
        Error = GetLastError();
        goto done;
    }
    if (Size.QuadPart > MAX_TEXT_FILE)
    {
        Error = ERROR_FILE_TOO_LARGE;
        goto done;
    }
    Length = (DWORD)Size.QuadPart;
    Data = Alloc(Length + sizeof(WCHAR));
    if (!Data)
    {
        Error = ERROR_OUTOFMEMORY;
        goto done;
    }
    if (!ReadFile(File, Data, Length, &Read, NULL) || Read != Length)
    {
        Error = GetLastError();
        if (!Error)
            Error = ERROR_READ_FAULT;
        goto done;
    }

    if (Length >= 2 && Data[0] == 0xFF && Data[1] == 0xFE)
    {
        Chars = (Length - 2) / sizeof(WCHAR);
        Text = Alloc((Chars + 1) * sizeof(WCHAR));
        if (!Text)
            Error = ERROR_OUTOFMEMORY;
        else
            memcpy(Text, Data + 2, Chars * sizeof(WCHAR));
        goto done;
    }

    Start = Data;
    if (Length >= 3 && Data[0] == 0xEF && Data[1] == 0xBB && Data[2] == 0xBF)
    {
        Start += 3;
        Length -= 3;
    }
    Chars = Length ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (PCSTR)Start, Length, NULL, 0) : 0;
    if (!Chars && Length)
    {
        CodePage = CP_ACP;
        Chars = MultiByteToWideChar(CP_ACP, 0, (PCSTR)Start, Length, NULL, 0);
        if (!Chars)
        {
            Error = GetLastError();
            goto done;
        }
    }
    Text = Alloc((Chars + 1) * sizeof(WCHAR));
    if (!Text)
        Error = ERROR_OUTOFMEMORY;
    else if (Chars)
        MultiByteToWideChar(CodePage, CodePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, (PCSTR)Start, Length, Text, Chars);

done:
    Free(Data);
    CloseHandle(File);
    SetLastError(Error);
    return Text;
}

static BOOL WriteTextFile(PCWSTR Path, PCWSTR Text)
{
    static const BYTE Bom[2] = { 0xFF, 0xFE };
    HANDLE File;
    DWORD Written, Length = (DWORD)(wcslen(Text) * sizeof(WCHAR));
    BOOL Ok;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Bom, sizeof(Bom), &Written, NULL) && Written == sizeof(Bom) &&
         WriteFile(File, Text, Length, &Written, NULL) && Written == Length;
    {
        DWORD Error = GetLastError();
        CloseHandle(File);
        SetLastError(Error);
    }
    return Ok;
}

static PWSTR LoadMessageFromSource(PCWSTR Channel, PCWSTR Source, PCWSTR ValueName, DWORD MessageId)
{
    WCHAR KeyName[512];
    PWSTR Files, Path, Next, Result = NULL;
    DWORD Size = 0;

    if (FAILED(StringCchPrintfW(KeyName, ARRAYSIZE(KeyName), L"%ls\\%ls\\%ls", EVENTLOG_KEY, Channel, Source)))
        return NULL;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, KeyName, ValueName, RRF_RT_REG_SZ, NULL, NULL, &Size) != ERROR_SUCCESS)
        return NULL;
    Size += 1024 * sizeof(WCHAR);
    Files = Alloc(Size + sizeof(WCHAR));
    if (!Files)
        return NULL;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, KeyName, ValueName, RRF_RT_REG_SZ, NULL, Files, &Size) != ERROR_SUCCESS)
    {
        Free(Files);
        return NULL;
    }

    for (Path = Files; Path && *Path; Path = Next)
    {
        HMODULE Module;

        Next = wcschr(Path, L';');
        if (Next)
            *Next++ = UNICODE_NULL;
        while (*Path == L' ')
            Path++;
        if (!*Path)
            continue;
        Module = LoadLibraryExW(Path, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (!Module)
            continue;
        if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                           Module, MessageId, 0, (PWSTR)&Result, 0, NULL) && Result)
        {
            FreeLibrary(Module);
            break;
        }
        Result = NULL;
        FreeLibrary(Module);
    }
    Free(Files);
    return Result;
}

static PWSTR ExpandParameters(PCWSTR Text, PCWSTR Channel, PCWSTR Source)
{
    STRBUF Sb = { 0 };
    PCWSTR p = Text;

    while (*p)
    {
        if (p[0] == L'%' && p[1] == L'%' && iswdigit(p[2]))
        {
            PWSTR End, Parameter;
            DWORD Id = wcstoul(p + 2, &End, 10);

            Parameter = LoadMessageFromSource(Channel, Source, L"ParameterMessageFile", Id);
            if (Parameter)
            {
                SIZE_T Length = wcslen(Parameter);
                while (Length && (Parameter[Length - 1] == L'\r' || Parameter[Length - 1] == L'\n'))
                    Parameter[--Length] = UNICODE_NULL;
                SbAppend(&Sb, Parameter);
                LocalFree(Parameter);
            }
            else
            {
                SbAppendN(&Sb, p, End - p);
            }
            p = End;
            continue;
        }
        SbAppendN(&Sb, p, 1);
        p++;
    }
    return SbFinish(&Sb);
}

static PWSTR ExpandMessage(PCWSTR Template, PCWSTR *Strings, DWORD StringCount, PCWSTR Channel, PCWSTR Source)
{
    STRBUF Sb = { 0 };
    PCWSTR p = Template;
    PWSTR Expanded, Result;
    BOOL HasParameters = FALSE;

    while (*p)
    {
        if (*p != L'%')
        {
            SbAppendN(&Sb, p, 1);
            p++;
            continue;
        }
        if (iswdigit(p[1]))
        {
            PCWSTR q = p + 1;
            DWORD Number = *q++ - L'0';

            if (iswdigit(*q))
                Number = Number * 10 + (*q++ - L'0');
            if (*q == L'!')
            {
                PCWSTR Close = wcschr(q + 1, L'!');
                if (Close)
                    q = Close + 1;
            }
            if (Number >= 1 && Number <= StringCount)
            {
                SbAppend(&Sb, Strings[Number - 1]);
                if (wcsstr(Strings[Number - 1], L"%%"))
                    HasParameters = TRUE;
            }
            else
            {
                SbAppendN(&Sb, p, q - p);
            }
            p = q;
            continue;
        }
        switch (p[1])
        {
            case L'n': SbAppend(&Sb, L"\r\n"); break;
            case L'r': SbAppend(&Sb, L"\r"); break;
            case L't': SbAppend(&Sb, L"\t"); break;
            case L'0': goto done;
            case L'%':
                SbAppend(&Sb, L"%");
                if (iswdigit(p[2]))
                {
                    SbAppend(&Sb, L"%");
                    HasParameters = TRUE;
                }
                break;
            case UNICODE_NULL:
                SbAppend(&Sb, L"%");
                p++;
                continue;
            default:
                SbAppendN(&Sb, p, 2);
                break;
        }
        p += 2;
    }

done:
    Expanded = SbFinish(&Sb);
    if (!Expanded || !HasParameters)
        return Expanded;
    Result = ExpandParameters(Expanded, Channel, Source);
    Free(Expanded);
    return Result;
}

static PWSTR GetSourceMessage(PCWSTR Channel, PCWSTR Source, PCWSTR ValueName, DWORD MessageId,
                              PCWSTR *Strings, DWORD StringCount)
{
    PWSTR Template, Result;
    SIZE_T Length;

    Template = LoadMessageFromSource(Channel, Source, ValueName, MessageId);
    if (!Template)
        return NULL;
    Result = ExpandMessage(Template, Strings, StringCount, Channel, Source);
    LocalFree(Template);
    if (!Result)
        return NULL;
    Length = wcslen(Result);
    while (Length && (Result[Length - 1] == L'\r' || Result[Length - 1] == L'\n' ||
                      Result[Length - 1] == L' ' || Result[Length - 1] == L'\t'))
    {
        Result[--Length] = UNICODE_NULL;
    }
    return Result;
}

typedef struct _EVENT_INFO
{
    PEVT_VARIANT System;
    PEVT_VARIANT User;
    PCWSTR *Strings;
    DWORD StringCount;
    PCWSTR Provider;
    PCWSTR Channel;
    PCWSTR Computer;
    DWORD EventId;
    DWORD FullEventId;
    BYTE Level;
    USHORT Task;
    ULONGLONG Keywords;
    ULONGLONG TimeCreated;
    PSID Sid;
    PWSTR Description;
    PWSTR TaskName;
} EVENT_INFO;

static PCWSTR VariantString(const EVT_VARIANT *Variant)
{
    return (Variant->Type == EvtVarTypeString && Variant->StringVal) ? Variant->StringVal : L"";
}

static VOID FreeEventInfo(EVENT_INFO *Info)
{
    Free(Info->System);
    Free(Info->User);
    Free(Info->Strings);
    Free(Info->Description);
    Free(Info->TaskName);
}

static BOOL CollectEventInfo(EVT_HANDLE Event, EVT_HANDLE SystemContext, EVT_HANDLE UserContext, EVENT_INFO *Info)
{
    DWORD Count = 0, i;

    memset(Info, 0, sizeof(*Info));
    Info->System = RenderAlloc(SystemContext, Event, EvtRenderEventValues, &Count);
    if (!Info->System || Count < EvtSystemPropertyIdEND)
    {
        if (Info->System)
            SetLastError(ERROR_INVALID_DATA);
        FreeEventInfo(Info);
        return FALSE;
    }
    Info->User = RenderAlloc(UserContext, Event, EvtRenderEventValues, &Info->StringCount);
    if (!Info->User)
    {
        FreeEventInfo(Info);
        return FALSE;
    }
    Info->Strings = Alloc((Info->StringCount + 1) * sizeof(PCWSTR));
    if (!Info->Strings)
    {
        FreeEventInfo(Info);
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    for (i = 0; i < Info->StringCount; i++)
        Info->Strings[i] = VariantString(&Info->User[i]);

    Info->Provider = VariantString(&Info->System[EvtSystemProviderName]);
    Info->Channel = VariantString(&Info->System[EvtSystemChannel]);
    Info->Computer = VariantString(&Info->System[EvtSystemComputer]);
    Info->EventId = Info->System[EvtSystemEventID].UInt16Val;
    Info->FullEventId = ((DWORD)Info->System[EvtSystemQualifiers].UInt16Val << 16) | Info->EventId;
    Info->Level = Info->System[EvtSystemLevel].ByteVal;
    Info->Task = Info->System[EvtSystemTask].UInt16Val;
    Info->Keywords = Info->System[EvtSystemKeywords].UInt64Val;
    Info->TimeCreated = Info->System[EvtSystemTimeCreated].FileTimeVal;
    if (Info->System[EvtSystemUserID].Type == EvtVarTypeSid)
        Info->Sid = Info->System[EvtSystemUserID].SidVal;

    Info->Description = GetSourceMessage(Info->Channel, Info->Provider, L"EventMessageFile", Info->FullEventId,
                                         Info->Strings, Info->StringCount);
    if (Info->Task)
        Info->TaskName = GetSourceMessage(Info->Channel, Info->Provider, L"CategoryMessageFile", Info->Task, NULL, 0);
    return TRUE;
}

static VOID GetLevelName(BYTE Level, PWSTR Buffer, UINT Count)
{
    UINT Id;

    switch (Level)
    {
        case 1:  Id = IDS_LEVEL_CRITICAL; break;
        case 2:  Id = IDS_LEVEL_ERROR; break;
        case 3:  Id = IDS_LEVEL_WARNING; break;
        case 5:  Id = IDS_LEVEL_VERBOSE; break;
        default: Id = IDS_LEVEL_INFORMATION; break;
    }
    LoadResString(Id, Buffer, Count);
}

typedef struct _KEYWORD_NAME
{
    ULONGLONG Mask;
    UINT Id;
} KEYWORD_NAME;

static const KEYWORD_NAME KeywordNames[] =
{
    { KEYWORD_AUDIT_FAILURE, IDS_KEYWORD_AUDIT_FAILURE },
    { KEYWORD_AUDIT_SUCCESS, IDS_KEYWORD_AUDIT_SUCCESS },
    { KEYWORD_CLASSIC, IDS_KEYWORD_CLASSIC },
};

static VOID PrintXmlEscaped(PCWSTR Text, BOOL Attribute)
{
    PCWSTR Start = Text, p;

    for (p = Text; *p; p++)
    {
        PCWSTR Entity = NULL;

        switch (*p)
        {
            case L'&': Entity = L"&amp;"; break;
            case L'<': Entity = L"&lt;"; break;
            case L'>': Entity = L"&gt;"; break;
            case L'"': if (Attribute) Entity = L"&quot;"; break;
        }
        if (!Entity)
            continue;
        if (p > Start)
            ConWrite(StdOut, Start, (DWORD)(p - Start));
        ConPuts(StdOut, Entity);
        Start = p + 1;
    }
    if (p > Start)
        ConWrite(StdOut, Start, (DWORD)(p - Start));
}

static VOID PrintDescription(const EVENT_INFO *Info, BOOL Xml)
{
    DWORD i;

    if (Info->Description)
    {
        if (Xml)
            PrintXmlEscaped(Info->Description, FALSE);
        else
            ConPrintf(StdOut, L"%ls \n", Info->Description);
        return;
    }
    ConResPrintf(StdOut, IDS_DESC_NOT_FOUND, Info->EventId, Info->Provider);
    for (i = 0; i < Info->StringCount; i++)
    {
        if (Xml)
            PrintXmlEscaped(Info->Strings[i], FALSE);
        else
            ConPuts(StdOut, Info->Strings[i]);
        ConPuts(StdOut, L"\n");
    }
    if (!Xml)
        ConPuts(StdOut, L"\n");
}

static DWORD PrintEventText(EVT_HANDLE Event, EVT_HANDLE SystemContext, EVT_HANDLE UserContext, DWORD Index)
{
    EVENT_INFO Info;
    WCHAR Date[64], Level[64], Keyword[256], Name[256], Domain[256];
    PWSTR SidString = NULL;
    DWORD NameLength = ARRAYSIZE(Name), DomainLength = ARRAYSIZE(Domain);
    SID_NAME_USE Use;
    SIZE_T i;

    if (!CollectEventInfo(Event, SystemContext, UserContext, &Info))
        return FailWith(IDS_ERR_RENDER_EVENT, NULL, GetLastError());

    FormatFileTime(Info.TimeCreated, TRUE, TRUE, Date, ARRAYSIZE(Date));
    GetLevelName(Info.Level, Level, ARRAYSIZE(Level));
    Keyword[0] = UNICODE_NULL;
    for (i = 0; i < ARRAYSIZE(KeywordNames); i++)
    {
        WCHAR Text[64];
        if (!(Info.Keywords & KeywordNames[i].Mask))
            continue;
        LoadResString(KeywordNames[i].Id, Text, ARRAYSIZE(Text));
        StringCchCatW(Keyword, ARRAYSIZE(Keyword), Text);
        StringCchCatW(Keyword, ARRAYSIZE(Keyword), L", ");
    }
    if (Info.Sid)
        ConvertSidToStringSidW(Info.Sid, &SidString);

    ConPrintf(StdOut, L"Event[%lu]\n", Index);
    ConPrintf(StdOut, L"  Log Name: %ls\n", Info.Channel);
    ConPrintf(StdOut, L"  Source: %ls\n", Info.Provider);
    ConPrintf(StdOut, L"  Date: %ls\n", Date);
    ConPrintf(StdOut, L"  Event ID: %lu\n", Info.EventId);
    if (Info.TaskName)
        ConPrintf(StdOut, L"  Task: %ls \n", Info.TaskName);
    else if (Info.Task)
        ConPrintf(StdOut, L"  Task: %u\n", Info.Task);
    else
        ConPuts(StdOut, L"  Task: N/A\n");
    ConPrintf(StdOut, L"  Level: %ls \n", Level);
    ConPuts(StdOut, L"  Opcode: N/A\n");
    ConPrintf(StdOut, L"  Keyword: %ls\n", Keyword[0] ? Keyword : L"N/A");
    ConPrintf(StdOut, L"  User: %ls\n", SidString ? SidString : L"N/A");
    if (Info.Sid && LookupAccountSidW(NULL, Info.Sid, Name, &NameLength, Domain, &DomainLength, &Use))
        ConPrintf(StdOut, L"  User Name: %ls\\%ls\n", Domain, Name);
    else
        ConPuts(StdOut, L"  User Name: N/A\n");
    ConPrintf(StdOut, L"  Computer: %ls\n", Info.Computer);
    ConPuts(StdOut, L"  Description: \n");
    PrintDescription(&Info, FALSE);
    ConPuts(StdOut, L"\n");

    if (SidString)
        LocalFree(SidString);
    FreeEventInfo(&Info);
    return ERROR_SUCCESS;
}

static VOID GetCultureName(PWSTR Buffer, UINT Count)
{
    WCHAR Language[16], Country[16];

    if (GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SISO639LANGNAME, Language, ARRAYSIZE(Language)) &&
        GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, Country, ARRAYSIZE(Country)))
    {
        StringCchPrintfW(Buffer, Count, L"%ls-%ls", Language, Country);
        return;
    }
    StringCchCopyW(Buffer, Count, L"en-US");
}

static DWORD PrintEventXml(EVT_HANDLE Event, EVT_HANDLE SystemContext, EVT_HANDLE UserContext, BOOL Rendered)
{
    PWSTR Xml, End = NULL, p;
    EVENT_INFO Info;
    WCHAR Culture[64], Level[64];
    BOOL AnyKeyword = FALSE;
    SIZE_T i;

    Xml = RenderAlloc(NULL, Event, EvtRenderEventXml, NULL);
    if (!Xml)
        return FailWith(IDS_ERR_RENDER_EVENT, NULL, GetLastError());
    if (!Rendered)
    {
        ConPuts(StdOut, Xml);
        Free(Xml);
        return ERROR_SUCCESS;
    }

    for (p = wcsstr(Xml, L"</Event>"); p; p = wcsstr(p + 1, L"</Event>"))
        End = p;
    if (!End || !CollectEventInfo(Event, SystemContext, UserContext, &Info))
    {
        DWORD Error = End ? GetLastError() : ERROR_INVALID_DATA;
        Free(Xml);
        return FailWith(IDS_ERR_RENDER_EVENT, NULL, Error);
    }
    *End = UNICODE_NULL;
    ConPuts(StdOut, Xml);

    GetCultureName(Culture, ARRAYSIZE(Culture));
    GetLevelName(Info.Level, Level, ARRAYSIZE(Level));
    ConPrintf(StdOut, L"<RenderingInfo Culture='%ls'><Message>", Culture);
    PrintDescription(&Info, TRUE);
    ConPuts(StdOut, L"</Message><Level>");
    PrintXmlEscaped(Level, FALSE);
    ConPuts(StdOut, L"</Level>");
    if (Info.TaskName)
    {
        ConPuts(StdOut, L"<Task>");
        PrintXmlEscaped(Info.TaskName, FALSE);
        ConPuts(StdOut, L"</Task>");
    }
    ConPuts(StdOut, L"<Provider>");
    PrintXmlEscaped(Info.Provider, FALSE);
    ConPuts(StdOut, L"</Provider>");
    for (i = 0; i < ARRAYSIZE(KeywordNames); i++)
    {
        WCHAR Text[64];
        if (!(Info.Keywords & KeywordNames[i].Mask))
            continue;
        if (!AnyKeyword)
            ConPuts(StdOut, L"<Keywords>");
        AnyKeyword = TRUE;
        LoadResString(KeywordNames[i].Id, Text, ARRAYSIZE(Text));
        ConPuts(StdOut, L"<Keyword>");
        PrintXmlEscaped(Text, FALSE);
        ConPuts(StdOut, L"</Keyword>");
    }
    if (AnyKeyword)
        ConPuts(StdOut, L"</Keywords>");
    ConPuts(StdOut, L"</RenderingInfo></Event>");

    FreeEventInfo(&Info);
    Free(Xml);
    return ERROR_SUCCESS;
}

static DWORD CmdEnumLogs(int ArgCount, PWSTR *Args)
{
    EVT_HANDLE Enum;
    PWSTR Name = NULL;
    DWORD Capacity = 0, Used, Error = ERROR_SUCCESS;

    UNREFERENCED_PARAMETER(ArgCount);
    UNREFERENCED_PARAMETER(Args);

    Enum = EvtOpenChannelEnum(Session, 0);
    if (!Enum)
    {
        Error = GetLastError();
        PrintSystemError(Error);
        return Error;
    }
    for (;;)
    {
        if (EvtNextChannelPath(Enum, Capacity, Name, &Used))
        {
            ConPrintf(StdOut, L"%ls\n", Name);
            continue;
        }
        Error = GetLastError();
        if (Error == ERROR_INSUFFICIENT_BUFFER)
        {
            Free(Name);
            Capacity = Used;
            Name = Alloc(Capacity * sizeof(WCHAR));
            if (!Name)
            {
                Error = ERROR_OUTOFMEMORY;
                PrintSystemError(Error);
                break;
            }
            continue;
        }
        if (Error == ERROR_NO_MORE_ITEMS)
        {
            Error = ERROR_SUCCESS;
            break;
        }
        PrintSystemError(Error);
        break;
    }
    Free(Name);
    EvtClose(Enum);
    return Error;
}

static PCWSTR ChannelTypeName(ULONGLONG Type)
{
    switch (Type)
    {
        case 1:  return L"Operational";
        case 2:  return L"Analytic";
        case 3:  return L"Debug";
        default: return L"Admin";
    }
}

static PCWSTR IsolationName(ULONGLONG Isolation)
{
    switch (Isolation)
    {
        case 1:  return L"System";
        case 2:  return L"Custom";
        default: return L"Application";
    }
}

static VOID GetChannelAccess(PCWSTR Name, PWSTR Buffer, SIZE_T Count)
{
    WCHAR KeyName[512];
    DWORD Size = (DWORD)(Count * sizeof(WCHAR));

    if (SUCCEEDED(StringCchPrintfW(KeyName, ARRAYSIZE(KeyName), L"%ls\\%ls", EVENTLOG_KEY, Name)) &&
        RegGetValueW(HKEY_LOCAL_MACHINE, KeyName, L"CustomSD", RRF_RT_REG_SZ, NULL, Buffer, &Size) == ERROR_SUCCESS)
    {
        return;
    }
    StringCchCopyW(Buffer, Count, _wcsicmp(Name, L"Security") ? CLASSIC_SDDL : SECURITY_SDDL);
}

static DWORD CmdGetLog(int ArgCount, PWSTR *Args)
{
    PCWSTR Name = Args[0];
    EVT_HANDLE Config;
    BOOL Xml = FALSE;
    ULONGLONG Enabled, Type, Isolation, Retention, AutoBackup, MaxSize, FileMax;
    WCHAR Publisher[256], Access[1024], FilePath[MAX_PATH], MaxSizeText[32];
    DWORD Error;

    UNREFERENCED_PARAMETER(ArgCount);

    if (OptionValues[OPT_FORMAT])
    {
        if (!_wcsicmp(OptionValues[OPT_FORMAT], L"xml"))
            Xml = TRUE;
        else if (_wcsicmp(OptionValues[OPT_FORMAT], L"text"))
            return UsageError(IDS_ERR_INVALID_VALUE, OptionTyped[OPT_FORMAT]);
    }

    Config = EvtOpenChannelConfig(Session, Name, 0);
    if (!Config)
        return FailWith(IDS_ERR_READ_CONFIG, Name, GetLastError());

    if (!GetChannelNumber(Config, EvtChannelConfigEnabled, &Enabled) ||
        !GetChannelNumber(Config, EvtChannelConfigType, &Type) ||
        !GetChannelNumber(Config, EvtChannelLoggingConfigRetention, &Retention) ||
        !GetChannelNumber(Config, EvtChannelLoggingConfigAutoBackup, &AutoBackup) ||
        !GetChannelNumber(Config, EvtChannelLoggingConfigMaxSize, &MaxSize) ||
        !GetChannelString(Config, EvtChannelLoggingConfigLogFilePath, FilePath, ARRAYSIZE(FilePath)))
    {
        Error = GetLastError();
        EvtClose(Config);
        return FailWith(IDS_ERR_READ_CONFIG, Name, Error);
    }
    GetChannelString(Config, EvtChannelConfigOwningPublisher, Publisher, ARRAYSIZE(Publisher));
    if (!GetChannelNumber(Config, EvtChannelConfigIsolation, &Isolation))
    {
        if (!_wcsicmp(Name, L"Security"))
            Isolation = 2;
        else if (!_wcsicmp(Name, L"System"))
            Isolation = 1;
        else
            Isolation = 0;
    }
    if (!GetChannelString(Config, EvtChannelConfigAccess, Access, ARRAYSIZE(Access)))
        GetChannelAccess(Name, Access, ARRAYSIZE(Access));
    if (!GetChannelNumber(Config, EvtChannelPublishingConfigFileMax, &FileMax))
        FileMax = 1;
    EvtClose(Config);

    StringCchPrintfW(MaxSizeText, ARRAYSIZE(MaxSizeText), L"%I64u", MaxSize);
    if (!Xml)
    {
        ConPrintf(StdOut, L"name: %ls\n", Name);
        ConPrintf(StdOut, L"enabled: %ls\n", Enabled ? L"true" : L"false");
        ConPrintf(StdOut, L"type: %ls\n", ChannelTypeName(Type));
        ConPrintf(StdOut, L"owningPublisher: %ls\n", Publisher);
        ConPrintf(StdOut, L"isolation: %ls\n", IsolationName(Isolation));
        ConPrintf(StdOut, L"channelAccess: %ls\n", Access);
        ConPuts(StdOut, L"logging:\n");
        ConPrintf(StdOut, L"  logFileName: %ls\n", FilePath);
        ConPrintf(StdOut, L"  retention: %ls\n", Retention ? L"true" : L"false");
        ConPrintf(StdOut, L"  autoBackup: %ls\n", AutoBackup ? L"true" : L"false");
        ConPrintf(StdOut, L"  maxSize: %ls\n", MaxSizeText);
        ConPuts(StdOut, L"publishing:\n");
        ConPrintf(StdOut, L"  fileMax: %I64u\n", FileMax);
        return ERROR_SUCCESS;
    }

    ConPuts(StdOut, L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    ConPuts(StdOut, L"<channel name=\"");
    PrintXmlEscaped(Name, TRUE);
    ConPrintf(StdOut, L"\" enabled=\"%ls\" type=\"%ls\" owningPublisher=\"", Enabled ? L"true" : L"false",
              ChannelTypeName(Type));
    PrintXmlEscaped(Publisher, TRUE);
    ConPrintf(StdOut, L"\" isolation=\"%ls\" channelAccess=\"", IsolationName(Isolation));
    PrintXmlEscaped(Access, TRUE);
    ConPuts(StdOut, L"\" xmlns=\"http://schemas.microsoft.com/win/2004/08/events\">\n");
    ConPuts(StdOut, L"  <logging>\n");
    ConPuts(StdOut, L"    <logFileName>");
    PrintXmlEscaped(FilePath, FALSE);
    ConPuts(StdOut, L"</logFileName>\n");
    ConPrintf(StdOut, L"    <retention>%ls</retention>\n", Retention ? L"true" : L"false");
    ConPrintf(StdOut, L"    <autoBackup>%ls</autoBackup>\n", AutoBackup ? L"true" : L"false");
    ConPrintf(StdOut, L"    <maxSize>%ls</maxSize>\n", MaxSizeText);
    ConPuts(StdOut, L"  </logging>\n");
    ConPuts(StdOut, L"  <publishing>\n");
    ConPrintf(StdOut, L"    <fileMax>%I64u</fileMax>\n", FileMax);
    ConPuts(StdOut, L"  </publishing>\n");
    ConPuts(StdOut, L"</channel>\n");
    return ERROR_SUCCESS;
}

static DWORD CmdSetLog(int ArgCount, PWSTR *Args)
{
    PCWSTR Name = Args[0];
    EVT_HANDLE Config;
    EVT_VARIANT Value;
    DWORD Error = ERROR_SUCCESS;
    BOOL Retention;

    UNREFERENCED_PARAMETER(ArgCount);

    /* TODO: /e, /i, /lfn, /ab, /l, /k, /ca and /c need channel properties the API does not expose yet */
    if (OptionValues[OPT_ENABLED] || OptionValues[OPT_ISOLATION] || OptionValues[OPT_LOGFILENAME] ||
        OptionValues[OPT_AUTOBACKUP] || OptionValues[OPT_LEVEL] || OptionValues[OPT_KEYWORDS] ||
        OptionValues[OPT_CHANNELACCESS] || OptionValues[OPT_CONFIG])
    {
        return FailWith(IDS_ERR_SAVE_CONFIG, Name, ERROR_CALL_NOT_IMPLEMENTED);
    }
    if (OptionValues[OPT_MAXSIZE] && !ParseUInt64(OptionValues[OPT_MAXSIZE], &Value.UInt64Val))
        return UsageError(IDS_ERR_INVALID_VALUE, OptionTyped[OPT_MAXSIZE]);
    if (!GetBoolOption(OPT_RETENTION, FALSE, &Retention, &Error))
        return Error;

    Config = EvtOpenChannelConfig(Session, Name, 0);
    if (!Config)
        return FailWith(IDS_ERR_READ_CONFIG, Name, GetLastError());

    if (OptionValues[OPT_MAXSIZE])
    {
        Value.Type = EvtVarTypeUInt64;
        Value.Count = 0;
        if (!EvtSetChannelConfigProperty(Config, EvtChannelLoggingConfigMaxSize, 0, &Value))
            Error = GetLastError();
    }
    if (!Error && OptionValues[OPT_RETENTION])
    {
        memset(&Value, 0, sizeof(Value));
        Value.Type = EvtVarTypeBoolean;
        Value.BooleanVal = Retention;
        if (!EvtSetChannelConfigProperty(Config, EvtChannelLoggingConfigRetention, 0, &Value))
            Error = GetLastError();
    }
    if (!Error && !EvtSaveChannelConfig(Config, 0))
        Error = GetLastError();
    EvtClose(Config);

    if (Error)
        return FailWith(IDS_ERR_SAVE_CONFIG, Name, Error);
    return ERROR_SUCCESS;
}

static DWORD CmdGetLogInfo(int ArgCount, PWSTR *Args)
{
    static const struct { PCWSTR Label; EVT_LOG_PROPERTY_ID Id; } Properties[] =
    {
        { L"creationTime", EvtLogCreationTime },
        { L"lastAccessTime", EvtLogLastAccessTime },
        { L"lastWriteTime", EvtLogLastWriteTime },
        { L"fileSize", EvtLogFileSize },
        { L"attributes", EvtLogAttributes },
        { L"numberOfLogRecords", EvtLogNumberOfLogRecords },
        { L"oldestRecordNumber", EvtLogOldestRecordNumber },
    };
    PCWSTR Name = Args[0];
    EVT_HANDLE Log;
    BOOL LogFile;
    DWORD Error = ERROR_SUCCESS;
    SIZE_T i;

    UNREFERENCED_PARAMETER(ArgCount);

    if (!GetBoolOption(OPT_LOGFILE, FALSE, &LogFile, &Error))
        return Error;
    Log = EvtOpenLog(Session, Name, LogFile ? EvtOpenFilePath : EvtOpenChannelPath);
    if (!Log)
        return FailWith(IDS_ERR_READ_LOG_INFO, Name, GetLastError());

    for (i = 0; i < ARRAYSIZE(Properties); i++)
    {
        PEVT_VARIANT Variant = GetLogProperty(Log, Properties[i].Id);
        WCHAR Text[64];

        if (!Variant)
        {
            Error = GetLastError();
            break;
        }
        FormatVariant(Variant, Text, ARRAYSIZE(Text));
        Free(Variant);
        ConPrintf(StdOut, L"%ls: %ls\n", Properties[i].Label, Text);
    }
    EvtClose(Log);

    if (Error)
        return FailWith(IDS_ERR_READ_LOG_INFO, Name, Error);
    return ERROR_SUCCESS;
}

typedef enum _EVENT_FORMAT
{
    FormatXml,
    FormatText,
    FormatRenderedXml
} EVENT_FORMAT;

static DWORD CmdQueryEvents(int ArgCount, PWSTR *Args)
{
    PCWSTR Path = Args[0];
    PWSTR QueryText = NULL, BookmarkText = NULL;
    EVT_HANDLE Result = NULL, Bookmark = NULL, SaveBookmark = NULL;
    EVT_HANDLE SystemContext = NULL, UserContext = NULL;
    EVENT_FORMAT Format = FormatXml;
    BOOL LogFile, Structured, Reverse;
    ULONGLONG Count = ~0ULL;
    DWORD Flags, Index = 0, Error = ERROR_SUCCESS;

    UNREFERENCED_PARAMETER(ArgCount);

    if (OptionValues[OPT_FORMAT])
    {
        if (!_wcsicmp(OptionValues[OPT_FORMAT], L"text"))
            Format = FormatText;
        else if (!_wcsicmp(OptionValues[OPT_FORMAT], L"renderedxml"))
            Format = FormatRenderedXml;
        else if (_wcsicmp(OptionValues[OPT_FORMAT], L"xml"))
            return UsageError(IDS_ERR_INVALID_VALUE, OptionTyped[OPT_FORMAT]);
    }
    if (OptionValues[OPT_COUNT] && !ParseUInt64(OptionValues[OPT_COUNT], &Count))
        return UsageError(IDS_ERR_INVALID_VALUE, OptionTyped[OPT_COUNT]);
    if (!GetBoolOption(OPT_LOGFILE, FALSE, &LogFile, &Error) ||
        !GetBoolOption(OPT_STRUCTURED, FALSE, &Structured, &Error) ||
        !GetBoolOption(OPT_REVERSE, FALSE, &Reverse, &Error))
    {
        return Error;
    }
    if (Structured && OptionValues[OPT_QUERY])
        return UsageError(IDS_ERR_INVALID_OPTION, OptionTyped[OPT_QUERY]);
    /* TODO: /l selects the message language; messages come from the message file's default language */

    if (Structured)
    {
        QueryText = ReadTextFile(Path);
        if (!QueryText)
            return FailWith(IDS_ERR_READ_FILE, Path, GetLastError());
    }

    Flags = (LogFile ? EvtQueryFilePath : EvtQueryChannelPath) |
            (Reverse ? EvtQueryReverseDirection : EvtQueryForwardDirection) |
            EvtQueryTolerateQueryErrors;
    Result = EvtQuery(Session, Structured ? NULL : Path, QueryText ? QueryText : OptionValues[OPT_QUERY], Flags);
    if (!Result)
    {
        WCHAR Status[1024];
        DWORD StatusLength = 0;

        Error = GetLastError();
        if (EvtGetExtendedStatus(ARRAYSIZE(Status), Status, &StatusLength) == ERROR_SUCCESS && StatusLength > 1)
            ConPrintf(StdErr, L"%ls\n\n", Status);
        FailWith(IDS_ERR_OPEN_QUERY, NULL, Error);
        goto done;
    }

    if (OptionValues[OPT_BOOKMARK])
    {
        BookmarkText = ReadTextFile(OptionValues[OPT_BOOKMARK]);
        if (!BookmarkText)
        {
            Error = FailWith(IDS_ERR_READ_FILE, OptionValues[OPT_BOOKMARK], GetLastError());
            goto done;
        }
        Bookmark = EvtCreateBookmark(BookmarkText);
        if (!Bookmark || !EvtSeek(Result, 1, Bookmark, 0, EvtSeekRelativeToBookmark))
        {
            Error = FailWith(IDS_ERR_OPEN_QUERY, NULL, GetLastError());
            goto done;
        }
    }
    if (OptionValues[OPT_SAVEBOOKMARK])
    {
        SaveBookmark = EvtCreateBookmark(NULL);
        if (!SaveBookmark)
        {
            Error = FailWith(IDS_ERR_OPEN_QUERY, NULL, GetLastError());
            goto done;
        }
    }
    if (Format != FormatXml)
    {
        SystemContext = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
        UserContext = EvtCreateRenderContext(0, NULL, EvtRenderContextUser);
        if (!SystemContext || !UserContext)
        {
            Error = FailWith(IDS_ERR_RENDER_EVENT, NULL, GetLastError());
            goto done;
        }
    }

    if (OptionValues[OPT_ELEMENT] && Format != FormatText)
        ConPrintf(StdOut, L"<%ls>\n", OptionValues[OPT_ELEMENT]);

    while (Index < Count)
    {
        EVT_HANDLE Event;
        DWORD Returned;

        if (!EvtNext(Result, 1, &Event, INFINITE, 0, &Returned))
        {
            Error = GetLastError();
            if (Error == ERROR_NO_MORE_ITEMS)
                Error = ERROR_SUCCESS;
            else
                FailWith(IDS_ERR_NEXT_EVENT, NULL, Error);
            break;
        }
        if (Format == FormatText)
            Error = PrintEventText(Event, SystemContext, UserContext, Index);
        else
            Error = PrintEventXml(Event, SystemContext, UserContext, Format == FormatRenderedXml);
        if (!Error && SaveBookmark)
            EvtUpdateBookmark(SaveBookmark, Event);
        EvtClose(Event);
        if (Error)
            break;
        Index++;
    }

    if (OptionValues[OPT_ELEMENT] && Format != FormatText)
        ConPrintf(StdOut, L"\n</%ls>\n", OptionValues[OPT_ELEMENT]);

    if (!Error && SaveBookmark)
    {
        PWSTR Xml = RenderAlloc(NULL, SaveBookmark, EvtRenderBookmark, NULL);
        if (!Xml || !WriteTextFile(OptionValues[OPT_SAVEBOOKMARK], Xml))
            Error = FailWith(IDS_ERR_WRITE_FILE, OptionValues[OPT_SAVEBOOKMARK], GetLastError());
        Free(Xml);
    }

done:
    if (UserContext) EvtClose(UserContext);
    if (SystemContext) EvtClose(SystemContext);
    if (SaveBookmark) EvtClose(SaveBookmark);
    if (Bookmark) EvtClose(Bookmark);
    if (Result) EvtClose(Result);
    Free(BookmarkText);
    Free(QueryText);
    return Error;
}

static DWORD CmdExportLog(int ArgCount, PWSTR *Args)
{
    PCWSTR Path = Args[0], Target = Args[1];
    PWSTR QueryText = NULL;
    BOOL LogFile, Structured, Overwrite;
    DWORD Flags, Error = ERROR_SUCCESS;

    UNREFERENCED_PARAMETER(ArgCount);

    if (!GetBoolOption(OPT_LOGFILE, FALSE, &LogFile, &Error) ||
        !GetBoolOption(OPT_STRUCTURED, FALSE, &Structured, &Error) ||
        !GetBoolOption(OPT_OVERWRITE, FALSE, &Overwrite, &Error))
    {
        return Error;
    }
    if (Structured && OptionValues[OPT_QUERY])
        return UsageError(IDS_ERR_INVALID_OPTION, OptionTyped[OPT_QUERY]);
    if (Structured)
    {
        /* TODO: a structured query names its channel inside the file; the export API wants a path */
        QueryText = ReadTextFile(Path);
        if (!QueryText)
            return FailWith(IDS_ERR_READ_FILE, Path, GetLastError());
    }

    Flags = (LogFile ? EvtExportLogFilePath : EvtExportLogChannelPath) | (Overwrite ? EvtExportLogOverwrite : 0);
    if (!EvtExportLog(Session, Structured ? NULL : Path, QueryText ? QueryText : OptionValues[OPT_QUERY], Target, Flags))
        Error = FailWith(IDS_ERR_EXPORT_LOG, Path, GetLastError());
    Free(QueryText);
    return Error;
}

static DWORD CmdClearLog(int ArgCount, PWSTR *Args)
{
    PCWSTR Name = Args[0];

    UNREFERENCED_PARAMETER(ArgCount);

    if (!EvtClearLog(Session, Name, OptionValues[OPT_BACKUP], 0))
        return FailWith(IDS_ERR_CLEAR_LOG, Name, GetLastError());
    return ERROR_SUCCESS;
}

static DWORD CmdNotAvailable(int ArgCount, PWSTR *Args)
{
    UNREFERENCED_PARAMETER(ArgCount);
    UNREFERENCED_PARAMETER(Args);

    /* TODO: ep, gp, im, um and al need publisher metadata and manifest support in wevtapi */
    return FailWith(IDS_ERR_COMMAND_UNAVAILABLE, CurrentCommand->Short, ERROR_CALL_NOT_IMPLEMENTED);
}

static const OPTION_ID CommonOptions[] = { OPT_REMOTE, OPT_USER, OPT_PASSWORD, OPT_AUTH, OPT_UNICODE, OPT_MAX };
static const OPTION_ID NoOptions[] = { OPT_MAX };
static const OPTION_ID GetLogOptions[] = { OPT_FORMAT, OPT_MAX };
static const OPTION_ID SetLogOptions[] =
{
    OPT_ENABLED, OPT_ISOLATION, OPT_LOGFILENAME, OPT_RETENTION, OPT_AUTOBACKUP, OPT_MAXSIZE,
    OPT_LEVEL, OPT_KEYWORDS, OPT_CHANNELACCESS, OPT_CONFIG, OPT_MAX
};
static const OPTION_ID QueryOptions[] =
{
    OPT_LOGFILE, OPT_STRUCTURED, OPT_QUERY, OPT_BOOKMARK, OPT_SAVEBOOKMARK, OPT_REVERSE,
    OPT_FORMAT, OPT_LOCALE, OPT_COUNT, OPT_ELEMENT, OPT_MAX
};
static const OPTION_ID LogInfoOptions[] = { OPT_LOGFILE, OPT_MAX };
static const OPTION_ID ExportOptions[] = { OPT_LOGFILE, OPT_STRUCTURED, OPT_QUERY, OPT_OVERWRITE, OPT_MAX };
static const OPTION_ID ClearOptions[] = { OPT_BACKUP, OPT_MAX };

static const COMMAND Commands[] =
{
    { L"el",  L"enum-logs",          IDS_HELP_EL,  0, 0, NoOptions,      CmdEnumLogs },
    { L"gl",  L"get-log",            IDS_HELP_GL,  1, 1, GetLogOptions,  CmdGetLog },
    { L"sl",  L"set-log",            IDS_HELP_SL,  1, 1, SetLogOptions,  CmdSetLog },
    { L"ep",  L"enum-publishers",    IDS_HELP_NOT_SUPPORTED, 0, 0, NoOptions, CmdNotAvailable },
    { L"gp",  L"get-publisher",      IDS_HELP_NOT_SUPPORTED, 1, 1, NoOptions, CmdNotAvailable },
    { L"im",  L"install-manifest",   IDS_HELP_NOT_SUPPORTED, 1, 1, NoOptions, CmdNotAvailable },
    { L"um",  L"uninstall-manifest", IDS_HELP_NOT_SUPPORTED, 1, 1, NoOptions, CmdNotAvailable },
    { L"qe",  L"query-events",       IDS_HELP_QE,  1, 1, QueryOptions,   CmdQueryEvents },
    { L"gli", L"get-log-info",       IDS_HELP_GLI, 1, 1, LogInfoOptions, CmdGetLogInfo },
    { L"gli", L"get-loginfo",        IDS_HELP_GLI, 1, 1, LogInfoOptions, CmdGetLogInfo },
    { L"epl", L"export-log",         IDS_HELP_EPL, 2, 2, ExportOptions,  CmdExportLog },
    { L"al",  L"archive-log",        IDS_HELP_NOT_SUPPORTED, 1, 1, NoOptions, CmdNotAvailable },
    { L"cl",  L"clear-log",          IDS_HELP_CL,  1, 1, ClearOptions,   CmdClearLog },
};

static const COMMAND *FindCommand(PCWSTR Name)
{
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(Commands); i++)
    {
        if (!_wcsicmp(Name, Commands[i].Short) || !_wcsicmp(Name, Commands[i].Long))
            return &Commands[i];
    }
    return NULL;
}

static BOOL OptionMatches(OPTION_ID Id, PCWSTR Name, SIZE_T Length)
{
    return (wcslen(OptionNames[Id].Short) == Length && !_wcsnicmp(Name, OptionNames[Id].Short, Length)) ||
           (wcslen(OptionNames[Id].Long) == Length && !_wcsnicmp(Name, OptionNames[Id].Long, Length));
}

static OPTION_ID FindOption(const COMMAND *Command, PCWSTR Name, SIZE_T Length)
{
    const OPTION_ID *Id;

    for (Id = Command->Options; *Id != OPT_MAX; Id++)
    {
        if (OptionMatches(*Id, Name, Length))
            return *Id;
    }
    for (Id = CommonOptions; *Id != OPT_MAX; Id++)
    {
        if (OptionMatches(*Id, Name, Length))
            return *Id;
    }
    return OPT_MAX;
}

static DWORD OpenRemoteSession(VOID)
{
    EVT_RPC_LOGIN Login;
    WCHAR User[256] = L"";
    PWSTR Separator;

    memset(&Login, 0, sizeof(Login));
    Login.Server = (PWSTR)OptionValues[OPT_REMOTE];
    if (OptionValues[OPT_USER])
    {
        StringCchCopyW(User, ARRAYSIZE(User), OptionValues[OPT_USER]);
        Separator = wcschr(User, L'\\');
        if (Separator)
        {
            *Separator = UNICODE_NULL;
            Login.Domain = User;
            Login.User = Separator + 1;
        }
        else
        {
            Login.User = User;
        }
        /* TODO: a missing password or "*" should prompt on the console */
        Login.Password = (PWSTR)OptionValues[OPT_PASSWORD];
    }
    Login.Flags = EvtRpcLoginAuthNegotiate;
    if (OptionValues[OPT_AUTH])
    {
        if (!_wcsicmp(OptionValues[OPT_AUTH], L"Default"))
            Login.Flags = EvtRpcLoginAuthDefault;
        else if (!_wcsicmp(OptionValues[OPT_AUTH], L"Kerberos"))
            Login.Flags = EvtRpcLoginAuthKerberos;
        else if (!_wcsicmp(OptionValues[OPT_AUTH], L"NTLM"))
            Login.Flags = EvtRpcLoginAuthNTLM;
        else if (_wcsicmp(OptionValues[OPT_AUTH], L"Negotiate"))
            return UsageError(IDS_ERR_INVALID_VALUE, OptionTyped[OPT_AUTH]);
    }

    Session = EvtOpenSession(EvtRpcLogin, &Login, 0, 0);
    if (!Session)
        return FailWith(IDS_ERR_OPEN_SESSION, Login.Server, GetLastError());
    return ERROR_SUCCESS;
}

int wmain(int argc, WCHAR *argv[])
{
    const COMMAND *Command;
    PWSTR Positional[MAX_POSITIONAL];
    UINT PositionalCount = 0;
    BOOL Unicode;
    DWORD Error = ERROR_SUCCESS;
    int i;

    ConInitStdStreams();

    if (argc < 2)
    {
        ConResPuts(StdErr, IDS_ERR_NO_COMMAND);
        ConResPuts(StdOut, IDS_HELP_MAIN);
        return ERROR_SUCCESS;
    }
    if (!wcscmp(argv[1], L"/?"))
    {
        ConResPuts(StdOut, IDS_HELP_MAIN);
        return ERROR_SUCCESS;
    }
    Command = FindCommand(argv[1]);
    if (!Command)
        return UsageError(IDS_ERR_BAD_COMMAND, argv[1]);
    CurrentCommand = Command;

    for (i = 2; i < argc; i++)
    {
        PCWSTR Argument = argv[i], Colon;
        SIZE_T Length;
        OPTION_ID Id;

        if (!wcscmp(Argument, L"/?"))
        {
            ConResPuts(StdOut, Command->HelpId);
            return ERROR_SUCCESS;
        }
        if (Argument[0] != L'/')
        {
            if (PositionalCount >= Command->MaxArgs)
                return UsageError(IDS_ERR_EXTRA_ARG, Argument);
            Positional[PositionalCount++] = argv[i];
            continue;
        }
        Argument++;
        Colon = wcschr(Argument, L':');
        Length = Colon ? (SIZE_T)(Colon - Argument) : wcslen(Argument);
        Id = FindOption(Command, Argument, Length);
        if (Id == OPT_MAX || Length >= ARRAYSIZE(OptionTyped[0]))
        {
            WCHAR Typed[64];
            StringCchCopyNW(Typed, ARRAYSIZE(Typed), Argument, Length);
            return UsageError(IDS_ERR_INVALID_OPTION, Typed);
        }
        StringCchCopyNW(OptionTyped[Id], ARRAYSIZE(OptionTyped[Id]), Argument, Length);
        OptionValues[Id] = Colon ? Colon + 1 : L"";
    }
    if (PositionalCount < Command->MinArgs)
        return UsageError(IDS_ERR_MISSING_ARGS, NULL);

    if (!GetBoolOption(OPT_UNICODE, FALSE, &Unicode, &Error))
        return Error;
    if (Unicode)
        ConStreamSetMode(StdOut, UTF16Text, 0);

    if (OptionValues[OPT_REMOTE])
    {
        Error = OpenRemoteSession();
        if (Error)
            return Error;
    }

    Error = Command->Proc(PositionalCount, Positional);

    if (Session)
        EvtClose(Session);
    return Error;
}

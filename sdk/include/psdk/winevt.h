/* Definitions for the Windows Event Log (wevtapi.dll)
 *
 * Copyright 2012 André Hentschel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_WINEVT_H
#define __WINE_WINEVT_H

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif /* defined(__cplusplus) */

typedef HANDLE EVT_HANDLE, *PEVT_HANDLE;

typedef enum _EVT_CHANNEL_CONFIG_PROPERTY_ID {
    EvtChannelConfigEnabled = 0,
    EvtChannelConfigIsolation,
    EvtChannelConfigType,
    EvtChannelConfigOwningPublisher,
    EvtChannelConfigClassicEventlog,
    EvtChannelConfigAccess,
    EvtChannelLoggingConfigRetention,
    EvtChannelLoggingConfigAutoBackup,
    EvtChannelLoggingConfigMaxSize,
    EvtChannelLoggingConfigLogFilePath,
    EvtChannelPublishingConfigLevel,
    EvtChannelPublishingConfigKeywords,
    EvtChannelPublishingConfigControlGuid,
    EvtChannelPublishingConfigBufferSize,
    EvtChannelPublishingConfigMinBuffers,
    EvtChannelPublishingConfigMaxBuffers,
    EvtChannelPublishingConfigLatency,
    EvtChannelPublishingConfigClockType,
    EvtChannelPublishingConfigSidType,
    EvtChannelPublisherList,
    EvtChannelPublishingConfigFileMax,
    EvtChannelConfigPropertyIdEND
} EVT_CHANNEL_CONFIG_PROPERTY_ID;

typedef enum _EVT_LOGIN_CLASS {
    EvtRpcLogin = 1
} EVT_LOGIN_CLASS;

typedef enum _EVT_QUERY_FLAGS {
    EvtQueryChannelPath         = 0x1,
    EvtQueryFilePath            = 0x2,
    EvtQueryForwardDirection    = 0x100,
    EvtQueryReverseDirection    = 0x200,
    EvtQueryTolerateQueryErrors = 0x1000
} EVT_QUERY_FLAGS;

typedef enum _EVT_SEEK_FLAGS {
    EvtSeekRelativeToFirst    = 1,
    EvtSeekRelativeToLast     = 2,
    EvtSeekRelativeToCurrent  = 3,
    EvtSeekRelativeToBookmark = 4,
    EvtSeekOriginMask         = 7,
    EvtSeekStrict             = 0x10000
} EVT_SEEK_FLAGS;

typedef enum _EVT_SUBSCRIBE_FLAGS {
    EvtSubscribeToFutureEvents      = 1,
    EvtSubscribeStartAtOldestRecord = 2,
    EvtSubscribeStartAfterBookmark  = 3,
    EvtSubscribeOriginMask          = 3,
    EvtSubscribeTolerateQueryErrors = 0x1000,
    EvtSubscribeStrict              = 0x10000
} EVT_SUBSCRIBE_FLAGS;

typedef enum _EVT_SYSTEM_PROPERTY_ID {
    EvtSystemProviderName = 0,
    EvtSystemProviderGuid,
    EvtSystemEventID,
    EvtSystemQualifiers,
    EvtSystemLevel,
    EvtSystemTask,
    EvtSystemOpcode,
    EvtSystemKeywords,
    EvtSystemTimeCreated,
    EvtSystemEventRecordId,
    EvtSystemActivityID,
    EvtSystemRelatedActivityID,
    EvtSystemProcessID,
    EvtSystemThreadID,
    EvtSystemChannel,
    EvtSystemComputer,
    EvtSystemUserID,
    EvtSystemVersion,
    EvtSystemPropertyIdEND
} EVT_SYSTEM_PROPERTY_ID;

typedef enum _EVT_RENDER_CONTEXT_FLAGS {
    EvtRenderContextValues = 0,
    EvtRenderContextSystem,
    EvtRenderContextUser
} EVT_RENDER_CONTEXT_FLAGS;

typedef enum _EVT_RENDER_FLAGS {
    EvtRenderEventValues = 0,
    EvtRenderEventXml,
    EvtRenderBookmark
} EVT_RENDER_FLAGS;

typedef enum _EVT_OPEN_LOG_FLAGS {
    EvtOpenChannelPath = 0x1,
    EvtOpenFilePath    = 0x2
} EVT_OPEN_LOG_FLAGS;

typedef enum _EVT_LOG_PROPERTY_ID {
    EvtLogCreationTime = 0,
    EvtLogLastAccessTime,
    EvtLogLastWriteTime,
    EvtLogFileSize,
    EvtLogAttributes,
    EvtLogNumberOfLogRecords,
    EvtLogOldestRecordNumber,
    EvtLogFull
} EVT_LOG_PROPERTY_ID;

typedef enum _EVT_EXPORTLOG_FLAGS {
    EvtExportLogChannelPath         = 0x1,
    EvtExportLogFilePath            = 0x2,
    EvtExportLogTolerateQueryErrors = 0x1000,
    EvtExportLogOverwrite           = 0x2000
} EVT_EXPORTLOG_FLAGS;

typedef enum _EVT_FORMAT_MESSAGE_FLAGS {
    EvtFormatMessageEvent    = 1,
    EvtFormatMessageLevel    = 2,
    EvtFormatMessageTask     = 3,
    EvtFormatMessageOpcode   = 4,
    EvtFormatMessageKeyword  = 5,
    EvtFormatMessageChannel  = 6,
    EvtFormatMessageProvider = 7,
    EvtFormatMessageId       = 8,
    EvtFormatMessageXml      = 9
} EVT_FORMAT_MESSAGE_FLAGS;

typedef enum _EVT_PUBLISHER_METADATA_PROPERTY_ID {
    EvtPublisherMetadataPublisherGuid = 0,
    EvtPublisherMetadataResourceFilePath,
    EvtPublisherMetadataParameterFilePath,
    EvtPublisherMetadataMessageFilePath,
    EvtPublisherMetadataHelpLink,
    EvtPublisherMetadataPublisherMessageID,
    EvtPublisherMetadataChannelReferences,
    EvtPublisherMetadataChannelReferencePath,
    EvtPublisherMetadataChannelReferenceIndex,
    EvtPublisherMetadataChannelReferenceID,
    EvtPublisherMetadataChannelReferenceFlags,
    EvtPublisherMetadataChannelReferenceMessageID,
    EvtPublisherMetadataLevels,
    EvtPublisherMetadataLevelName,
    EvtPublisherMetadataLevelValue,
    EvtPublisherMetadataLevelMessageID,
    EvtPublisherMetadataTasks,
    EvtPublisherMetadataTaskName,
    EvtPublisherMetadataTaskEventGuid,
    EvtPublisherMetadataTaskValue,
    EvtPublisherMetadataTaskMessageID,
    EvtPublisherMetadataOpcodes,
    EvtPublisherMetadataOpcodeName,
    EvtPublisherMetadataOpcodeValue,
    EvtPublisherMetadataOpcodeMessageID,
    EvtPublisherMetadataKeywords,
    EvtPublisherMetadataKeywordName,
    EvtPublisherMetadataKeywordValue,
    EvtPublisherMetadataKeywordMessageID,
    EvtPublisherMetadataPropertyIdEND
} EVT_PUBLISHER_METADATA_PROPERTY_ID;

typedef enum _EVT_RPC_LOGIN_FLAGS {
    EvtRpcLoginAuthDefault = 0,
    EvtRpcLoginAuthNegotiate,
    EvtRpcLoginAuthKerberos,
    EvtRpcLoginAuthNTLM
} EVT_RPC_LOGIN_FLAGS;

typedef struct _EVT_RPC_LOGIN {
    LPWSTR Server;
    LPWSTR User;
    LPWSTR Domain;
    LPWSTR Password;
    DWORD  Flags;
} EVT_RPC_LOGIN;

typedef enum _EVT_SUBSCRIBE_NOTIFY_ACTION {
    EvtSubscribeActionError = 0,
    EvtSubscribeActionDeliver
} EVT_SUBSCRIBE_NOTIFY_ACTION;

typedef enum _EVT_VARIANT_TYPE {
    EvtVarTypeNull,
    EvtVarTypeString,
    EvtVarTypeAnsiString,
    EvtVarTypeSByte,
    EvtVarTypeByte,
    EvtVarTypeInt16,
    EvtVarTypeUInt16,
    EvtVarTypeInt32,
    EvtVarTypeUInt32,
    EvtVarTypeInt64,
    EvtVarTypeUInt64,
    EvtVarTypeSingle,
    EvtVarTypeDouble,
    EvtVarTypeBoolean,
    EvtVarTypeBinary,
    EvtVarTypeGuid,
    EvtVarTypeSizeT,
    EvtVarTypeFileTime,
    EvtVarTypeSysTime,
    EvtVarTypeSid,
    EvtVarTypeHexInt32,
    EvtVarTypeHexInt64,
    EvtVarTypeEvtHandle = 32,
    EvtVarTypeEvtXml = 35
} EVT_VARIANT_TYPE;

#define EVT_VARIANT_TYPE_MASK  0x7f
#define EVT_VARIANT_TYPE_ARRAY 128

typedef struct _EVT_VARIANT {
    union {
        BOOL        BooleanVal;
        INT8        SByteVal;
        INT16       Int16Val;
        INT32       Int32Val;
        INT64       Int64Val;
        UINT8       ByteVal;
        UINT16      UInt16Val;
        UINT32      UInt32Val;
        UINT64      UInt64Val;
        FLOAT       SingleVal;
        DOUBLE      DoubleVal;
        ULONGLONG   FileTimeVal;
        SYSTEMTIME* SysTimeVal;
        GUID*       GuidVal;
        LPCWSTR     StringVal;
        LPCSTR      AnsiStringVal;
        PBYTE       BinaryVal;
        PSID        SidVal;
        SIZE_T      SizeTVal;

        BOOL*       BooleanArr;
        INT8*       SByteArr;
        INT16*      Int16Arr;
        INT32*      Int32Arr;
        INT64*      Int64Arr;
        UINT8*      ByteArr;
        UINT16*     UInt16Arr;
        UINT32*     UInt32Arr;
        UINT64*     UInt64Arr;
        FLOAT*      SingleArr;
        DOUBLE*     DoubleArr;
        FILETIME*   FileTimeArr;
        SYSTEMTIME* SysTimeArr;
        GUID*       GuidArr;
        LPWSTR*     StringArr;
        LPSTR*      AnsiStringArr;
        PSID*       SidArr;
        SIZE_T*     SizeTArr;

        EVT_HANDLE  EvtHandleVal;
        LPCWSTR     XmlVal;
        LPCWSTR*    XmlValArr;
    } DUMMYUNIONNAME;

    DWORD Count;
    DWORD Type;
} EVT_VARIANT, *PEVT_VARIANT;

typedef DWORD (WINAPI *EVT_SUBSCRIBE_CALLBACK)(EVT_SUBSCRIBE_NOTIFY_ACTION Action,
                                               PVOID UserContext, EVT_HANDLE Event);
EVT_HANDLE WINAPI EvtCreateBookmark(LPCWSTR);
EVT_HANDLE WINAPI EvtCreateRenderContext(DWORD,LPCWSTR*,DWORD);
BOOL WINAPI EvtClose(EVT_HANDLE);
BOOL WINAPI EvtExportLog(EVT_HANDLE session, const WCHAR *path, const WCHAR *query,
                         const WCHAR *file, DWORD flags);
BOOL WINAPI EvtGetChannelConfigProperty(EVT_HANDLE ChannelConfig,
                                        EVT_CHANNEL_CONFIG_PROPERTY_ID PropertyId,
                                        DWORD Flags, DWORD PropertyValueBufferSize,
                                        PEVT_VARIANT PropertyValueBuffer,
                                        PDWORD PropertyValueBufferUsed);
BOOL WINAPI EvtSetChannelConfigProperty(EVT_HANDLE ChannelConfig,
                                        EVT_CHANNEL_CONFIG_PROPERTY_ID PropertyId,
                                        DWORD Flags, PEVT_VARIANT PropertyValue);
EVT_HANDLE WINAPI EvtSubscribe(EVT_HANDLE Session, HANDLE SignalEvent, LPCWSTR ChannelPath,
                               LPCWSTR Query, EVT_HANDLE Bookmark, PVOID context,
                               EVT_SUBSCRIBE_CALLBACK Callback, DWORD Flags);
EVT_HANDLE WINAPI EvtOpenChannelConfig(EVT_HANDLE Session, LPCWSTR ChannelPath, DWORD Flags);
BOOL WINAPI EvtSaveChannelConfig(EVT_HANDLE,DWORD);
BOOL WINAPI EvtRender(EVT_HANDLE, EVT_HANDLE, DWORD, DWORD, void *, DWORD *, DWORD *);

BOOL WINAPI EvtCancel(EVT_HANDLE);
BOOL WINAPI EvtClearLog(EVT_HANDLE session, LPCWSTR channel, LPCWSTR target, DWORD flags);
DWORD WINAPI EvtGetExtendedStatus(DWORD buffer_size, LPWSTR buffer, PDWORD buffer_used);
BOOL WINAPI EvtGetLogInfo(EVT_HANDLE log, EVT_LOG_PROPERTY_ID id, DWORD buffer_size,
                          PEVT_VARIANT buffer, PDWORD buffer_used);
BOOL WINAPI EvtNext(EVT_HANDLE result_set, DWORD size, PEVT_HANDLE events, DWORD timeout,
                    DWORD flags, PDWORD returned);
BOOL WINAPI EvtNextChannelPath(EVT_HANDLE channel_enum, DWORD buffer_size, LPWSTR buffer,
                               PDWORD buffer_used);
EVT_HANDLE WINAPI EvtOpenChannelEnum(EVT_HANDLE session, DWORD flags);
EVT_HANDLE WINAPI EvtOpenLog(EVT_HANDLE session, LPCWSTR path, DWORD flags);
EVT_HANDLE WINAPI EvtOpenSession(EVT_LOGIN_CLASS login_class, PVOID login, DWORD timeout, DWORD flags);
EVT_HANDLE WINAPI EvtQuery(EVT_HANDLE session, LPCWSTR path, LPCWSTR query, DWORD flags);
BOOL WINAPI EvtSeek(EVT_HANDLE result_set, LONGLONG position, EVT_HANDLE bookmark, DWORD timeout,
                    DWORD flags);
BOOL WINAPI EvtUpdateBookmark(EVT_HANDLE bookmark, EVT_HANDLE event);
EVT_HANDLE WINAPI EvtOpenPublisherMetadata(EVT_HANDLE session, LPCWSTR publisher_id, LPCWSTR logfile_path,
                                           LCID locale, DWORD flags);
BOOL WINAPI EvtGetPublisherMetadataProperty(EVT_HANDLE publisher, EVT_PUBLISHER_METADATA_PROPERTY_ID id,
                                            DWORD flags, DWORD buffer_size, PEVT_VARIANT buffer, PDWORD buffer_used);
BOOL WINAPI EvtFormatMessage(EVT_HANDLE publisher, EVT_HANDLE event, DWORD message_id, DWORD value_count,
                             PEVT_VARIANT values, DWORD flags, DWORD buffer_size, LPWSTR buffer, PDWORD buffer_used);

#ifdef __cplusplus
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* __WINE_WINEVT_H */

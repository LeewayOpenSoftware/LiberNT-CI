/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMB2 client engine: connect, negotiate, NTLM session setup (SSPI), tree connect, file operations
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef _SMB2CLIENT_H
#define _SMB2CLIENT_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include "smb2proto.h"

#define SMB2_MAX_IO     65536

/* Directory entry decoded from FileDirectoryInformation */
typedef struct _SMB2_DIRENT
{
    uint64_t EndOfFile;
    uint64_t AllocationSize;
    uint64_t LastWriteTime;     /* FILETIME units */
    uint32_t Attributes;
    WCHAR Name[260];
} SMB2_DIRENT;

typedef int (*SMB2_DIR_CALLBACK)(const SMB2_DIRENT *Entry, void *Context);

typedef struct _SMB2_CONN SMB2_CONN;

/* All functions return an NTSTATUS-style value (SMB2_STATUS_SUCCESS on success). */
uint32_t Smb2Connect(const WCHAR *Host, unsigned short Port, SMB2_CONN **Conn);
void Smb2Disconnect(SMB2_CONN *Conn);
uint32_t Smb2Negotiate(SMB2_CONN *Conn);
/* User == NULL: use the caller's default credentials. */
uint32_t Smb2SessionSetup(SMB2_CONN *Conn, const WCHAR *Host, const WCHAR *User, const WCHAR *Domain, const WCHAR *Password);
uint32_t Smb2TreeConnect(SMB2_CONN *Conn, const WCHAR *Host, const WCHAR *Share, uint32_t *TreeId);
uint32_t Smb2TreeDisconnect(SMB2_CONN *Conn, uint32_t TreeId);
uint32_t Smb2Logoff(SMB2_CONN *Conn);

typedef struct _SMB2_CREATE_RESULT
{
    SMB2_FILE_ID FileId;
    uint64_t CreationTime;
    uint64_t LastAccessTime;
    uint64_t LastWriteTime;
    uint64_t ChangeTime;
    uint64_t AllocationSize;
    uint64_t EndOfFile;
    uint32_t Attributes;
    uint32_t CreateAction;
} SMB2_CREATE_RESULT;

uint32_t Smb2CreateEx(SMB2_CONN *Conn, uint32_t TreeId, const WCHAR *Path, uint32_t DesiredAccess,
                      uint32_t ShareAccess, uint32_t FileAttributes, uint32_t CreateDisposition,
                      uint32_t CreateOptions, SMB2_CREATE_RESULT *Result);

/*
 * One QUERY_DIRECTORY round with a caller-chosen information class. Flags: SMB2_RESTART_SCANS,
 * SMB2_RETURN_SINGLE_ENTRY. Pattern NULL means "*". Returns SMB2_STATUS_NO_MORE_FILES at the end.
 */
uint32_t Smb2QueryDirectoryRaw(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint8_t InfoClass,
                               uint8_t Flags, const WCHAR *Pattern, uint32_t OutputLength, void *Buffer,
                               uint32_t *Returned);

/* Path is relative to the share, backslash separated, no leading separator ("" is the share root). */
uint32_t Smb2Create(SMB2_CONN *Conn, uint32_t TreeId, const WCHAR *Path, uint32_t DesiredAccess,
                    uint32_t CreateDisposition, uint32_t CreateOptions, SMB2_FILE_ID *FileId,
                    uint64_t *EndOfFile, uint32_t *Attributes);
uint32_t Smb2Close(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId);
uint32_t Smb2Read(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint64_t Offset,
                  uint32_t Length, void *Buffer, uint32_t *BytesRead);
uint32_t Smb2QueryDirectory(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId,
                            SMB2_DIR_CALLBACK Callback, void *Context);

/* Write at most SMB2_MAX_IO bytes; Written is what the server accepted */
uint32_t Smb2Write(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint64_t Offset,
                   const void *Data, uint32_t Length, uint32_t *Written);
uint32_t Smb2Flush(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId);
/* InfoType SMB2_0_INFO_FILE with a FILE_*_INFORMATION class and its MS-FSCC buffer */
uint32_t Smb2SetInfo(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint8_t InfoClass,
                     const void *Buffer, uint32_t BufferLength);

/* One byte range; Flags is SMB2_LOCKFLAG_SHARED, _EXCLUSIVE or _UNLOCK. Never waits for the range. */
uint32_t Smb2Lock(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint64_t Offset, uint64_t Length,
                  uint32_t Flags);

/*
 * A lock request the server may hold until the range is free. Smb2OpInit before, Smb2OpCancel from any thread
 * (it sends SMB2 CANCEL; the wait then ends with STATUS_CANCELLED). Needs the multiplexed session (after
 * Smb2SessionSetup), else STATUS_NOT_SUPPORTED.
 */
typedef struct _SMB2_OP
{
    SMB2_CONN *Conn;
    volatile LONG Cancelled;
    volatile LONG64 MessageId;
} SMB2_OP;

void Smb2OpInit(SMB2_OP *Op, SMB2_CONN *Conn);
void Smb2OpCancel(SMB2_OP *Op);
uint32_t Smb2LockWait(SMB2_CONN *Conn, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint64_t Offset, uint64_t Length,
                      uint32_t Flags, SMB2_OP *Op);

uint16_t Smb2ConnDialect(const SMB2_CONN *Conn);
int Smb2ConnSigned(const SMB2_CONN *Conn);

/* Signing primitives (smb2sign.c) */
void Smb2HmacSha256(const uint8_t *Key, size_t KeyLen, const uint8_t *Msg, size_t MsgLen, uint8_t Out[32]);
void Smb2SignMessage(const uint8_t *Key16, uint8_t *Msg, size_t Len);
int Smb2VerifyMessage(const uint8_t *Key16, const uint8_t *Msg, size_t Len);

#endif /* _SMB2CLIENT_H */

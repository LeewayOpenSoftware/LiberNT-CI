/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMB2 mini-redirector internal declarations
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef _SMB2RDR_H
#define _SMB2RDR_H

#include <rx.h>
#include <ntstrsafe.h>

#include "smb2rdr_ioctl.h"

#define SMB2RDR_TAG         '2bmS'
#define SMB2RDR_TAG_UP      'puMS'
#define SMB2RDR_MAX_SERVER_NAME     256     /* characters */
#define SMB2RDR_UPCALL_TIMEOUT_SEC  30
#define SMB2RDR_LOCK_POLL_MS        100

typedef struct _SMB2RDR_SERVER
{
    PMRX_SRV_CALL SrvCall;
    UNICODE_STRING Name;                    /* without the leading backslash */
    WCHAR NameBuffer[SMB2RDR_MAX_SERVER_NAME];
} SMB2RDR_SERVER, *PSMB2RDR_SERVER;

typedef struct _SMB2RDR_DEVICE_EXTENSION
{
    NODE_TYPE_CODE NodeTypeCode;
    NODE_BYTE_SIZE NodeByteSize;
    PRDBSS_DEVICE_OBJECT DeviceObject;
} SMB2RDR_DEVICE_EXTENSION, *PSMB2RDR_DEVICE_EXTENSION;

#define NTC_SMB2RDR_DEVICE_EXTENSION    ((NODE_TYPE_CODE)0xFC10)

/* Per-VNetRoot (per user and share) state: the daemon's tree handle */
typedef struct _SMB2RDR_VNETROOT_EXT
{
    NODE_TYPE_CODE NodeTypeCode;
    NODE_BYTE_SIZE NodeByteSize;
    ULONGLONG TreeHandle;
} SMB2RDR_VNETROOT_EXT, *PSMB2RDR_VNETROOT_EXT;

#define Smb2GetVNetRootExt(VNetRoot) \
    (((VNetRoot) == NULL) ? NULL : (PSMB2RDR_VNETROOT_EXT)((VNetRoot)->Context))

/* A byte range this open holds on the server, kept to answer unlock-all requests */
typedef struct _SMB2RDR_LOCK
{
    struct _SMB2RDR_LOCK *Next;
    PEPROCESS Process;
    ULONG Key;
    ULONGLONG Offset;
    ULONGLONG Length;
} SMB2RDR_LOCK, *PSMB2RDR_LOCK;

#define SMB2RDR_NOTIFY_SLOTS    8
#define SMB2RDR_SLOT_RESERVED   (-1LL)

typedef struct _SMB2RDR_CANCEL_SLOTS
{
    volatile LONG Closing;
    volatile LONGLONG Id[SMB2RDR_NOTIFY_SLOTS];
} SMB2RDR_CANCEL_SLOTS, *PSMB2RDR_CANCEL_SLOTS;

/* Per-open state: the daemon's file handle */
typedef struct _SMB2RDR_FOBX_EXT
{
    NODE_TYPE_CODE NodeTypeCode;
    NODE_BYTE_SIZE NodeByteSize;
    ULONGLONG FileHandle;
    BOOLEAN IsDirectory;
    PSMB2RDR_LOCK Locks;
    SMB2RDR_CANCEL_SLOTS Notify;
} SMB2RDR_FOBX_EXT, *PSMB2RDR_FOBX_EXT;

#define Smb2GetFobxExt(Fobx) \
    (((Fobx) == NULL) ? NULL : (PSMB2RDR_FOBX_EXT)((Fobx)->Context))

extern PRDBSS_DEVICE_OBJECT Smb2RdrDevice;

/* upcall.c: kernel -> daemon transport */
VOID Smb2UpcallInit(VOID);
VOID Smb2UpcallShutdown(VOID);
BOOLEAN Smb2UpcallDaemonRunning(VOID);
VOID Smb2UpcallSetRunning(BOOLEAN Running);

/* Sends a request to the daemon and waits for its reply (STATUS_IO_TIMEOUT after the upcall timeout).
 * Reply may be NULL when no payload is expected. */
NTSTATUS Smb2Upcall(ULONG Opcode, const VOID *Payload, ULONG PayloadLength,
                    PVOID Reply, ULONG ReplyCapacity, PULONG ReplyLength);

/* As Smb2Upcall, and the daemon receives a handle to Token (an impersonation token of the caller) with
 * the request. Token may be NULL. */
NTSTATUS Smb2UpcallEx(ULONG Opcode, const VOID *Payload, ULONG PayloadLength,
                      PVOID Reply, ULONG ReplyCapacity, PULONG ReplyLength, PACCESS_TOKEN Token);

/* An upcall the daemon may hold until the caller's IRP is cancelled or its thread ends (blocking locks) */
NTSTATUS Smb2UpcallCancellable(ULONG Opcode, const VOID *Payload, ULONG PayloadLength, PIRP Irp,
                               PVOID Reply, ULONG ReplyCapacity, PULONG ReplyLength, PSMB2RDR_CANCEL_SLOTS Slots);

NTSTATUS Smb2HandleGetRequest(PVOID OutBuffer, ULONG OutLength, PULONG Written, ULONG RequestorPid);
NTSTATUS Smb2HandlePutReply(const VOID *InBuffer, ULONG InLength);

#define Smb2Trace(...) DbgPrint("smb2rdr: " __VA_ARGS__)

#endif /* _SMB2RDR_H */

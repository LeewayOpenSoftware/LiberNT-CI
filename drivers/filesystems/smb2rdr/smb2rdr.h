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

NTSTATUS Smb2HandleGetRequest(PVOID OutBuffer, ULONG OutLength, PULONG Written);
NTSTATUS Smb2HandlePutReply(const VOID *InBuffer, ULONG InLength);

#define Smb2Trace(...) DbgPrint("smb2rdr: " __VA_ARGS__)

#endif /* _SMB2RDR_H */

/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Interface between the smb2rdr mini-redirector and the smb2d protocol daemon
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Included by both kernel and user mode: only plain types, no kernel-only headers.
 */

#ifndef _SMB2RDR_IOCTL_H
#define _SMB2RDR_IOCTL_H

#define SMB2RDR_DEVICE_NAME         L"\\Device\\smb2rdr"
#define SMB2RDR_SHADOW_DEVICE_NAME  L"\\??\\smb2rdr"
#define SMB2RDR_USER_DEVICE_NAME    L"\\\\.\\smb2rdr"
#define SMB2RDR_PROVIDER_NAME       L"SMB2 Network"

/* Bumped when the upcall/downcall layout changes; the daemon sends it with IOCTL_SMB2RDR_START. */
#define SMB2RDR_PROTOCOL_VERSION    1

#define _SMB2RDR_CTL_CODE(code, method) \
    CTL_CODE(FILE_DEVICE_NETWORK_REDIRECTOR, 0x800 | (code), method, FILE_ANY_ACCESS)

#define IOCTL_SMB2RDR_START     _SMB2RDR_CTL_CODE(0, METHOD_BUFFERED)   /* in: ULONG protocol version */
#define IOCTL_SMB2RDR_STOP      _SMB2RDR_CTL_CODE(1, METHOD_BUFFERED)
#define IOCTL_SMB2RDR_GETSTATE  _SMB2RDR_CTL_CODE(3, METHOD_BUFFERED)   /* out: ULONG SMB2RDR_STATE_* */
#define IOCTL_SMB2RDR_GETREQ    _SMB2RDR_CTL_CODE(6, METHOD_BUFFERED)   /* blocks; out: SMB2RDR_UPCALL */
#define IOCTL_SMB2RDR_PUTREPLY  _SMB2RDR_CTL_CODE(7, METHOD_BUFFERED)   /* in: SMB2RDR_DOWNCALL */

#define SMB2RDR_STATE_STOPPED   0
#define SMB2RDR_STATE_STARTING  1
#define SMB2RDR_STATE_STARTED   2

/* Largest payload either way (a 64K read plus its parameters) */
#define SMB2RDR_MAX_PAYLOAD     (64 * 1024 + 256)

/* Operations the kernel asks of the daemon */
#define SMB2RDR_UP_PING         1   /* no payload; reply payload is echoed (transport self-test) */
#define SMB2RDR_UP_CONNECT      2   /* payload: server name, UTF-16, no terminator */

#define SMB2RDR_UP_TREE_CONNECT     3   /* payload: SMB2RDR_TREE_CONNECT_REQ; reply: SMB2RDR_TREE_CONNECT_REPLY */
#define SMB2RDR_UP_TREE_DISCONNECT  4   /* payload: SMB2RDR_HANDLE_REQ (tree handle) */
#define SMB2RDR_UP_OPEN             5   /* payload: SMB2RDR_OPEN_REQ; reply: SMB2RDR_OPEN_REPLY */
#define SMB2RDR_UP_CLOSE            6   /* payload: SMB2RDR_HANDLE_REQ (file handle) */
#define SMB2RDR_UP_READ             7   /* payload: SMB2RDR_READ_REQ; reply: the data */
#define SMB2RDR_UP_QUERYDIR         8   /* payload: SMB2RDR_QUERYDIR_REQ; reply: raw FILE_*_INFORMATION entries */

/*
 * Handles are opaque 64-bit values owned by the daemon (it maps them to connection, tree and file ids).
 * 0 is never a valid handle.
 */
typedef struct _SMB2RDR_HANDLE_REQ
{
    unsigned long long Handle;
} SMB2RDR_HANDLE_REQ;

/* Server and share as "server\share", UTF-16, no terminator, no leading backslash */
typedef struct _SMB2RDR_TREE_CONNECT_REQ
{
    unsigned long NameLength;   /* bytes of Name */
    unsigned long Reserved;
    /* WCHAR Name[] follows */
} SMB2RDR_TREE_CONNECT_REQ;

typedef struct _SMB2RDR_TREE_CONNECT_REPLY
{
    unsigned long long TreeHandle;
} SMB2RDR_TREE_CONNECT_REPLY;

typedef struct _SMB2RDR_OPEN_REQ
{
    unsigned long long TreeHandle;
    unsigned long DesiredAccess;
    unsigned long ShareAccess;
    unsigned long FileAttributes;
    unsigned long Disposition;      /* FILE_OPEN .. FILE_OVERWRITE_IF, same values as SMB2 */
    unsigned long CreateOptions;
    unsigned long PathLength;       /* bytes of Path */
    /* WCHAR Path[] follows: relative to the share, no leading backslash */
} SMB2RDR_OPEN_REQ;

typedef struct _SMB2RDR_OPEN_REPLY
{
    unsigned long long FileHandle;
    long long CreationTime;
    long long LastAccessTime;
    long long LastWriteTime;
    long long ChangeTime;
    long long AllocationSize;
    long long EndOfFile;
    unsigned long FileAttributes;
    unsigned long CreateAction;     /* FILE_OPENED, FILE_CREATED, ... */
} SMB2RDR_OPEN_REPLY;

typedef struct _SMB2RDR_READ_REQ
{
    unsigned long long FileHandle;
    unsigned long long Offset;
    unsigned long Length;           /* at most SMB2RDR_MAX_IO */
    unsigned long Reserved;
} SMB2RDR_READ_REQ;

#define SMB2RDR_QD_RESTART  0x1
#define SMB2RDR_QD_SINGLE   0x2

typedef struct _SMB2RDR_QUERYDIR_REQ
{
    unsigned long long FileHandle;
    unsigned long InfoClass;        /* FILE_INFORMATION_CLASS value, passed through */
    unsigned long Flags;            /* SMB2RDR_QD_* */
    unsigned long OutputLength;     /* at most SMB2RDR_MAX_IO */
    unsigned long PatternLength;    /* bytes of Pattern, 0 means "*" */
    /* WCHAR Pattern[] follows */
} SMB2RDR_QUERYDIR_REQ;

/* One request moves at most this many bytes of file or directory data */
#define SMB2RDR_MAX_IO          (60 * 1024)

/* Daemon <- kernel (output of IOCTL_SMB2RDR_GETREQ): header followed by Length payload bytes */
typedef struct _SMB2RDR_UPCALL
{
    unsigned long Opcode;
    unsigned long Length;
    unsigned long long Id;
} SMB2RDR_UPCALL;

/* Daemon -> kernel (input of IOCTL_SMB2RDR_PUTREPLY): header followed by Length payload bytes */
typedef struct _SMB2RDR_DOWNCALL
{
    unsigned long long Id;
    long Status;                /* NTSTATUS result of the operation */
    unsigned long Length;
} SMB2RDR_DOWNCALL;

#endif /* _SMB2RDR_IOCTL_H */

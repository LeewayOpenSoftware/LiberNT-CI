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
#define SMB2RDR_PROTOCOL_VERSION    2

#define _SMB2RDR_CTL_CODE(code, method) \
    CTL_CODE(FILE_DEVICE_NETWORK_REDIRECTOR, 0x800 | (code), method, FILE_ANY_ACCESS)

#define IOCTL_SMB2RDR_START     _SMB2RDR_CTL_CODE(0, METHOD_BUFFERED)   /* in: ULONG protocol version */
#define IOCTL_SMB2RDR_STOP      _SMB2RDR_CTL_CODE(1, METHOD_BUFFERED)   /* in: optional ULONG force (1 = stop even with open files) */
#define IOCTL_SMB2RDR_ADDCONN   _SMB2RDR_CTL_CODE(4, METHOD_BUFFERED)   /* in: SMB2RDR_CONN_REQ + name + EA */
#define IOCTL_SMB2RDR_DELCONN   _SMB2RDR_CTL_CODE(5, METHOD_BUFFERED)   /* in: connection name, NUL terminated */
#define IOCTL_SMB2RDR_GETSTATE  _SMB2RDR_CTL_CODE(3, METHOD_BUFFERED)   /* out: ULONG SMB2RDR_STATE_* */
#define IOCTL_SMB2RDR_GETREQ    _SMB2RDR_CTL_CODE(6, METHOD_BUFFERED)   /* blocks; out: SMB2RDR_UPCALL */
#define IOCTL_SMB2RDR_PUTREPLY  _SMB2RDR_CTL_CODE(7, METHOD_BUFFERED)   /* in: SMB2RDR_DOWNCALL */

/*
 * Explicit connection (net use): the network provider asks the redirector to connect. The buffer is
 *   SMB2RDR_CONN_REQ, WCHAR Name[NameLength / 2] (NUL included), EaPadding bytes, EaLength bytes of
 *   FILE_FULL_EA_INFORMATION entries. Name is \Device\smb2rdr\;X:\server\share or \Device\smb2rdr\server\share.
 * Credentials travel as extended attributes with these names; the value is UTF-16 without terminator.
 */
typedef struct _SMB2RDR_CONN_REQ
{
    unsigned short NameLength;
    unsigned short EaPadding;
    unsigned long EaLength;
} SMB2RDR_CONN_REQ;

#define SMB2RDR_EA_USER         "UserName"
#define SMB2RDR_EA_DOMAIN       "Domain"
#define SMB2RDR_EA_PASSWORD     "Password"

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

#define SMB2RDR_UP_WRITE            9   /* payload: SMB2RDR_WRITE_REQ + data; reply: ULONG bytes written */
#define SMB2RDR_UP_SETINFO          10  /* payload: SMB2RDR_SETINFO_REQ + buffer */
#define SMB2RDR_UP_FLUSH            11  /* payload: SMB2RDR_HANDLE_REQ (file handle) */

/*
 * Handles are opaque 64-bit values owned by the daemon (it maps them to connection, tree and file ids).
 * 0 is never a valid handle.
 */
typedef struct _SMB2RDR_HANDLE_REQ
{
    unsigned long long Handle;
} SMB2RDR_HANDLE_REQ;

/*
 * Server and share as "server\share", then the optional credentials, all UTF-16 without terminators,
 * laid out one after the other after this header: Name, User, Domain, Password. An empty User means
 * the daemon's own identity.
 */
typedef struct _SMB2RDR_TREE_CONNECT_REQ
{
    unsigned long NameLength;       /* bytes */
    unsigned long UserLength;       /* bytes */
    unsigned long DomainLength;     /* bytes */
    unsigned long PasswordLength;   /* bytes */
    unsigned long long CallerLuid;  /* authentication id of the opening caller (implicit identity), else 0 */
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

typedef struct _SMB2RDR_WRITE_REQ
{
    unsigned long long FileHandle;
    unsigned long long Offset;
    unsigned long Length;           /* bytes of data following, at most SMB2RDR_MAX_IO */
    unsigned long Reserved;
    /* data follows */
} SMB2RDR_WRITE_REQ;

/* Classes the daemon accepts: 4 basic, 13 disposition, 19 allocation, 20 end of file (MS-FSCC layouts) */
typedef struct _SMB2RDR_SETINFO_REQ
{
    unsigned long long FileHandle;
    unsigned long InfoClass;        /* FILE_INFORMATION_CLASS value */
    unsigned long BufferLength;     /* bytes of buffer following */
    /* buffer follows */
} SMB2RDR_SETINFO_REQ;

/* One request moves at most this many bytes of file or directory data */
#define SMB2RDR_MAX_IO          (60 * 1024)

/* Daemon <- kernel (output of IOCTL_SMB2RDR_GETREQ): header followed by Length payload bytes */
typedef struct _SMB2RDR_UPCALL
{
    unsigned long Opcode;
    unsigned long Length;
    unsigned long long Id;
    unsigned long long TokenHandle; /* impersonation token of the caller, valid in the daemon; 0 = none */
} SMB2RDR_UPCALL;

/* Daemon -> kernel (input of IOCTL_SMB2RDR_PUTREPLY): header followed by Length payload bytes */
typedef struct _SMB2RDR_DOWNCALL
{
    unsigned long long Id;
    long Status;                /* NTSTATUS result of the operation */
    unsigned long Length;
} SMB2RDR_DOWNCALL;


/* Pointer names for the kernel */
typedef SMB2RDR_HANDLE_REQ *PSMB2RDR_HANDLE_REQ;
typedef SMB2RDR_TREE_CONNECT_REQ *PSMB2RDR_TREE_CONNECT_REQ;
typedef SMB2RDR_TREE_CONNECT_REPLY *PSMB2RDR_TREE_CONNECT_REPLY;
typedef SMB2RDR_OPEN_REQ *PSMB2RDR_OPEN_REQ;
typedef SMB2RDR_OPEN_REPLY *PSMB2RDR_OPEN_REPLY;
typedef SMB2RDR_READ_REQ *PSMB2RDR_READ_REQ;
typedef SMB2RDR_QUERYDIR_REQ *PSMB2RDR_QUERYDIR_REQ;
typedef SMB2RDR_WRITE_REQ *PSMB2RDR_WRITE_REQ;
typedef SMB2RDR_SETINFO_REQ *PSMB2RDR_SETINFO_REQ;

#endif /* _SMB2RDR_IOCTL_H */

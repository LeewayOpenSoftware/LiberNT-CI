/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMB2 wire constants and field offsets (public MS-SMB2 specification, section 2.2)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Message bodies are accessed through byte-offset helpers rather than packed structs,
 * so the code has no structure padding or alignment dependence.
 */

#ifndef _SMB2PROTO_H
#define _SMB2PROTO_H

#include <stdint.h>

#define SMB2_HEADER_SIZE        64

/* Commands (2.2.1) */
#define SMB2_NEGOTIATE          0x0000
#define SMB2_SESSION_SETUP      0x0001
#define SMB2_LOGOFF             0x0002
#define SMB2_TREE_CONNECT       0x0003
#define SMB2_TREE_DISCONNECT    0x0004
#define SMB2_CREATE             0x0005
#define SMB2_CLOSE              0x0006
#define SMB2_FLUSH              0x0007
#define SMB2_READ               0x0008
#define SMB2_WRITE              0x0009
#define SMB2_LOCK               0x000A
#define SMB2_IOCTL              0x000B
#define SMB2_CANCEL             0x000C
#define SMB2_ECHO               0x000D
#define SMB2_QUERY_DIRECTORY    0x000E
#define SMB2_CHANGE_NOTIFY      0x000F
#define SMB2_QUERY_INFO         0x0010
#define SMB2_SET_INFO           0x0011

/* Header flags */
#define SMB2_FLAGS_SERVER_TO_REDIR  0x00000001u
#define SMB2_FLAGS_ASYNC_COMMAND    0x00000002u
#define SMB2_FLAGS_SIGNED           0x00000008u

/* Header field offsets (2.2.1.2, synchronous form) */
#define SMB2H_STRUCTSIZE        4
#define SMB2H_CREDITCHARGE      6
#define SMB2H_STATUS            8
#define SMB2H_COMMAND           12
#define SMB2H_CREDITS           14
#define SMB2H_FLAGS             16
#define SMB2H_NEXTCOMMAND       20
#define SMB2H_MESSAGEID         24
#define SMB2H_ASYNCID           32
#define SMB2H_TREEID            36
#define SMB2H_SESSIONID         40
#define SMB2H_SIGNATURE         48

/* Dialects */
#define SMB2_DIALECT_202        0x0202
#define SMB2_DIALECT_210        0x0210

/* Security modes (negotiate) */
#define SMB2_NEGOTIATE_SIGNING_ENABLED   0x0001
#define SMB2_NEGOTIATE_SIGNING_REQUIRED  0x0002

/* Session flags (session setup response) */
#define SMB2_SESSION_FLAG_IS_GUEST  0x0001
#define SMB2_SESSION_FLAG_IS_NULL   0x0002

/* Create */
#define SMB2_OPLOCK_NONE            0x00
#define SMB2_IMPERSONATION          0x00000002u
#define SMB2_FILE_SUPERSEDE         0
#define SMB2_FILE_OPEN              1
#define SMB2_FILE_CREATE            2
#define SMB2_FILE_OPEN_IF           3
#define SMB2_FILE_OVERWRITE         4
#define SMB2_FILE_OVERWRITE_IF      5

/* Query directory */
#define SMB2_FILE_DIRECTORY_INFORMATION  0x01
#define SMB2_RESTART_SCANS               0x01

/* NTSTATUS values the client logic cares about */
#define SMB2_STATUS_SUCCESS                  0x00000000u
#define SMB2_STATUS_PENDING                  0x00000103u
#define SMB2_STATUS_END_OF_FILE              0xC0000011u
#define SMB2_STATUS_MORE_PROCESSING          0xC0000016u
#define SMB2_STATUS_ACCESS_DENIED            0xC0000022u
#define SMB2_STATUS_OBJECT_NAME_NOT_FOUND    0xC0000034u
#define SMB2_STATUS_LOGON_FAILURE            0xC000006Du
#define SMB2_STATUS_NO_MORE_FILES            0x80000006u
#define SMB2_STATUS_BAD_NETWORK_NAME         0xC00000CCu
#define SMB2_STATUS_INVALID_NETWORK_RESPONSE 0xC00000C3u
#define SMB2_STATUS_INSUFFICIENT_RESOURCES   0xC000009Au
#define SMB2_STATUS_INVALID_PARAMETER        0xC000000Du
#define SMB2_STATUS_UNSUCCESSFUL             0xC0000001u
#define SMB2_STATUS_CONNECTION_DISCONNECTED  0xC000020Cu

typedef struct _SMB2_FILE_ID
{
    uint64_t Persistent;
    uint64_t Volatile;
} SMB2_FILE_ID;

/* Little-endian field accessors on a byte buffer */
static __inline uint16_t Smb2Get16(const uint8_t *p, unsigned o)
{
    return (uint16_t)(p[o] | (p[o + 1] << 8));
}

static __inline uint32_t Smb2Get32(const uint8_t *p, unsigned o)
{
    return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
}

static __inline uint64_t Smb2Get64(const uint8_t *p, unsigned o)
{
    return (uint64_t)Smb2Get32(p, o) | ((uint64_t)Smb2Get32(p, o + 4) << 32);
}

static __inline void Smb2Put16(uint8_t *p, unsigned o, uint16_t v)
{
    p[o] = (uint8_t)v;
    p[o + 1] = (uint8_t)(v >> 8);
}

static __inline void Smb2Put32(uint8_t *p, unsigned o, uint32_t v)
{
    Smb2Put16(p, o, (uint16_t)v);
    Smb2Put16(p, o + 2, (uint16_t)(v >> 16));
}

static __inline void Smb2Put64(uint8_t *p, unsigned o, uint64_t v)
{
    Smb2Put32(p, o, (uint32_t)v);
    Smb2Put32(p, o + 4, (uint32_t)(v >> 32));
}

#endif /* _SMB2PROTO_H */

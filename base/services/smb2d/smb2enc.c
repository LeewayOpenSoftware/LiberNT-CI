/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Validation of the request buffers the redirector sends for rename and byte-range locks
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "smb2enc.h"
#include <string.h>
#include "smb2proto.h"

/*
 * MS-FSCC 2.4.34.2 as used by SMB2: BYTE ReplaceIfExists, 7 reserved bytes, 8 bytes RootDirectory (must be
 * zero), ULONG FileNameLength, WCHAR FileName[]. The name is relative to the share: no leading or trailing
 * backslash, no NUL, at least one character.
 */
uint32_t Smb2ValidateRenameInfo(const uint8_t *Buffer, uint32_t Length)
{
    uint32_t nameBytes, i, chars;

    if (!Buffer || Length < SMB2ENC_RENAME_HEADER + sizeof(uint16_t))
        return SMB2_STATUS_INVALID_PARAMETER;
    if (Buffer[0] > 1 || Smb2Get64(Buffer, 8) != 0)
        return SMB2_STATUS_INVALID_PARAMETER;
    nameBytes = Smb2Get32(Buffer, 16);
    if (nameBytes == 0 || (nameBytes & 1) || nameBytes > 2u * SMB2ENC_MAX_NAME_CHARS ||
        nameBytes != Length - SMB2ENC_RENAME_HEADER)
        return SMB2_STATUS_INVALID_PARAMETER;
    chars = nameBytes / 2;
    for (i = 0; i < chars; i++)
    {
        if (Smb2Get16(Buffer, SMB2ENC_RENAME_HEADER + 2 * i) == 0)
            return SMB2_STATUS_INVALID_PARAMETER;
    }
    if (Smb2Get16(Buffer, SMB2ENC_RENAME_HEADER) == 0x5C ||
        Smb2Get16(Buffer, SMB2ENC_RENAME_HEADER + 2 * (chars - 1)) == 0x5C)
        return SMB2_STATUS_INVALID_PARAMETER;
    return SMB2_STATUS_SUCCESS;
}

uint32_t Smb2ValidateLockRange(uint64_t Offset, uint64_t Length, uint32_t Flags)
{
    if (Flags != SMB2_LOCKFLAG_SHARED && Flags != SMB2_LOCKFLAG_EXCLUSIVE && Flags != SMB2_LOCKFLAG_UNLOCK)
        return SMB2_STATUS_INVALID_PARAMETER;
    if (Length > UINT64_MAX - Offset)
        return SMB2_STATUS_INVALID_PARAMETER;
    return SMB2_STATUS_SUCCESS;
}

void Smb2BuildLockBody(uint8_t Body[SMB2ENC_LOCK_BODY], uint64_t Persistent, uint64_t Volatile, uint64_t Offset,
                       uint64_t Length, uint32_t Flags, int Wait)
{
    memset(Body, 0, SMB2ENC_LOCK_BODY);
    Smb2Put16(Body, 0, 48);
    Smb2Put16(Body, 2, 1);
    Smb2Put64(Body, 8, Persistent);
    Smb2Put64(Body, 16, Volatile);
    Smb2Put64(Body, 24, Offset);
    Smb2Put64(Body, 32, Length);
    Smb2Put32(Body, 40, (Wait || Flags == SMB2_LOCKFLAG_UNLOCK) ? Flags : (Flags | SMB2_LOCKFLAG_FAIL_IMMEDIATELY));
}

uint32_t Smb2ValidateNotifyChain(const uint8_t *Buffer, uint32_t Length)
{
    uint32_t pos = 0, next, action, nameBytes;

    if (Length == 0)
        return SMB2_STATUS_SUCCESS;
    if (!Buffer)
        return SMB2_STATUS_INVALID_PARAMETER;
    for (;;)
    {
        if (Length - pos < 12)
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        next = Smb2Get32(Buffer, pos);
        action = Smb2Get32(Buffer, pos + 4);
        nameBytes = Smb2Get32(Buffer, pos + 8);
        if (action < 1 || action > 5 || (nameBytes & 1) || nameBytes > 2u * SMB2ENC_MAX_NAME_CHARS ||
            nameBytes > Length - pos - 12)
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        if (next == 0)
            return SMB2_STATUS_SUCCESS;
        if ((next & 3) || next < 12 + nameBytes || next > Length - pos)
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        pos += next;
    }
}

void Smb2BuildNotifyBody(uint8_t Body[SMB2ENC_NOTIFY_BODY], uint64_t Persistent, uint64_t Volatile, uint32_t Filter,
                         int WatchTree, uint32_t OutputLength)
{
    memset(Body, 0, SMB2ENC_NOTIFY_BODY);
    Smb2Put16(Body, 0, 32);
    Smb2Put16(Body, 2, WatchTree ? 1 : 0);
    Smb2Put32(Body, 4, OutputLength);
    Smb2Put64(Body, 8, Persistent);
    Smb2Put64(Body, 16, Volatile);
    Smb2Put32(Body, 24, Filter);
}

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

size_t Smb2BuildNegotiateRequest(uint8_t *Out, size_t Capacity, uint16_t MaxDialect, uint32_t Capabilities,
                                 uint16_t SecurityMode, const uint8_t Guid[16], const uint8_t Salt[32])
{
    static const uint16_t all[5] = {0x0202, 0x0210, 0x0300, 0x0302, 0x0311};
    unsigned n = 0, i, pos;
    size_t ctxStart;

    if (Capacity < SMB2ENC_NEGOTIATE_BODY_MAX)
        return 0;
    for (i = 0; i < 5; i++)
    {
        if (all[i] <= MaxDialect)
            n++;
    }
    if (n == 0 || all[n - 1] != MaxDialect)
        return 0;

    memset(Out, 0, SMB2ENC_NEGOTIATE_BODY_MAX);
    Smb2Put16(Out, 0, 36);
    Smb2Put16(Out, 2, (uint16_t)n);
    Smb2Put16(Out, 4, SecurityMode);
    Smb2Put32(Out, 8, Capabilities);
    memcpy(Out + 12, Guid, 16);
    for (i = 0; i < n; i++)
        Smb2Put16(Out, 36 + 2 * i, all[i]);
    pos = 36 + 2 * n;
    if (MaxDialect < 0x0311)
        return pos;

    ctxStart = (SMB2_HEADER_SIZE + pos + 7) & ~(size_t)7;
    pos = (unsigned)(ctxStart - SMB2_HEADER_SIZE);
    Smb2Put32(Out, 28, (uint32_t)ctxStart);
    Smb2Put16(Out, 32, 2);

    Smb2Put16(Out, pos, 1);                 /* SMB2_PREAUTH_INTEGRITY_CAPABILITIES */
    Smb2Put16(Out, pos + 2, 6 + 32);
    Smb2Put16(Out, pos + 8, 1);             /* HashAlgorithmCount */
    Smb2Put16(Out, pos + 10, 32);           /* SaltLength */
    Smb2Put16(Out, pos + 12, SMB2ENC_HASH_SHA512);
    memcpy(Out + pos + 14, Salt, 32);
    pos += 8 + 6 + 32;
    pos = (pos + 7) & ~7u;

    Smb2Put16(Out, pos, 8);                 /* SMB2_SIGNING_CAPABILITIES */
    Smb2Put16(Out, pos + 2, 4);
    Smb2Put16(Out, pos + 8, 1);
    Smb2Put16(Out, pos + 10, SMB2ENC_SIGN_CMAC);
    pos += 8 + 4;
    return pos;
}

uint32_t Smb2ParseNegotiateContexts(const uint8_t *Msg, size_t Length, uint32_t Offset, uint32_t Count,
                                    SMB2ENC_NEGCTX *Out)
{
    size_t pos = Offset;
    uint32_t i;

    memset(Out, 0, sizeof(*Out));
    if (!Msg || Count == 0 || Count > 16 || (Offset & 7) || Offset < SMB2_HEADER_SIZE + 64 || Offset > Length)
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    for (i = 0; i < Count; i++)
    {
        uint16_t type, dataLen;
        size_t data;

        if (Length - pos < 8)
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        type = Smb2Get16(Msg, (unsigned)pos);
        dataLen = Smb2Get16(Msg, (unsigned)pos + 2);
        data = pos + 8;
        if (dataLen > Length - data)
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        switch (type)
        {
            case 1:
            {
                uint16_t algs, salt;

                if (Out->HavePreauth || dataLen < 4)
                    return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
                algs = Smb2Get16(Msg, (unsigned)data);
                salt = Smb2Get16(Msg, (unsigned)data + 2);
                if (algs != 1 || (size_t)4 + 2 * algs + salt > dataLen)
                    return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
                Out->HavePreauth = 1;
                Out->HashAlgorithm = Smb2Get16(Msg, (unsigned)data + 4);
                break;
            }
            case 2:
                if (Out->HaveCipher || dataLen < 4 || Smb2Get16(Msg, (unsigned)data) != 1)
                    return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
                Out->HaveCipher = 1;
                Out->Cipher = Smb2Get16(Msg, (unsigned)data + 2);
                break;
            case 8:
                if (Out->HaveSigning || dataLen < 4 || Smb2Get16(Msg, (unsigned)data) != 1)
                    return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
                Out->HaveSigning = 1;
                Out->SigningAlgorithm = Smb2Get16(Msg, (unsigned)data + 2);
                break;
            default:
                break;
        }
        pos = (data + dataLen + 7) & ~(size_t)7;
        if (pos > Length && i + 1 < Count)
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    return SMB2_STATUS_SUCCESS;
}

/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Validation of the request buffers the redirector sends for rename and byte-range locks
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "smb2enc.h"
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

/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Validation of the request buffers the redirector sends for rename and byte-range locks
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Plain C with no operating system dependency, so the daemon and the apitest use the same code.
 */

#ifndef _SMB2ENC_H
#define _SMB2ENC_H

#include <stdint.h>
#include <stddef.h>

/* Rename buffer (SMB2 form of FILE_RENAME_INFORMATION): returns an SMB2_STATUS value, 0 when valid */
#define SMB2ENC_RENAME_HEADER   20
#define SMB2ENC_MAX_NAME_CHARS  1024
uint32_t Smb2ValidateRenameInfo(const uint8_t *Buffer, uint32_t Length);

#endif /* _SMB2ENC_H */

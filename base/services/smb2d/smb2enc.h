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

/*
 * Byte-range lock request. Flags is one of SMB2_LOCKFLAG_SHARED, _EXCLUSIVE or _UNLOCK (the redirector never
 * sends FAIL_IMMEDIATELY: the builder adds it to a lock unless Wait is set; an unlock stands alone, 2.2.26.1). The range must not wrap past 2^64.
 */
uint32_t Smb2ValidateLockRange(uint64_t Offset, uint64_t Length, uint32_t Flags);

/* LOCK request body (2.2.26) with one element: SMB2ENC_LOCK_BODY bytes */
#define SMB2ENC_LOCK_BODY   48
void Smb2BuildLockBody(uint8_t Body[SMB2ENC_LOCK_BODY], uint64_t Persistent, uint64_t Volatile, uint64_t Offset,
                       uint64_t Length, uint32_t Flags, int Wait);

#endif /* _SMB2ENC_H */

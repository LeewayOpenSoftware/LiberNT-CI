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

/*
 * FILE_NOTIFY_INFORMATION chain from CHANGE_NOTIFY (2.2.36): every entry (NextEntryOffset, Action 1..5,
 * FileNameLength, FileName) lies inside Length, names are even-sized and at most SMB2ENC_MAX_NAME_CHARS, the
 * next entry starts 4-aligned after the end of this one. An empty buffer is valid.
 */
uint32_t Smb2ValidateNotifyChain(const uint8_t *Buffer, uint32_t Length);

/* CHANGE_NOTIFY request body (2.2.35): SMB2ENC_NOTIFY_BODY bytes */
#define SMB2ENC_NOTIFY_BODY 32
void Smb2BuildNotifyBody(uint8_t Body[SMB2ENC_NOTIFY_BODY], uint64_t Persistent, uint64_t Volatile, uint32_t Filter,
                         int WatchTree, uint32_t OutputLength);

/*
 * NEGOTIATE request body (2.2.3) offering every dialect up to MaxDialect from 2.0.2. With 3.1.1 it carries
 * the negotiate contexts: preauth integrity (SHA-512, Salt) and signing capabilities (AES-CMAC only).
 * Returns the body length, 0 when Capacity (at least 160) is too small or MaxDialect is not a known dialect.
 */
#define SMB2ENC_NEGOTIATE_BODY_MAX  160
size_t Smb2BuildNegotiateRequest(uint8_t *Out, size_t Capacity, uint16_t MaxDialect, uint32_t Capabilities,
                                 uint16_t SecurityMode, const uint8_t Guid[16], const uint8_t Salt[32]);

/* The context values a 3.1.1 NEGOTIATE response selected */
#define SMB2ENC_HASH_SHA512     1
#define SMB2ENC_SIGN_HMAC       0
#define SMB2ENC_SIGN_CMAC       1
#define SMB2ENC_SIGN_GMAC       2
typedef struct _SMB2ENC_NEGCTX
{
    int HavePreauth;
    uint16_t HashAlgorithm;
    int HaveCipher;
    uint16_t Cipher;
    int HaveSigning;
    uint16_t SigningAlgorithm;
} SMB2ENC_NEGCTX;

/*
 * The negotiate contexts of a whole NEGOTIATE response message (Length bytes, header included): Offset from the
 * start of the message, 8-aligned and after the fixed part, Count at most 16, every context inside the message,
 * a type at most once, a preauth context selecting exactly one hash and a signing or cipher context exactly one
 * algorithm. Returns an SMB2_STATUS value, 0 when valid.
 */
uint32_t Smb2ParseNegotiateContexts(const uint8_t *Msg, size_t Length, uint32_t Offset, uint32_t Count,
                                    SMB2ENC_NEGCTX *Out);

#endif /* _SMB2ENC_H */

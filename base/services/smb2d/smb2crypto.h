/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Portable crypto for SMB 3: SHA-512, AES-128, AES-CMAC, AES-GCM, AES-CCM, SP800-108 key derivation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Plain C with no operating system dependency, so the daemon and the apitest use the same code.
 */

#ifndef _SMB2CRYPTO_H
#define _SMB2CRYPTO_H

#include <stdint.h>
#include <stddef.h>

typedef struct _SMB2_SHA512
{
    uint64_t H[8];
    uint64_t Total;
    uint8_t Buf[128];
    size_t Used;
} SMB2_SHA512;

void Smb2Sha512Init(SMB2_SHA512 *Ctx);
void Smb2Sha512Update(SMB2_SHA512 *Ctx, const uint8_t *Data, size_t Length);
void Smb2Sha512Final(SMB2_SHA512 *Ctx, uint8_t Digest[64]);
void Smb2Sha512(const uint8_t *Data, size_t Length, uint8_t Digest[64]);

/* HMAC-SHA256 (smb2sign.c) */
void Smb2HmacSha256(const uint8_t *Key, size_t KeyLen, const uint8_t *Msg, size_t MsgLen, uint8_t Out[32]);

typedef struct _SMB2_AES
{
    uint8_t Rk[176];
} SMB2_AES;

void Smb2AesInit(SMB2_AES *Aes, const uint8_t Key[16]);
void Smb2AesEncrypt(const SMB2_AES *Aes, const uint8_t In[16], uint8_t Out[16]);

/* RFC 4493 */
void Smb2AesCmac(const uint8_t Key[16], const uint8_t *Msg, size_t Length, uint8_t Mac[16]);

/* SP 800-38D with a 96-bit IV and a 128-bit tag; Decrypt returns 1 when the tag is valid (Pt is untouched otherwise) */
void Smb2GcmEncrypt(const uint8_t Key[16], const uint8_t Iv[12], const uint8_t *Aad, size_t AadLength, const uint8_t *Pt,
                    size_t Length, uint8_t *Ct, uint8_t Tag[16]);
int Smb2GcmDecrypt(const uint8_t Key[16], const uint8_t Iv[12], const uint8_t *Aad, size_t AadLength, const uint8_t *Ct,
                   size_t Length, const uint8_t Tag[16], uint8_t *Pt);

/* RFC 3610; SMB uses NonceLen 11 and TagLen 16. Decrypt returns 1 when the tag is valid (Pt is zeroed otherwise) */
void Smb2CcmEncrypt(const uint8_t Key[16], const uint8_t *Nonce, unsigned NonceLen, unsigned TagLen, const uint8_t *Aad,
                    size_t AadLength, const uint8_t *Pt, size_t Length, uint8_t *Ct, uint8_t *Tag);
int Smb2CcmDecrypt(const uint8_t Key[16], const uint8_t *Nonce, unsigned NonceLen, unsigned TagLen, const uint8_t *Aad,
                   size_t AadLength, const uint8_t *Ct, size_t Length, const uint8_t *Tag, uint8_t *Pt);

/*
 * SP 800-108 counter mode with HMAC-SHA256 as in MS-SMB2 3.1.4.2: one 128-bit output of
 * PRF(Key, 00000001 || Label || 00 || Context || 00000080). Label includes its terminating NUL as the
 * specification writes it; at most 32 bytes of label and 64 bytes of context.
 */
void Smb2Kdf128(const uint8_t Key[16], const uint8_t *Label, size_t LabelLength, const uint8_t *Context,
                size_t ContextLength, uint8_t Out[16]);

#endif /* _SMB2CRYPTO_H */

/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104) for SMB 2.x message signing
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdlib.h>
#include <string.h>
#include "smb2proto.h"
#include "smb2client.h"
#include "smb2crypto.h"

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K[64] =
{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

typedef struct
{
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    unsigned fill;
} SHA256_STATE;

static void Sha256Block(SHA256_STATE *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    unsigned i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (i = 16; i < 64; i++)
    {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++)
    {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void Sha256Init(SHA256_STATE *c)
{
    static const uint32_t iv[8] =
        {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned i;

    for (i = 0; i < 8; i++)
        c->h[i] = iv[i];
    c->total = 0;
    c->fill = 0;
}

static void Sha256Update(SHA256_STATE *c, const uint8_t *p, size_t n)
{
    c->total += n;
    while (n)
    {
        size_t take = 64 - c->fill;

        if (take > n)
            take = n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += (unsigned)take;
        p += take;
        n -= take;
        if (c->fill == 64)
        {
            Sha256Block(c, c->buf);
            c->fill = 0;
        }
    }
}

static void Sha256Final(SHA256_STATE *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    unsigned i;

    Sha256Update(c, &pad, 1);
    while (c->fill != 56)
        Sha256Update(c, &zero, 1);
    for (i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    Sha256Update(c, len, 8);
    for (i = 0; i < 8; i++)
    {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

void Smb2HmacSha256(const uint8_t *key, size_t keylen, const uint8_t *msg, size_t msglen, uint8_t out[32])
{
    uint8_t k[64], ipad[64], opad[64], inner[32];
    SHA256_STATE c;
    unsigned i;

    memset(k, 0, sizeof(k));
    if (keylen > 64)
    {
        Sha256Init(&c);
        Sha256Update(&c, key, keylen);
        Sha256Final(&c, k);
    }
    else
    {
        memcpy(k, key, keylen);
    }
    for (i = 0; i < 64; i++)
    {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256Init(&c);
    Sha256Update(&c, ipad, 64);
    Sha256Update(&c, msg, msglen);
    Sha256Final(&c, inner);
    Sha256Init(&c);
    Sha256Update(&c, opad, 64);
    Sha256Update(&c, inner, 32);
    Sha256Final(&c, out);
}

/* 3.1.4.1: the signature is the first 16 bytes of HMAC-SHA256(SessionKey, message with Signature zeroed) */
void Smb2SignMessage(const uint8_t *key16, uint8_t *msg, size_t len)
{
    uint8_t mac[32];

    Smb2Put32(msg, SMB2H_FLAGS, Smb2Get32(msg, SMB2H_FLAGS) | SMB2_FLAGS_SIGNED);
    memset(msg + SMB2H_SIGNATURE, 0, 16);
    Smb2HmacSha256(key16, 16, msg, len, mac);
    memcpy(msg + SMB2H_SIGNATURE, mac, 16);
}

static int Mac16Equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    int i;

    for (i = 0; i < 16; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

int Smb2VerifyMessage(const uint8_t *key16, const uint8_t *msg, size_t len)
{
    uint8_t sig[16], mac[32], *tmp;
    int ok;

    tmp = (uint8_t *)malloc(len);
    if (!tmp)
        return 0;
    memcpy(tmp, msg, len);
    memcpy(sig, msg + SMB2H_SIGNATURE, 16);
    memset(tmp + SMB2H_SIGNATURE, 0, 16);
    Smb2HmacSha256(key16, 16, tmp, len, mac);
    free(tmp);
    ok = Mac16Equal(mac, sig);
    return ok;
}

/* 3.1.4.1 for dialect 3.x: the signature is AES-128-CMAC(SigningKey, message with Signature zeroed) */
void Smb2SignMessageEx(const uint8_t *key16, uint8_t *msg, size_t len, int cmac)
{
    uint8_t mac[16];

    if (!cmac)
    {
        Smb2SignMessage(key16, msg, len);
        return;
    }
    Smb2Put32(msg, SMB2H_FLAGS, Smb2Get32(msg, SMB2H_FLAGS) | SMB2_FLAGS_SIGNED);
    memset(msg + SMB2H_SIGNATURE, 0, 16);
    Smb2AesCmac(key16, msg, len, mac);
    memcpy(msg + SMB2H_SIGNATURE, mac, 16);
}

int Smb2VerifyMessageEx(const uint8_t *key16, const uint8_t *msg, size_t len, int cmac)
{
    uint8_t sig[16], mac[16], *tmp;
    int ok;

    if (!cmac)
        return Smb2VerifyMessage(key16, msg, len);
    tmp = (uint8_t *)malloc(len);
    if (!tmp)
        return 0;
    memcpy(tmp, msg, len);
    memcpy(sig, msg + SMB2H_SIGNATURE, 16);
    memset(tmp + SMB2H_SIGNATURE, 0, 16);
    Smb2AesCmac(key16, tmp, len, mac);
    free(tmp);
    ok = Mac16Equal(mac, sig);
    return ok;
}

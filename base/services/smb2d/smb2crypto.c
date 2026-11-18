/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Portable crypto for SMB 3: SHA-512, AES-128, AES-CMAC, AES-GCM, AES-CCM, SP800-108 key derivation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "smb2crypto.h"
#include <string.h>

static const uint64_t K512[80] =
{
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};
static const uint64_t H512[8] =
{
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

void Smb2Sha512Init(SMB2_SHA512 *c)
{
    memcpy(c->H, H512, sizeof(H512));
    c->Total = 0;
    c->Used = 0;
}

static void Sha512Block(SMB2_SHA512 *c, const uint8_t *p)
{
    uint64_t w[80], a, b, d, e, f, g, h, t1, t2, cc;
    unsigned i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((uint64_t)p[8 * i] << 56) | ((uint64_t)p[8 * i + 1] << 48) | ((uint64_t)p[8 * i + 2] << 40) |
               ((uint64_t)p[8 * i + 3] << 32) | ((uint64_t)p[8 * i + 4] << 24) | ((uint64_t)p[8 * i + 5] << 16) |
               ((uint64_t)p[8 * i + 6] << 8) | (uint64_t)p[8 * i + 7];
    }
    for (; i < 80; i++)
    {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->H[0]; b = c->H[1]; cc = c->H[2]; d = c->H[3]; e = c->H[4]; f = c->H[5]; g = c->H[6]; h = c->H[7];
    for (i = 0; i < 80; i++)
    {
        uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);

        t1 = h + S1 + ch + K512[i] + w[i];
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->H[0] += a; c->H[1] += b; c->H[2] += cc; c->H[3] += d; c->H[4] += e; c->H[5] += f; c->H[6] += g; c->H[7] += h;
}

void Smb2Sha512Update(SMB2_SHA512 *c, const uint8_t *p, size_t n)
{
    c->Total += n;
    while (n)
    {
        size_t take = 128 - c->Used;

        if (take > n)
            take = n;
        memcpy(c->Buf + c->Used, p, take);
        c->Used += take;
        p += take;
        n -= take;
        if (c->Used == 128)
        {
            Sha512Block(c, c->Buf);
            c->Used = 0;
        }
    }
}

void Smb2Sha512Final(SMB2_SHA512 *c, uint8_t out[64])
{
    uint64_t bits = c->Total * 8;
    unsigned i;

    c->Buf[c->Used++] = 0x80;
    if (c->Used > 112)
    {
        memset(c->Buf + c->Used, 0, 128 - c->Used);
        Sha512Block(c, c->Buf);
        c->Used = 0;
    }
    memset(c->Buf + c->Used, 0, 128 - c->Used);
    for (i = 0; i < 8; i++)
        c->Buf[120 + i] = (uint8_t)(bits >> (56 - 8 * i));
    Sha512Block(c, c->Buf);
    for (i = 0; i < 8; i++)
    {
        unsigned j;

        for (j = 0; j < 8; j++)
            out[8 * i + j] = (uint8_t)(c->H[i] >> (56 - 8 * j));
    }
}

void Smb2Sha512(const uint8_t *p, size_t n, uint8_t out[64])
{
    SMB2_SHA512 c;

    Smb2Sha512Init(&c);
    Smb2Sha512Update(&c, p, n);
    Smb2Sha512Final(&c, out);
}

/* ---- AES-128 (encryption only) ---- */

static uint8_t g_Sbox[256];
static volatile int g_SboxReady;

static uint8_t Rotl8(uint8_t x, unsigned s)
{
    return (uint8_t)((x << s) | (x >> (8 - s)));
}

static void SboxInit(void)
{
    uint8_t p = 1, q = 1, x;
    uint8_t box[256];

    do
    {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80)
            q ^= 0x09;
        x = (uint8_t)(q ^ Rotl8(q, 1) ^ Rotl8(q, 2) ^ Rotl8(q, 3) ^ Rotl8(q, 4));
        box[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    box[0] = 0x63;
    memcpy(g_Sbox, box, sizeof(box));
    g_SboxReady = 1;
}

static uint8_t Xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

void Smb2AesInit(SMB2_AES *a, const uint8_t Key[16])
{
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
    unsigned i, u_;

    if (!g_SboxReady)
        SboxInit();
    memcpy(a->Rk, Key, 16);
    for (i = 4; i < 44; i++)
    {
        uint8_t t[4];

        memcpy(t, a->Rk + 4 * (i - 1), 4);
        if ((i & 3) == 0)
        {
            uint8_t u = t[0];

            t[0] = (uint8_t)(g_Sbox[t[1]] ^ rcon[i / 4 - 1]);
            t[1] = g_Sbox[t[2]];
            t[2] = g_Sbox[t[3]];
            t[3] = g_Sbox[u];
        }
        for (u_ = 0; u_ < 4; u_++)
            a->Rk[4 * i + u_] = (uint8_t)(a->Rk[4 * (i - 4) + u_] ^ t[u_]);
    }
}

void Smb2AesEncrypt(const SMB2_AES *a, const uint8_t In[16], uint8_t Out[16])
{
    uint8_t s[16], t[16];
    unsigned r, i;

    for (i = 0; i < 16; i++)
        s[i] = (uint8_t)(In[i] ^ a->Rk[i]);
    for (r = 1; r <= 10; r++)
    {
        for (i = 0; i < 16; i++)
            s[i] = g_Sbox[s[i]];
        /* ShiftRows (column-major state) */
        t[0] = s[0];  t[4] = s[4];  t[8] = s[8];   t[12] = s[12];
        t[1] = s[5];  t[5] = s[9];  t[9] = s[13];  t[13] = s[1];
        t[2] = s[10]; t[6] = s[14]; t[10] = s[2];  t[14] = s[6];
        t[3] = s[15]; t[7] = s[3];  t[11] = s[7];  t[15] = s[11];
        if (r != 10)
        {
            for (i = 0; i < 16; i += 4)
            {
                uint8_t a0 = t[i], a1 = t[i + 1], a2 = t[i + 2], a3 = t[i + 3];
                uint8_t x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

                s[i] = (uint8_t)(a0 ^ x ^ Xtime((uint8_t)(a0 ^ a1)));
                s[i + 1] = (uint8_t)(a1 ^ x ^ Xtime((uint8_t)(a1 ^ a2)));
                s[i + 2] = (uint8_t)(a2 ^ x ^ Xtime((uint8_t)(a2 ^ a3)));
                s[i + 3] = (uint8_t)(a3 ^ x ^ Xtime((uint8_t)(a3 ^ a0)));
            }
        }
        else
        {
            memcpy(s, t, 16);
        }
        for (i = 0; i < 16; i++)
            s[i] = (uint8_t)(s[i] ^ a->Rk[16 * r + i]);
    }
    memcpy(Out, s, 16);
}

/* ---- AES-CMAC (RFC 4493) ---- */

static void ShiftLeft1(const uint8_t In[16], uint8_t Out[16])
{
    unsigned i;
    uint8_t carry = 0;

    for (i = 16; i-- > 0;)
    {
        uint8_t next = (uint8_t)(In[i] >> 7);

        Out[i] = (uint8_t)((In[i] << 1) | carry);
        carry = next;
    }
}

void Smb2AesCmac(const uint8_t Key[16], const uint8_t *Msg, size_t Len, uint8_t Mac[16])
{
    SMB2_AES aes;
    uint8_t l[16], k1[16], k2[16], x[16], y[16], last[16];
    size_t blocks, i;
    unsigned j;
    int complete;

    Smb2AesInit(&aes, Key);
    memset(l, 0, 16);
    Smb2AesEncrypt(&aes, l, l);
    ShiftLeft1(l, k1);
    if (l[0] & 0x80)
        k1[15] ^= 0x87;
    ShiftLeft1(k1, k2);
    if (k1[0] & 0x80)
        k2[15] ^= 0x87;

    blocks = (Len + 15) / 16;
    complete = blocks != 0 && (Len % 16) == 0;
    if (blocks == 0)
        blocks = 1;
    if (complete)
    {
        for (j = 0; j < 16; j++)
            last[j] = (uint8_t)(Msg[16 * (blocks - 1) + j] ^ k1[j]);
    }
    else
    {
        size_t rem = Len - 16 * (blocks - 1);

        memset(last, 0, 16);
        if (rem)
            memcpy(last, Msg + 16 * (blocks - 1), rem);
        last[rem] = 0x80;
        for (j = 0; j < 16; j++)
            last[j] ^= k2[j];
    }
    memset(x, 0, 16);
    for (i = 0; i + 1 < blocks; i++)
    {
        for (j = 0; j < 16; j++)
            y[j] = (uint8_t)(x[j] ^ Msg[16 * i + j]);
        Smb2AesEncrypt(&aes, y, x);
    }
    for (j = 0; j < 16; j++)
        y[j] = (uint8_t)(x[j] ^ last[j]);
    Smb2AesEncrypt(&aes, y, Mac);
}

/* ---- AES-GCM (SP 800-38D), 96-bit IV, 128-bit tag ---- */

static void GfMul(const uint8_t X[16], const uint8_t Y[16], uint8_t Z[16])
{
    uint8_t z[16] = {0}, v[16];
    unsigned i, j;

    memcpy(v, Y, 16);
    for (i = 0; i < 128; i++)
    {
        uint8_t lsb;

        if (X[i / 8] & (0x80 >> (i % 8)))
        {
            for (j = 0; j < 16; j++)
                z[j] ^= v[j];
        }
        lsb = (uint8_t)(v[15] & 1);
        for (j = 15; j > 0; j--)
            v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb)
            v[0] ^= 0xE1;
    }
    memcpy(Z, z, 16);
}

static void GhashUpdate(const uint8_t H[16], uint8_t Y[16], const uint8_t *Data, size_t Len)
{
    while (Len)
    {
        uint8_t blk[16] = {0};
        size_t n = Len < 16 ? Len : 16;
        unsigned j;

        memcpy(blk, Data, n);
        for (j = 0; j < 16; j++)
            Y[j] ^= blk[j];
        GfMul(Y, H, Y);
        Data += n;
        Len -= n;
    }
}

static void Inc32(uint8_t Ctr[16])
{
    unsigned i;

    for (i = 15; i >= 12; i--)
    {
        if (++Ctr[i] != 0)
            break;
    }
}

static void GcmCrypt(const SMB2_AES *aes, const uint8_t Counter0[16], const uint8_t *In, size_t Len, uint8_t *Out)
{
    uint8_t ctr[16], ks[16];
    size_t i, n;

    memcpy(ctr, Counter0, 16);
    for (i = 0; i < Len; i += 16)
    {
        unsigned j;

        Inc32(ctr);
        Smb2AesEncrypt(aes, ctr, ks);
        n = Len - i < 16 ? Len - i : 16;
        for (j = 0; j < n; j++)
            Out[i + j] = (uint8_t)(In[i + j] ^ ks[j]);
    }
}

static void GcmTag(const SMB2_AES *aes, const uint8_t H[16], const uint8_t Counter0[16], const uint8_t *Aad,
                   size_t AadLen, const uint8_t *Ct, size_t CtLen, uint8_t Tag[16])
{
    uint8_t y[16] = {0}, lens[16], ek[16];
    unsigned j;
    uint64_t a = (uint64_t)AadLen * 8, c = (uint64_t)CtLen * 8;

    GhashUpdate(H, y, Aad, AadLen);
    GhashUpdate(H, y, Ct, CtLen);
    for (j = 0; j < 8; j++)
    {
        lens[j] = (uint8_t)(a >> (56 - 8 * j));
        lens[8 + j] = (uint8_t)(c >> (56 - 8 * j));
    }
    GhashUpdate(H, y, lens, 16);
    Smb2AesEncrypt(aes, Counter0, ek);
    for (j = 0; j < 16; j++)
        Tag[j] = (uint8_t)(y[j] ^ ek[j]);
}

void Smb2GcmEncrypt(const uint8_t Key[16], const uint8_t Iv[12], const uint8_t *Aad, size_t AadLen, const uint8_t *Pt,
                    size_t Len, uint8_t *Ct, uint8_t Tag[16])
{
    SMB2_AES aes;
    uint8_t h[16] = {0}, c0[16];

    Smb2AesInit(&aes, Key);
    Smb2AesEncrypt(&aes, h, h);
    memcpy(c0, Iv, 12);
    c0[12] = c0[13] = c0[14] = 0;
    c0[15] = 1;
    GcmCrypt(&aes, c0, Pt, Len, Ct);
    GcmTag(&aes, h, c0, Aad, AadLen, Ct, Len, Tag);
}

int Smb2GcmDecrypt(const uint8_t Key[16], const uint8_t Iv[12], const uint8_t *Aad, size_t AadLen, const uint8_t *Ct,
                   size_t Len, const uint8_t Tag[16], uint8_t *Pt)
{
    SMB2_AES aes;
    uint8_t h[16] = {0}, c0[16], tag[16], diff = 0;
    unsigned j;

    Smb2AesInit(&aes, Key);
    Smb2AesEncrypt(&aes, h, h);
    memcpy(c0, Iv, 12);
    c0[12] = c0[13] = c0[14] = 0;
    c0[15] = 1;
    GcmTag(&aes, h, c0, Aad, AadLen, Ct, Len, tag);
    for (j = 0; j < 16; j++)
        diff |= (uint8_t)(tag[j] ^ Tag[j]);
    if (diff)
        return 0;
    GcmCrypt(&aes, c0, Ct, Len, Pt);
    return 1;
}

/* ---- AES-CCM (RFC 3610 / SP 800-38C) ---- */

static void CcmCtr(const SMB2_AES *aes, const uint8_t *Nonce, unsigned NonceLen, unsigned L, uint64_t Counter,
                   uint8_t Out[16])
{
    uint8_t a[16] = {0};
    unsigned i;

    a[0] = (uint8_t)(L - 1);
    memcpy(a + 1, Nonce, NonceLen);
    for (i = 0; i < L; i++)
        a[15 - i] = (uint8_t)(Counter >> (8 * i));
    Smb2AesEncrypt(aes, a, Out);
}

static void CcmMac(const SMB2_AES *aes, const uint8_t *Nonce, unsigned NonceLen, unsigned TagLen, const uint8_t *Aad,
                   size_t AadLen, const uint8_t *Pt, size_t Len, uint8_t Mac[16])
{
    unsigned L = 15 - NonceLen, i;
    uint8_t b[16], x[16];
    size_t pos;

    b[0] = (uint8_t)((AadLen ? 0x40 : 0) | (((TagLen - 2) / 2) << 3) | (L - 1));
    memcpy(b + 1, Nonce, NonceLen);
    for (i = 0; i < L; i++)
        b[15 - i] = (uint8_t)((uint64_t)Len >> (8 * i));
    Smb2AesEncrypt(aes, b, x);
    if (AadLen)
    {
        uint8_t hdr[16] = {0};
        size_t n, used;

        if (AadLen < 0xFF00)
        {
            hdr[0] = (uint8_t)(AadLen >> 8);
            hdr[1] = (uint8_t)AadLen;
            used = 2;
        }
        else
        {
            hdr[0] = 0xFF;
            hdr[1] = 0xFE;
            hdr[2] = (uint8_t)(AadLen >> 24);
            hdr[3] = (uint8_t)(AadLen >> 16);
            hdr[4] = (uint8_t)(AadLen >> 8);
            hdr[5] = (uint8_t)AadLen;
            used = 6;
        }
        n = AadLen < 16 - used ? AadLen : 16 - used;
        memcpy(hdr + used, Aad, n);
        for (i = 0; i < 16; i++)
            x[i] ^= hdr[i];
        Smb2AesEncrypt(aes, x, x);
        for (pos = n; pos < AadLen; pos += 16)
        {
            uint8_t blk[16] = {0};

            n = AadLen - pos < 16 ? AadLen - pos : 16;
            memcpy(blk, Aad + pos, n);
            for (i = 0; i < 16; i++)
                x[i] ^= blk[i];
            Smb2AesEncrypt(aes, x, x);
        }
    }
    for (pos = 0; pos < Len; pos += 16)
    {
        uint8_t blk[16] = {0};
        size_t n = Len - pos < 16 ? Len - pos : 16;

        memcpy(blk, Pt + pos, n);
        for (i = 0; i < 16; i++)
            x[i] ^= blk[i];
        Smb2AesEncrypt(aes, x, x);
    }
    memcpy(Mac, x, 16);
}

static void CcmXor(const SMB2_AES *aes, const uint8_t *Nonce, unsigned NonceLen, const uint8_t *In, size_t Len,
                   uint8_t *Out)
{
    unsigned L = 15 - NonceLen;
    size_t pos;
    uint64_t ctr = 1;

    for (pos = 0; pos < Len; pos += 16, ctr++)
    {
        uint8_t ks[16];
        size_t n = Len - pos < 16 ? Len - pos : 16, j;

        CcmCtr(aes, Nonce, NonceLen, L, ctr, ks);
        for (j = 0; j < n; j++)
            Out[pos + j] = (uint8_t)(In[pos + j] ^ ks[j]);
    }
}

void Smb2CcmEncrypt(const uint8_t Key[16], const uint8_t *Nonce, unsigned NonceLen, unsigned TagLen, const uint8_t *Aad,
                    size_t AadLen, const uint8_t *Pt, size_t Len, uint8_t *Ct, uint8_t *Tag)
{
    SMB2_AES aes;
    uint8_t mac[16], s0[16];
    unsigned i;

    Smb2AesInit(&aes, Key);
    CcmMac(&aes, Nonce, NonceLen, TagLen, Aad, AadLen, Pt, Len, mac);
    CcmXor(&aes, Nonce, NonceLen, Pt, Len, Ct);
    CcmCtr(&aes, Nonce, NonceLen, 15 - NonceLen, 0, s0);
    for (i = 0; i < TagLen; i++)
        Tag[i] = (uint8_t)(mac[i] ^ s0[i]);
}

int Smb2CcmDecrypt(const uint8_t Key[16], const uint8_t *Nonce, unsigned NonceLen, unsigned TagLen, const uint8_t *Aad,
                   size_t AadLen, const uint8_t *Ct, size_t Len, const uint8_t *Tag, uint8_t *Pt)
{
    SMB2_AES aes;
    uint8_t mac[16], s0[16], diff = 0;
    unsigned i;

    Smb2AesInit(&aes, Key);
    CcmXor(&aes, Nonce, NonceLen, Ct, Len, Pt);
    CcmMac(&aes, Nonce, NonceLen, TagLen, Aad, AadLen, Pt, Len, mac);
    CcmCtr(&aes, Nonce, NonceLen, 15 - NonceLen, 0, s0);
    for (i = 0; i < TagLen; i++)
        diff |= (uint8_t)(mac[i] ^ s0[i] ^ Tag[i]);
    if (diff)
    {
        memset(Pt, 0, Len);
        return 0;
    }
    return 1;
}

/* ---- SP 800-108 counter mode KDF, HMAC-SHA256, one 128-bit output (MS-SMB2 3.1.4.2) ---- */

void Smb2Kdf128(const uint8_t Key[16], const uint8_t *Label, size_t LabelLen, const uint8_t *Context, size_t ContextLen,
                uint8_t Out[16])
{
    uint8_t buf[4 + 32 + 1 + 64 + 4], mac[32];
    size_t n = 0;

    /* labels in MS-SMB2 are at most 16 bytes including the NUL; contexts at most 64 bytes */
    if (LabelLen > 32 || ContextLen > 64)
    {
        memset(Out, 0, 16);
        return;
    }
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 1;
    memcpy(buf + n, Label, LabelLen);
    n += LabelLen;
    buf[n++] = 0;
    memcpy(buf + n, Context, ContextLen);
    n += ContextLen;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 128;
    Smb2HmacSha256(Key, 16, buf, n, mac);
    memcpy(Out, mac, 16);
}

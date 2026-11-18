/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the SMB 3 NEGOTIATE request layout and the validation of the negotiate contexts of a response
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <stdint.h>
#include "smb2enc.h"
#include "smb2proto.h"

static void
TestBuild(void)
{
    BYTE Body[SMB2ENC_NEGOTIATE_BODY_MAX];
    BYTE Guid[16], Salt[32];
    size_t Length;
    unsigned i;

    for (i = 0; i < 16; i++)
        Guid[i] = (BYTE)(0xA0 + i);
    for (i = 0; i < 32; i++)
        Salt[i] = (BYTE)i;

    Length = Smb2BuildNegotiateRequest(Body, sizeof(Body), SMB2_DIALECT_311, 0, 1, Guid, Salt);
    ok(Length != 0 && Length <= sizeof(Body), "Length %lu\n", (unsigned long)Length);
    ok(Smb2Get16(Body, 0) == 36, "Wrong structure size\n");
    ok(Smb2Get16(Body, 2) == 5, "Wrong dialect count %u\n", Smb2Get16(Body, 2));
    ok(Smb2Get16(Body, 4) == 1, "Wrong security mode\n");
    ok(!memcmp(Body + 12, Guid, 16), "Wrong client guid\n");
    ok(Smb2Get16(Body, 36) == 0x0202 && Smb2Get16(Body, 44) == 0x0311, "Wrong dialect list\n");
    ok(Smb2Get32(Body, 28) % 8 == 0 && Smb2Get32(Body, 28) >= SMB2_HEADER_SIZE + 46, "Wrong context offset %lu\n",
       (unsigned long)Smb2Get32(Body, 28));
    ok(Smb2Get16(Body, 32) == 2, "Wrong context count\n");
    if (Smb2Get32(Body, 28) % 8 == 0 && Smb2Get32(Body, 28) >= SMB2_HEADER_SIZE + 46)
    {
        const BYTE *Ctx = Body + Smb2Get32(Body, 28) - SMB2_HEADER_SIZE;

        ok(Smb2Get16(Ctx, 0) == 1 && Smb2Get16(Ctx, 2) == 38, "Wrong preauth context\n");
        ok(Smb2Get16(Ctx, 8) == 1 && Smb2Get16(Ctx, 10) == 32 && Smb2Get16(Ctx, 12) == 1, "Wrong preauth data\n");
        ok(!memcmp(Ctx + 14, Salt, 32), "Wrong salt\n");
        Ctx += 48;
        ok(Smb2Get16(Ctx, 0) == 8 && Smb2Get16(Ctx, 2) == 4 && Smb2Get16(Ctx, 8) == 1 && Smb2Get16(Ctx, 10) == 1,
           "Wrong signing context\n");
        ok(Length == (size_t)(Ctx + 12 - Body), "Wrong body length %lu\n", (unsigned long)Length);
    }

    Length = Smb2BuildNegotiateRequest(Body, sizeof(Body), SMB2_DIALECT_302, 0, 1, Guid, Salt);
    ok(Length == 36 + 8, "Length %lu for 3.0.2\n", (unsigned long)Length);
    ok(Smb2Get16(Body, 2) == 4 && Smb2Get16(Body, 36 + 6) == 0x0302, "Wrong 3.0.2 dialect list\n");
    ok(Smb2Get32(Body, 28) == 0 && Smb2Get16(Body, 32) == 0, "A 3.0.2 request carries contexts\n");

    Length = Smb2BuildNegotiateRequest(Body, sizeof(Body), SMB2_DIALECT_210, 0, 1, Guid, Salt);
    ok(Length == 36 + 4 && Smb2Get16(Body, 2) == 2, "Wrong 2.1 request\n");

    ok(Smb2BuildNegotiateRequest(Body, sizeof(Body), 0x0301, 0, 1, Guid, Salt) == 0, "An unknown dialect was accepted\n");
    ok(Smb2BuildNegotiateRequest(Body, 100, SMB2_DIALECT_311, 0, 1, Guid, Salt) == 0, "A small buffer was accepted\n");
}

static size_t
Put(BYTE *Msg, size_t Pos, WORD Type, const BYTE *Data, WORD Length)
{
    Smb2Put16(Msg, (unsigned)Pos, Type);
    Smb2Put16(Msg, (unsigned)Pos + 2, Length);
    Smb2Put32(Msg, (unsigned)Pos + 4, 0);
    memcpy(Msg + Pos + 8, Data, Length);
    return (Pos + 8 + Length + 7) & ~(size_t)7;
}

static void
TestParse(void)
{
    BYTE Msg[512];
    static const BYTE Preauth[6 + 32] = { 1, 0, 32, 0, 1, 0 };
    static const BYTE Preauth2[8] = { 2, 0, 0, 0, 1, 0, 2, 0 };
    static const BYTE Signing[4] = { 1, 0, 1, 0 };
    static const BYTE Cipher[4] = { 1, 0, 2, 0 };
    SMB2ENC_NEGCTX Ctx;
    size_t Pos, Second, Third;

    memset(Msg, 0, sizeof(Msg));
    Pos = Put(Msg, 128, 1, Preauth, sizeof(Preauth));
    Second = Pos;
    Pos = Put(Msg, Pos, 8, Signing, sizeof(Signing));
    Third = Pos;
    ok_hex(Smb2ParseNegotiateContexts(Msg, Pos, 128, 2, &Ctx), SMB2_STATUS_SUCCESS);
    ok(Ctx.HavePreauth && Ctx.HashAlgorithm == 1 && Ctx.HaveSigning && Ctx.SigningAlgorithm == 1 && !Ctx.HaveCipher,
       "Wrong selection\n");

    Pos = Put(Msg, Third, 2, Cipher, sizeof(Cipher));
    ok_hex(Smb2ParseNegotiateContexts(Msg, Pos, 128, 3, &Ctx), SMB2_STATUS_SUCCESS);
    ok(Ctx.HaveCipher && Ctx.Cipher == 2, "Wrong cipher\n");

    ok_hex(Smb2ParseNegotiateContexts(Msg, Third, 128, 0, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    ok_hex(Smb2ParseNegotiateContexts(Msg, Third, 128, 17, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    ok_hex(Smb2ParseNegotiateContexts(Msg, Third, 130, 2, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    ok_hex(Smb2ParseNegotiateContexts(Msg, Third, 120, 2, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    ok_hex(Smb2ParseNegotiateContexts(Msg, Third, 4096, 2, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    ok_hex(Smb2ParseNegotiateContexts(Msg, Second + 6, 128, 2, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    ok_hex(Smb2ParseNegotiateContexts(Msg, Third, 128, 3, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);

    Pos = Put(Msg, 128, 1, Preauth, sizeof(Preauth));
    Pos = Put(Msg, Pos, 1, Preauth, sizeof(Preauth));
    ok_hex(Smb2ParseNegotiateContexts(Msg, Pos, 128, 2, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);

    Pos = Put(Msg, 128, 1, Preauth2, sizeof(Preauth2));
    ok_hex(Smb2ParseNegotiateContexts(Msg, Pos, 128, 1, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);

    Pos = Put(Msg, 128, 8, Signing, 2);
    ok_hex(Smb2ParseNegotiateContexts(Msg, Pos, 128, 1, &Ctx), SMB2_STATUS_INVALID_NETWORK_RESPONSE);

    Pos = Put(Msg, 128, 99, Signing, sizeof(Signing));
    ok_hex(Smb2ParseNegotiateContexts(Msg, Pos, 128, 1, &Ctx), SMB2_STATUS_SUCCESS);
    ok(!Ctx.HavePreauth && !Ctx.HaveSigning, "An unknown context was selected\n");
}

START_TEST(Smb2Negotiate)
{
    TestBuild();
    TestParse();
}

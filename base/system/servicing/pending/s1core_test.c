/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host logic tests for the S1 boot applier core
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Runs on the host (and as a LiberNT apitest): plan parse + signature verify,
 * hash-chained journal integrity, deterministic transactionId and the
 * two-attempt rollback rule. Fixtures were generated with the development
 * signing key (private key kept outside the repository) and an attacker key.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "s1core.h"

static int g_Failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_Failures++; } \
        else { printf("ok:   %s\n", msg); } \
    } while (0)

static uint8_t g_DevPub[S1_RSA2048_SIZE] = {
0xd0,0x5f,0x78,0x21,0x47,0x1b,0xf6,0xcd,0x23,0xba,0x6e,0x6b,0xa9,0x98,0x3e,0xfa,
0x52,0xb1,0x45,0xf2,0x74,0xd8,0x52,0x2a,0x9c,0x4a,0x50,0xce,0xd3,0x14,0xae,0xcb,
0xdf,0x46,0xe4,0xff,0x83,0x1f,0x0c,0x24,0xe4,0x78,0x72,0xb3,0x46,0xd4,0xdd,0xc8,
0x1e,0x99,0x94,0x34,0xdd,0x84,0xc5,0x41,0x00,0xc6,0x4c,0x5c,0x6f,0x44,0xa1,0xfb,
0x80,0x8c,0x82,0xed,0x67,0x4b,0x9f,0xc8,0x10,0x5d,0x0b,0x07,0x86,0x78,0xed,0x5e,
0x11,0x53,0x23,0x59,0x90,0xce,0xc4,0x60,0x6b,0xc9,0x31,0xe4,0xcd,0x44,0x51,0xb6,
0x73,0x18,0xc8,0x03,0xd6,0xd3,0xb5,0xd9,0x69,0x69,0x98,0x9b,0xb8,0xd9,0x15,0xd8,
0xa4,0x5a,0xc5,0x3b,0x42,0x32,0xe1,0xe7,0x32,0xef,0xa7,0xaa,0x6b,0xdf,0x5a,0x60,
0xca,0xd5,0xa5,0x64,0x60,0x84,0x89,0xdb,0xec,0x2e,0x69,0x7d,0x12,0x94,0xe6,0x10,
0xf5,0xa5,0xea,0x97,0x54,0x89,0x88,0xe1,0x0e,0xbb,0x85,0x6b,0xad,0xe3,0x2b,0x32,
0x7b,0x5d,0xae,0xac,0xae,0x2d,0x12,0x88,0x65,0xe3,0x36,0xdc,0x1c,0xd3,0x1e,0xc9,
0xba,0xe4,0xbc,0xb4,0x81,0xaf,0x33,0xfe,0x33,0x02,0xce,0xdb,0x5d,0x17,0x76,0xd5,
0x9f,0x96,0xb8,0x9a,0x62,0x96,0xaf,0x91,0x96,0x13,0xdf,0xe1,0x31,0x50,0x1a,0xdd,
0xe0,0xc2,0xde,0xe6,0x5f,0x9f,0x53,0x96,0x54,0x05,0x80,0xe6,0x20,0xf3,0x11,0x5b,
0xb6,0x64,0x1f,0x69,0xa0,0xce,0x5c,0x20,0xd2,0xaf,0x67,0x6f,0x8c,0x0d,0xae,0x21,
0xb3,0x5e,0xa3,0x91,0xb3,0xc6,0xf2,0x56,0x10,0x48,0xde,0x33,0x5a,0x3d,0x88,0x87,
};

static uint8_t *
ReadFile(const char *Path, size_t *Size)
{
    FILE *F = fopen(Path, "rb");
    uint8_t *Buf;
    long Len;
    if (!F) return NULL;
    fseek(F, 0, SEEK_END);
    Len = ftell(F);
    fseek(F, 0, SEEK_SET);
    Buf = (uint8_t *)malloc((size_t)Len);
    if (!Buf) { fclose(F); return NULL; }
    if (fread(Buf, 1, (size_t)Len, F) != (size_t)Len) { free(Buf); fclose(F); return NULL; }
    fclose(F);
    *Size = (size_t)Len;
    return Buf;
}

struct S1_TEST_CAPTURE
{
    char *Buf;
    size_t Used;
};

static S1Bool
S1TestAppender(void *IoCtx, const char *Bytes, size_t Length)
{
    struct S1_TEST_CAPTURE *C = (struct S1_TEST_CAPTURE *)IoCtx;
    memcpy(C->Buf + C->Used, Bytes, Length);
    C->Used += Length;
    return S1_TRUE;
}

static int
TestPlanAndSignature(const char *FixtureDir)
{
    char Path[512];
    size_t PlanSize, SigSize;
    uint8_t *PlanText, *Sig;
    S1_PLAN Plan;

    snprintf(Path, sizeof(Path), "%s/valid.plan.json", FixtureDir);
    PlanText = ReadFile(Path, &PlanSize);
    snprintf(Path, sizeof(Path), "%s/valid.sig.bin", FixtureDir);
    Sig = ReadFile(Path, &SigSize);
    if (!PlanText || !Sig) { printf("fixtures missing in %s\n", FixtureDir); return 1; }

    CHECK(S1PlanParse((const char *)PlanText, PlanSize, &Plan), "valid plan parses");
    CHECK(S1PlanCheckTransactionId(&Plan), "plan transactionId derives from its fields");
    CHECK(S1Rsa2048Sha256Verify(g_DevPub, 65537, PlanText, PlanSize, Sig),
          "manifest-style signature verifies with the development key");

    {
        char TxId[65];
        S1TransactionId(&Plan, TxId);
        CHECK(strcmp(TxId, Plan.TransactionId) == 0, "transactionId is deterministic and matches");
    }

    snprintf(Path, sizeof(Path), "%s/attacker.plan.json", FixtureDir);
    free(PlanText); free(Sig);
    PlanText = ReadFile(Path, &PlanSize);
    snprintf(Path, sizeof(Path), "%s/attacker.sig.bin", FixtureDir);
    Sig = ReadFile(Path, &SigSize);
    CHECK(S1PlanParse((const char *)PlanText, PlanSize, &Plan), "attacker-signed manifest parses");
    CHECK(!S1Rsa2048Sha256Verify(g_DevPub, 65537, PlanText, PlanSize, Sig),
          "manifest signature by a foreign key is rejected");

    snprintf(Path, sizeof(Path), "%s/wrongtx.plan.json", FixtureDir);
    free(PlanText);
    PlanText = ReadFile(Path, &PlanSize);
    CHECK(S1PlanParse((const char *)PlanText, PlanSize, &Plan), "tampered-transactionId plan parses");
    CHECK(!S1PlanCheckTransactionId(&Plan),
          "tampered transactionId is rejected before any signature check");

    /* structural rejections */
    {
        static const char BadSchema[] = "{\"schema\":2,\"transactionId\":\"x\"}";
        CHECK(!S1PlanParse(BadSchema, sizeof(BadSchema) - 1, &Plan), "unknown schema rejected");
    }
    {
        static const char BadSha[] =
            "{\"schema\":1,\"transactionId\":\"t\",\"target\":{\"machineId\":\"m\",\"arch\":\"a\"},"
            "\"package\":{\"id\":\"p\",\"version\":\"v\",\"digest\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
            "\"operation\":\"install\",\"operations\":[{\"seq\":1,\"type\":\"replace-file\","
            "\"component\":\"c\",\"source\":\"s\",\"target\":\"t\",\"sha256\":\"zz\"}]}";
        CHECK(!S1PlanParse(BadSha, sizeof(BadSha) - 1, &Plan), "malformed sha256 rejected");
    }
    {
        static const char BadType[] =
            "{\"schema\":1,\"transactionId\":\"t\",\"target\":{\"machineId\":\"m\",\"arch\":\"a\"},"
            "\"package\":{\"id\":\"p\",\"version\":\"v\",\"digest\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
            "\"operation\":\"install\",\"operations\":[{\"seq\":1,\"type\":\"replace-kernel\","
            "\"component\":\"c\",\"source\":\"s\",\"target\":\"t\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}";
        CHECK(!S1PlanParse(BadType, sizeof(BadType) - 1, &Plan), "unknown operation type rejected");
    }
    free(PlanText);
    free(Sig);
    return 0;
}

static int
TestJournal(const char *FixtureDir)
{
    char Path[512];
    size_t Size;
    uint8_t *Text;
    S1Bool Intact;
    uint64_t Last;

    snprintf(Path, sizeof(Path), "%s/journal-ok.jsonl", FixtureDir);
    Text = ReadFile(Path, &Size);
    Last = S1JournalVerifyChain((const char *)Text, Size, &Intact);
    CHECK(Intact && Last == 3, "intact 3-entry chain verifies");

    snprintf(Path, sizeof(Path), "%s/journal-truncated.jsonl", FixtureDir);
    free(Text);
    Text = ReadFile(Path, &Size);
    S1JournalVerifyChain((const char *)Text, Size, &Intact);
    CHECK(!Intact, "truncated journal breaks the chain");

    /* append produces a chain the verifier accepts */
    {
        uint8_t Hash1[S1_SHA256_SIZE], Hash2[S1_SHA256_SIZE], Zero[S1_SHA256_SIZE];
        static char Buf[1024];
        struct S1_TEST_CAPTURE Ctx = { Buf, 0 };
        memset(Zero, 0, sizeof(Zero));
        Buf[0] = 0;
        CHECK(S1JournalAppend(&Ctx, S1TestAppender, 1, Zero, "tx", "verified", -1, "ok", NULL, Hash1),
              "first journal entry appends");
        CHECK(S1JournalAppend(&Ctx, S1TestAppender, 2, Hash1, "tx", "applying", 1, "ok", NULL, Hash2),
              "second journal entry appends");
        Last = S1JournalVerifyChain(Buf, Ctx.Used, &Intact);
        CHECK(Intact && Last == 2, "appended two-entry chain verifies");
    }
    free(Text);
    return 0;
}

static int
TestRollbackRule(void)
{
    CHECK(!S1ShouldRollback(0, S1_FALSE), "fresh plan: no rollback");
    CHECK(!S1ShouldRollback(1, S1_FALSE), "first boot without health: no rollback yet");
    CHECK(S1ShouldRollback(2, S1_FALSE), "second boot without health: rollback");
    CHECK(!S1ShouldRollback(2, S1_TRUE), "health confirmed: no rollback");
    CHECK(S1IsVerifiableFailure(S1_TRUE), "failure marker makes it a verifiable failure");
    CHECK(!S1IsVerifiableFailure(S1_FALSE), "no marker: unexpected reboot");
    return 0;
}

static int
TestAttemptRoundTrip(void)
{
    S1_ATTEMPT A = { 1, "abc123", 2, S1_FALSE, S1_TRUE, "failed" };
    char Buf[512];
    S1_ATTEMPT B;

    CHECK(S1AttemptSerialize(&A, Buf, sizeof(Buf)) > 0, "attempt.json serializes");
    CHECK(S1AttemptParse(Buf, strlen(Buf), &B), "attempt.json parses back");
    CHECK(B.Attempt == 2 && B.FailureRecorded && !B.HealthConfirmed &&
          strcmp(B.TransactionId, "abc123") == 0,
          "attempt.json round-trips the rollback markers");
    return 0;
}

static int
TestBn(void)
{
    uint32_t R;
    S1TestBnMulMod(3, 3, 5, &R);
    CHECK(R == 4, "bn: 3*3 mod 5 == 4");
    S1TestBnMulMod(2, 10, 1000, &R);
    CHECK(R == 20, "bn: 2*10 mod 1000 == 20");
    S1TestBnMulMod(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFB, &R);
    CHECK(R == 16, "bn: (2^32-1)^2 mod (2^32-5) == 16");
    {
        /* 2^32 * 2^32 mod 7 == 2^64 mod 7 == 2 */
        static const uint32_t Two32[2] = { 0, 1 };
        static const uint32_t Seven[2] = { 7, 0 };
        uint32_t OutW[2];
        S1TestBnMulModW(Two32, Two32, Seven, OutW);
        CHECK(OutW[0] == 2 && OutW[1] == 0, "bn: 2^64 mod 7 == 2");
    }
    {
        uint32_t P;
        S1TestBnPowMod(3, 5, 7, &P);
        CHECK(P == 5, "bn: 3^5 mod 7 == 5");
        S1TestBnPowMod(2, 16, 65537, &P);
        CHECK(P == (uint32_t)((1ULL << 16) % 65537), "bn: 2^16 mod 65537");
    }
    return 0;
}

int
main(int argc, char **argv)
{
    const char *FixtureDir = (argc > 1) ? argv[1] : ".";
    TestPlanAndSignature(FixtureDir);
    TestJournal(FixtureDir);
    TestRollbackRule();
    TestAttemptRoundTrip();
    TestBn();
    if (g_Failures)
    {
        printf("%d FAILURES\n", g_Failures);
        return 1;
    }
    printf("ALL S1 CORE TESTS PASSED\n");
    return 0;
}

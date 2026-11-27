/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     RtlLookupFunctionEntry / RtlVirtualUnwind on synthetic frames:
 *              UWOP_SET_FPREG_LARGE (opcode 11) and out-of-range RIP leaf unwind
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include "precomp.h"

#ifdef _M_AMD64

/* AMD64 unwind opcodes (AMD64 unwind specification; the tree headers do not
   define them) */
#define TEST_UWOP_PUSH_NONVOL       0
#define TEST_UWOP_SET_FPREG         3
#define TEST_UWOP_SET_FPREG_LARGE   11

#define TEST_REG_RBP                5

#pragma pack(push, 1)
typedef struct _TEST_UNWIND_CODE
{
    union
    {
        struct
        {
            UCHAR CodeOffset;
            UCHAR UnwindOp : 4;
            UCHAR OpInfo : 4;
        } s;
        USHORT FrameOffset;
    } u;
} TEST_UNWIND_CODE, *PTEST_UNWIND_CODE;

typedef struct _TEST_UNWIND_INFO
{
    UCHAR Version : 3;
    UCHAR Flags : 5;
    UCHAR SizeOfProlog;
    UCHAR CountOfCodes;
    UCHAR FrameRegister : 4;
    UCHAR FrameOffset : 4;
    TEST_UNWIND_CODE UnwindCode[1];
} TEST_UNWIND_INFO, *PTEST_UNWIND_INFO;
#pragma pack(pop)

/* code bytes the synthetic entries point into; never executed */
static UCHAR g_Code[64];
static UCHAR g_Stack[64];

#define MAGIC_RBP   0x1122334455667788ULL
#define MAGIC_RIP   0x8877665544332211ULL
#define MAGIC_RIP2  0xDEADBEEFCAFEF00DULL

static
VOID
BuildPushRbpFunction(PRUNTIME_FUNCTION Function, PUCHAR Xdata)
{
    PTEST_UNWIND_INFO Info = (PTEST_UNWIND_INFO)Xdata;

    RtlZeroMemory(Xdata, 32);
    Info->Version = 1;
    Info->SizeOfProlog = 1;
    Info->CountOfCodes = 1;
    Info->UnwindCode[0].u.s.CodeOffset = 1;
    Info->UnwindCode[0].u.s.UnwindOp = TEST_UWOP_PUSH_NONVOL;
    Info->UnwindCode[0].u.s.OpInfo = TEST_REG_RBP;

    Function->BeginAddress = 0;
    Function->EndAddress = 8;
    Function->UnwindData = 16;
}

static
VOID
BuildSetFpregLargeFunction(PRUNTIME_FUNCTION Function, PUCHAR Xdata)
{
    PTEST_UNWIND_INFO Info = (PTEST_UNWIND_INFO)Xdata;

    RtlZeroMemory(Xdata, 32);
    Info->Version = 1;
    Info->SizeOfProlog = 4;
    /* push rbp; set fpreg rbp, rsp + 2 * 16 */
    Info->CountOfCodes = 3;
    Info->FrameRegister = TEST_REG_RBP;
    Info->UnwindCode[0].u.s.CodeOffset = 1;
    Info->UnwindCode[0].u.s.UnwindOp = TEST_UWOP_PUSH_NONVOL;
    Info->UnwindCode[0].u.s.OpInfo = TEST_REG_RBP;
    Info->UnwindCode[1].u.s.CodeOffset = 3;
    Info->UnwindCode[1].u.s.UnwindOp = TEST_UWOP_SET_FPREG_LARGE;
    Info->UnwindCode[1].u.s.OpInfo = TEST_REG_RBP;
    Info->UnwindCode[2].u.FrameOffset = 2;

    Function->BeginAddress = 0;
    Function->EndAddress = 8;
    Function->UnwindData = 16;
}

static
VOID
TestPushRbpSanity(void)
{
    RUNTIME_FUNCTION Function;
    UCHAR Xdata[32];
    ULONG_PTR ImageBase;
    PRUNTIME_FUNCTION Found;
    CONTEXT Context;
    PVOID HandlerData;
    ULONG64 EstablisherFrame;
    PEXCEPTION_ROUTINE Handler;

    BuildPushRbpFunction(&Function, Xdata);
    ok(RtlAddFunctionTable(&Function, 1, (ULONG_PTR)g_Code), "RtlAddFunctionTable failed\n");

    Found = RtlLookupFunctionEntry((ULONG_PTR)g_Code + 4, &ImageBase, NULL);
    ok(Found == &Function, "lookup %p, expected %p\n", Found, &Function);
    ok(ImageBase == (ULONG_PTR)g_Code, "ImageBase 0x%I64x\n", (ULONG64)ImageBase);

    *(ULONG64*)&g_Stack[0] = MAGIC_RBP;
    *(ULONG64*)&g_Stack[8] = MAGIC_RIP;
    RtlZeroMemory(&Context, sizeof(Context));
    Context.Rsp = (ULONG64)(ULONG_PTR)g_Stack;

    Handler = RtlVirtualUnwind(0, ImageBase, (ULONG_PTR)g_Code + 4, &Function,
                               &Context, &HandlerData, &EstablisherFrame, NULL);
    ok(Handler == NULL, "Handler %p\n", Handler);
    ok(Context.Rbp == MAGIC_RBP, "Rbp 0x%I64x, expected 0x%I64x\n", Context.Rbp, MAGIC_RBP);
    ok(Context.Rip == MAGIC_RIP, "Rip 0x%I64x, expected 0x%I64x\n", Context.Rip, MAGIC_RIP);
    ok(Context.Rsp == (ULONG64)(ULONG_PTR)g_Stack + 16, "Rsp 0x%I64x\n", Context.Rsp);

    ok(RtlDeleteFunctionTable(&Function), "RtlDeleteFunctionTable failed\n");
}

static
VOID
TestSetFpregLarge(void)
{
    RUNTIME_FUNCTION Function;
    UCHAR Xdata[32];
    ULONG_PTR ImageBase;
    PRUNTIME_FUNCTION Found;
    CONTEXT Context;
    PVOID HandlerData;
    ULONG64 EstablisherFrame;
    PEXCEPTION_ROUTINE Handler;

    BuildSetFpregLargeFunction(&Function, Xdata);
    ok(RtlAddFunctionTable(&Function, 1, (ULONG_PTR)g_Code), "RtlAddFunctionTable failed\n");

    Found = RtlLookupFunctionEntry((ULONG_PTR)g_Code + 7, &ImageBase, NULL);
    ok(Found == &Function, "lookup %p, expected %p\n", Found, &Function);

    *(ULONG64*)&g_Stack[0] = MAGIC_RBP;
    *(ULONG64*)&g_Stack[8] = MAGIC_RIP;
    RtlZeroMemory(&Context, sizeof(Context));
    Context.Rsp = (ULONG64)(ULONG_PTR)g_Stack;

    Handler = RtlVirtualUnwind(0, ImageBase, (ULONG_PTR)g_Code + 7, &Function,
                               &Context, &HandlerData, &EstablisherFrame, NULL);
    ok(Handler == NULL, "Handler %p\n", Handler);
    /* full prolog unwind: pop rbp, then rip; opcode 11 must size as two slots */
    ok(Context.Rbp == MAGIC_RBP, "Rbp 0x%I64x, expected 0x%I64x\n", Context.Rbp, MAGIC_RBP);
    ok(Context.Rip == MAGIC_RIP, "Rip 0x%I64x, expected 0x%I64x\n", Context.Rip, MAGIC_RIP);
    ok(Context.Rsp == (ULONG64)(ULONG_PTR)g_Stack + 16, "Rsp 0x%I64x\n", Context.Rsp);

    ok(RtlDeleteFunctionTable(&Function), "RtlDeleteFunctionTable failed\n");
}

static
VOID
TestOutOfRangeRip(void)
{
    ULONG_PTR ImageBase;
    PRUNTIME_FUNCTION Found;
    CONTEXT Context;
    PVOID HandlerData;
    ULONG64 EstablisherFrame;
    PEXCEPTION_ROUTINE Handler;

    Found = RtlLookupFunctionEntry((ULONG_PTR)0x10000, &ImageBase, NULL);
    ok(Found == NULL, "lookup %p for out-of-range RIP\n", Found);

    *(ULONG64*)&g_Stack[0] = MAGIC_RIP2;
    RtlZeroMemory(&Context, sizeof(Context));
    Context.Rip = 0x10000;
    Context.Rsp = (ULONG64)(ULONG_PTR)g_Stack;

    Handler = RtlVirtualUnwind(0, 0, 0x10000, NULL, &Context,
                               &HandlerData, &EstablisherFrame, NULL);
    ok(Handler == NULL, "Handler %p\n", Handler);
    ok(Context.Rip == MAGIC_RIP2, "Rip 0x%I64x, expected 0x%I64x\n", Context.Rip, MAGIC_RIP2);
    ok(Context.Rsp == (ULONG64)(ULONG_PTR)g_Stack + 8, "Rsp 0x%I64x\n", Context.Rsp);
}

START_TEST(Unwind)
{
    TestPushRbpSanity();
    TestSetFpregLarge();
    TestOutOfRangeRip();
}

#else

START_TEST(Unwind)
{
    skip("amd64 only\n");
}

#endif

/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the AMD64 initial context built by RtlInitializeContext
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Windows 11 26100 copies the caller's values: Rip = InitialPc,
 * Rsp = InitialSp, Rcx = Parameter. Rdx, Rbp and the selectors stay 0,
 * EFlags is 0x200 and ContextFlags is CONTROL | INTEGER | FLOATING_POINT.
 */

#include "precomp.h"

START_TEST(RtlInitializeContext)
{
    static CONTEXT Context;
    static DECLSPEC_ALIGN(16) UCHAR Stack[0x2000];
    PVOID InitialSp = &Stack[0x1800];
    PVOID InitialPc = (PVOID)(ULONG_PTR)0x140001000ULL;
    PVOID Parameter = (PVOID)(ULONG_PTR)0x1234;

    RtlZeroMemory(&Context, sizeof(Context));
    RtlInitializeContext(NtCurrentProcess(),
                         &Context,
                         Parameter,
                         (PTHREAD_START_ROUTINE)InitialPc,
                         (PINITIAL_TEB)InitialSp);

    ok(Context.ContextFlags == (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT),
       "ContextFlags = 0x%lx, expected 0x%lx\n",
       Context.ContextFlags, (ULONG)(CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT));
    ok(Context.Rip == (ULONG64)(ULONG_PTR)InitialPc, "Rip = 0x%I64x, expected 0x%I64x\n",
       Context.Rip, (ULONG64)(ULONG_PTR)InitialPc);
    ok(Context.Rsp == (ULONG64)(ULONG_PTR)InitialSp, "Rsp = 0x%I64x, expected 0x%I64x\n",
       Context.Rsp, (ULONG64)(ULONG_PTR)InitialSp);
    ok(Context.Rcx == (ULONG64)(ULONG_PTR)Parameter, "Rcx = 0x%I64x, expected 0x%I64x\n",
       Context.Rcx, (ULONG64)(ULONG_PTR)Parameter);
    ok(Context.Rdx == 0, "Rdx = 0x%I64x, expected 0\n", Context.Rdx);
    ok(Context.Rbp == 0, "Rbp = 0x%I64x, expected 0\n", Context.Rbp);
    ok(Context.EFlags == 0x200, "EFlags = 0x%lx, expected 0x200\n", Context.EFlags);
    ok(Context.SegCs == 0, "SegCs = 0x%x, expected 0\n", Context.SegCs);
    ok(Context.SegSs == 0, "SegSs = 0x%x, expected 0\n", Context.SegSs);
}

/*
 * PROJECT:     LiberNT host tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver Verifier lock order graph (ntoskrnl/vf/vfdeadlock.c) against stub kernel declarations
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Build and run from the source root:
 *   gcc -std=c11 -Wall -Wextra -I submodules/host-tests/vf/stub -o t_deadlock \
 *       submodules/host-tests/vf/t_deadlock.c ntoskrnl/vf/vfdeadlock.c && ./t_deadlock
 */

#include <ntoskrnl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static PKTHREAD CurrentThread = (PKTHREAD)0x1000;
static KIRQL CurrentIrql;
static ULONG_PTR StackLow = 0xF0000000, StackHigh = 0xF0010000;
static ULONG LastRule;
static ULONG Lines;
static int Failures;

PKTHREAD KeGetCurrentThread(void) { return CurrentThread; }
KIRQL KeGetCurrentIrql(void) { return CurrentIrql; }
void KeInitializeSpinLock(KSPIN_LOCK *Lock) { *Lock = 0; }
LONG InterlockedIncrement(PLONG Value) { return ++*Value; }
LONG InterlockedDecrement(PLONG Value) { return --*Value; }

void KeAcquireSpinLock(KSPIN_LOCK *Lock, KIRQL *OldIrql)
{
    if (*Lock != 0)
        abort();

    *Lock = 1;
    *OldIrql = CurrentIrql;
}

void KeReleaseSpinLock(KSPIN_LOCK *Lock, KIRQL OldIrql)
{
    (void)OldIrql;
    if (*Lock != 1)
        abort();

    *Lock = 0;
}

void IoGetStackLimits(PULONG_PTR Low, PULONG_PTR High)
{
    *Low = StackLow;
    *High = StackHigh;
}

void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
{
    (void)Code; (void)P1; (void)P2; (void)P3; (void)P4;
    abort();
}

ULONG DbgPrint(const char *Format, ...)
{
    va_list Arguments;

    va_start(Arguments, Format);
    LastRule = va_arg(Arguments, ULONG);
    va_end(Arguments);
    Lines++;
    return 0;
}

#define CHECK(Condition) \
    do { if (!(Condition)) { printf("FAIL line %d: %s\n", __LINE__, #Condition); Failures++; } } while (0)

static void Take(ULONG_PTR Lock)
{
    VfLockAcquiring((PVOID)Lock, (PVOID)1);
    VfLockAcquired((PVOID)Lock, (PVOID)1);
}

static void Try(ULONG_PTR Lock)
{
    VfLockAcquired((PVOID)Lock, (PVOID)1);
}

static void Drop(ULONG_PTR Lock)
{
    VfLockReleasing((PVOID)Lock, (PVOID)1);
}

int main(void)
{
    ULONG_PTR i;

    Take(0x10);
    Take(0x20);
    Drop(0x20);
    Drop(0x10);
    CHECK(VfDeadlockRuleHits == 0);

    VfInitializeDeadlockDetection();

    Take(0x10); Take(0x20); Drop(0x20); Drop(0x10);
    Take(0x10); Take(0x20); Drop(0x20); Drop(0x10);
    CHECK(VfDeadlockRuleHits == 0);

    Take(0x20); Take(0x10);
    CHECK(VfDeadlockRuleHits == 1 && LastRule == 0x1001);
    Drop(0x10); Drop(0x20);

    Take(0x20); Take(0x30); Drop(0x30); Drop(0x20);
    Take(0x30); Take(0x10);
    CHECK(VfDeadlockRuleHits == 2 && LastRule == 0x1001);
    Drop(0x10); Drop(0x30);

    Take(0x10); Take(0x10);
    CHECK(VfDeadlockRuleHits == 3 && LastRule == 0x1000);
    Drop(0x10);

    Drop(0x99);
    CHECK(VfDeadlockRuleHits == 4 && LastRule == 0x1007);

    Take(0x40);
    CurrentThread = (PKTHREAD)0x2000;
    Drop(0x40);
    CHECK(VfDeadlockRuleHits == 5 && LastRule == 0x1004);
    CurrentThread = (PKTHREAD)0x1000;

    VfForgetLocksInRange((PVOID)0x10, 0x10);
    Take(0x20); Take(0x10);
    CHECK(VfDeadlockRuleHits == 5);
    Drop(0x10); Drop(0x20);
    Take(0x10); Take(0x20);
    CHECK(VfDeadlockRuleHits == 6 && LastRule == 0x1001);
    Drop(0x20); Drop(0x10);

    Take(0x50);
    VfForgetLocksInRange((PVOID)0x50, 8);
    CHECK(VfDeadlockRuleHits == 7 && LastRule == 0x100B);

    Try(0x60);
    Drop(0x60);
    CHECK(VfDeadlockRuleHits == 7);

    Take(0x70); Try(0x20); Drop(0x20); Drop(0x70);
    Take(0x20); Take(0x70);
    CHECK(VfDeadlockRuleHits == 7);
    Drop(0x70); Drop(0x20);

    Take(StackLow + 0x100); Take(StackLow + 0x200); Drop(StackLow + 0x200); Drop(StackLow + 0x100);
    Take(StackLow + 0x200); Take(StackLow + 0x100); Drop(StackLow + 0x100); Drop(StackLow + 0x200);
    Take(StackLow + 0x100); Take(StackLow + 0x100); Drop(StackLow + 0x100);
    CHECK(VfDeadlockRuleHits == 7);

    CurrentIrql = DISPATCH_LEVEL + 1;
    Take(0x80);
    CurrentIrql = 0;
    Drop(0x80);
    CHECK(VfDeadlockRuleHits == 7);
    Drop(0x81);
    CHECK(VfDeadlockRuleHits == 8 && LastRule == 0x1007);

    CurrentThread = (PKTHREAD)0x3000;
    for (i = 0; i < 3000; i++)
    {
        Take(0x100000 + i * 16);
        Take(0x900000 + i * 16);
        Drop(0x900000 + i * 16);
        Drop(0x100000 + i * 16);
    }
    CHECK(VfDeadlockRuleHits == 8);
    CHECK(VfDeadlockGraphResets > 0);

    Take(0x7000);
    for (i = 0; i < 600; i++)
    {
        Take(0x8000 + i * 8);
        Drop(0x8000 + i * 8);
    }
    Drop(0x7000);
    CHECK(VfDeadlockRuleHits == 8);

    for (i = 0; i < 300; i++)
        Take(0xA000 + i * 8);
    for (i = 0; i < 300; i++)
        Drop(0xA000 + i * 8);
    CHECK(VfDeadlockRuleHits == 8);
    Drop(0xA000);
    CHECK(VfDeadlockRuleHits == 9 && LastRule == 0x1007);

    printf("hits %u, graph resets %u, lines %u, failures %d\n", VfDeadlockRuleHits, VfDeadlockGraphResets, Lines,
           Failures);
    return Failures != 0;
}

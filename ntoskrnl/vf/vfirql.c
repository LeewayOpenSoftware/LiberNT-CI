/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver Verifier: IRQL rules at the lock and wait calls of a verified driver
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#undef KeAcquireSpinLockAtDpcLevel
#undef KeReleaseSpinLockFromDpcLevel
#undef ExAcquireFastMutex
#undef ExReleaseFastMutex
#undef ExTryToAcquireFastMutex

VOID NTAPI KeAcquireSpinLockAtDpcLevel(IN PKSPIN_LOCK SpinLock);
VOID NTAPI KeReleaseSpinLockFromDpcLevel(IN PKSPIN_LOCK SpinLock);
VOID FASTCALL ExAcquireFastMutex(IN OUT PFAST_MUTEX FastMutex);
VOID FASTCALL ExReleaseFastMutex(IN OUT PFAST_MUTEX FastMutex);
BOOLEAN FASTCALL ExTryToAcquireFastMutex(IN OUT PFAST_MUTEX FastMutex);

#define VF_RELEASE_SPIN_LOCK_IRQL       0x32
#define VF_ACQUIRE_FAST_MUTEX_IRQL      0x33
#define VF_RELEASE_FAST_MUTEX_IRQL      0x34
#define VF_WAIT_AT_DISPATCH             0x3B
#define VF_ACQUIRE_AT_DPC_BELOW         0x40
#define VF_RELEASE_FROM_DPC_BELOW       0x41
#define VF_ACQUIRE_SPIN_LOCK_IRQL       0x42

static
VOID
NTAPI
VfKeAcquireSpinLockAtDpcLevel(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql < DISPATCH_LEVEL)
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_ACQUIRE_AT_DPC_BELOW, Irql, (ULONG_PTR)SpinLock, 0);

    VfLockAcquiring(SpinLock, _ReturnAddress());
    KeAcquireSpinLockAtDpcLevel(SpinLock);
    VfLockAcquired(SpinLock, _ReturnAddress());
}

static
VOID
NTAPI
VfKeReleaseSpinLockFromDpcLevel(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql < DISPATCH_LEVEL)
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_RELEASE_FROM_DPC_BELOW, Irql, (ULONG_PTR)SpinLock, 0);

    VfLockReleasing(SpinLock, _ReturnAddress());
    KeReleaseSpinLockFromDpcLevel(SpinLock);
}

#ifdef _WIN64

static
KIRQL
NTAPI
VfKeAcquireSpinLockRaiseToDpc(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql > DISPATCH_LEVEL)
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_ACQUIRE_SPIN_LOCK_IRQL, Irql, (ULONG_PTR)SpinLock, 0);

    VfLockAcquiring(SpinLock, _ReturnAddress());
    Irql = KeAcquireSpinLockRaiseToDpc(SpinLock);
    VfLockAcquired(SpinLock, _ReturnAddress());
    return Irql;
}

static
VOID
NTAPI
VfKeReleaseSpinLock(
    _Inout_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql != DISPATCH_LEVEL)
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_RELEASE_SPIN_LOCK_IRQL, Irql, (ULONG_PTR)SpinLock, 0);

    VfLockReleasing(SpinLock, _ReturnAddress());
    KeReleaseSpinLock(SpinLock, OldIrql);
}

#endif

static
VOID
FASTCALL
VfExAcquireFastMutex(
    _Inout_ PFAST_MUTEX FastMutex)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql > APC_LEVEL)
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_ACQUIRE_FAST_MUTEX_IRQL, Irql, (ULONG_PTR)FastMutex, 0);

    VfLockAcquiring(FastMutex, _ReturnAddress());
    ExAcquireFastMutex(FastMutex);
    VfLockAcquired(FastMutex, _ReturnAddress());
}

static
VOID
FASTCALL
VfExReleaseFastMutex(
    _Inout_ PFAST_MUTEX FastMutex)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql != APC_LEVEL)
    {
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_RELEASE_FAST_MUTEX_IRQL, Irql,
                     (ULONG_PTR)KeGetCurrentThread()->CombinedApcDisable, (ULONG_PTR)FastMutex);
    }

    VfLockReleasing(FastMutex, _ReturnAddress());
    ExReleaseFastMutex(FastMutex);
}

static
BOOLEAN
FASTCALL
VfExTryToAcquireFastMutex(
    _Inout_ PFAST_MUTEX FastMutex)
{
    BOOLEAN Acquired = ExTryToAcquireFastMutex(FastMutex);

    if (Acquired)
        VfLockAcquired(FastMutex, _ReturnAddress());

    return Acquired;
}

static
BOOLEAN
FASTCALL
VfKeTryToAcquireSpinLockAtDpcLevel(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    BOOLEAN Acquired = KeTryToAcquireSpinLockAtDpcLevel(SpinLock);

    if (Acquired)
        VfLockAcquired(SpinLock, _ReturnAddress());

    return Acquired;
}

static
VOID
VfpCheckWait(
    _In_ PVOID Object,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql < DISPATCH_LEVEL)
        return;

    if (Irql == DISPATCH_LEVEL && Timeout != NULL && Timeout->QuadPart == 0)
        return;

    KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_WAIT_AT_DISPATCH, Irql, (ULONG_PTR)Object, (ULONG_PTR)Timeout);
}

static
NTSTATUS
NTAPI
VfKeWaitForSingleObject(
    _In_ PVOID Object,
    _In_ KWAIT_REASON WaitReason,
    _In_ KPROCESSOR_MODE WaitMode,
    _In_ BOOLEAN Alertable,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    VfpCheckWait(Object, Timeout);
    return KeWaitForSingleObject(Object, WaitReason, WaitMode, Alertable, Timeout);
}

static
NTSTATUS
NTAPI
VfKeWaitForMultipleObjects(
    _In_ ULONG Count,
    _In_reads_(Count) PVOID Object[],
    _In_ WAIT_TYPE WaitType,
    _In_ KWAIT_REASON WaitReason,
    _In_ KPROCESSOR_MODE WaitMode,
    _In_ BOOLEAN Alertable,
    _In_opt_ PLARGE_INTEGER Timeout,
    _Out_opt_ PKWAIT_BLOCK WaitBlockArray)
{
    VfpCheckWait(Object, Timeout);
    return KeWaitForMultipleObjects(Count, Object, WaitType, WaitReason, WaitMode, Alertable, Timeout, WaitBlockArray);
}

const VF_THUNK VfIrqlThunks[] =
{
    { (ULONG_PTR)KeAcquireSpinLockAtDpcLevel, (ULONG_PTR)VfKeAcquireSpinLockAtDpcLevel },
    { (ULONG_PTR)KeReleaseSpinLockFromDpcLevel, (ULONG_PTR)VfKeReleaseSpinLockFromDpcLevel },
#ifdef _WIN64
    { (ULONG_PTR)KeAcquireSpinLockRaiseToDpc, (ULONG_PTR)VfKeAcquireSpinLockRaiseToDpc },
    { (ULONG_PTR)KeReleaseSpinLock, (ULONG_PTR)VfKeReleaseSpinLock },
#endif
    { (ULONG_PTR)ExAcquireFastMutex, (ULONG_PTR)VfExAcquireFastMutex },
    { (ULONG_PTR)ExReleaseFastMutex, (ULONG_PTR)VfExReleaseFastMutex },
    { (ULONG_PTR)ExTryToAcquireFastMutex, (ULONG_PTR)VfExTryToAcquireFastMutex },
    { (ULONG_PTR)KeTryToAcquireSpinLockAtDpcLevel, (ULONG_PTR)VfKeTryToAcquireSpinLockAtDpcLevel },
    { (ULONG_PTR)KeWaitForSingleObject, (ULONG_PTR)VfKeWaitForSingleObject },
    { (ULONG_PTR)KeWaitForMultipleObjects, (ULONG_PTR)VfKeWaitForMultipleObjects },
};

const ULONG VfIrqlThunkCount = RTL_NUMBER_OF(VfIrqlThunks);

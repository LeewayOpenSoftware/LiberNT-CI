/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver Verifier: lock order of the spin locks and fast mutexes a verified driver takes
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define VF_DEADLOCK_SELF                0x1000
#define VF_DEADLOCK_HIERARCHY           0x1001
#define VF_DEADLOCK_WRONG_THREAD        0x1004
#define VF_DEADLOCK_UNACQUIRED          0x1007
#define VF_DEADLOCK_DELETED_OWNED       0x100B

#define VF_LOCK_NODES                   512
#define VF_LOCK_EDGES                   2048
#define VF_LOCK_HELD                    256
#define VF_LOCK_NONE                    0xFFFF
#define VF_LOCK_LINES_PRINTED           64
#define VF_LOCK_RESET_LINES             16

typedef struct _VF_LOCK_NODE
{
    PVOID Lock;
    USHORT FirstEdge;
} VF_LOCK_NODE, *PVF_LOCK_NODE;

typedef struct _VF_LOCK_EDGE
{
    USHORT To;
    USHORT Next;
} VF_LOCK_EDGE, *PVF_LOCK_EDGE;

typedef struct _VF_LOCK_HELD_RECORD
{
    PVOID Lock;
    PKTHREAD Thread;
    PVOID Caller;
} VF_LOCK_HELD_RECORD, *PVF_LOCK_HELD_RECORD;

BOOLEAN VfDeadlockDetectionEnabled;
ULONG VfDeadlockRuleHits;
ULONG VfDeadlockGraphResets;

static KSPIN_LOCK VfpGraphLock;
static VF_LOCK_NODE VfpNode[VF_LOCK_NODES];
static VF_LOCK_EDGE VfpEdge[VF_LOCK_EDGES];
static VF_LOCK_HELD_RECORD VfpHeld[VF_LOCK_HELD];
static USHORT VfpFreeEdge;
static USHORT VfpWalk[VF_LOCK_NODES];
static UCHAR VfpSeen[VF_LOCK_NODES];
static LONG VfpHeldDropped;

static
VOID
VfpResetGraphLocked(VOID)
{
    USHORT i;

    for (i = 0; i < VF_LOCK_NODES; i++)
    {
        VfpNode[i].Lock = NULL;
        VfpNode[i].FirstEdge = VF_LOCK_NONE;
    }

    for (i = 0; i < VF_LOCK_EDGES; i++)
        VfpEdge[i].Next = (USHORT)((i + 1 < VF_LOCK_EDGES) ? (i + 1) : VF_LOCK_NONE);

    VfpFreeEdge = 0;
}

VOID
NTAPI
VfInitializeDeadlockDetection(VOID)
{
    KeInitializeSpinLock(&VfpGraphLock);
    VfpResetGraphLocked();
    VfDeadlockDetectionEnabled = TRUE;
}

static
USHORT
VfpFindNodeLocked(
    _In_ PVOID Lock,
    _In_ BOOLEAN Create)
{
    USHORT Free = VF_LOCK_NONE;
    USHORT i;

    for (i = 0; i < VF_LOCK_NODES; i++)
    {
        if (VfpNode[i].Lock == Lock)
            return i;

        if (VfpNode[i].Lock == NULL && Free == VF_LOCK_NONE)
            Free = i;
    }

    if (Create && Free != VF_LOCK_NONE)
        VfpNode[Free].Lock = Lock;

    return Create ? Free : VF_LOCK_NONE;
}

static
BOOLEAN
VfpHasEdgeLocked(
    _In_ USHORT From,
    _In_ USHORT To)
{
    USHORT Edge;

    for (Edge = VfpNode[From].FirstEdge; Edge != VF_LOCK_NONE; Edge = VfpEdge[Edge].Next)
    {
        if (VfpEdge[Edge].To == To)
            return TRUE;
    }

    return FALSE;
}

static
BOOLEAN
VfpAddEdgeLocked(
    _In_ USHORT From,
    _In_ USHORT To)
{
    USHORT Edge = VfpFreeEdge;

    if (Edge == VF_LOCK_NONE)
        return FALSE;

    VfpFreeEdge = VfpEdge[Edge].Next;
    VfpEdge[Edge].To = To;
    VfpEdge[Edge].Next = VfpNode[From].FirstEdge;
    VfpNode[From].FirstEdge = Edge;
    return TRUE;
}

static
BOOLEAN
VfpReachesLocked(
    _In_ USHORT From,
    _In_ USHORT Target)
{
    USHORT Depth = 0;
    USHORT Node, Edge;

    RtlZeroMemory(VfpSeen, sizeof(VfpSeen));
    VfpWalk[Depth++] = From;
    VfpSeen[From] = TRUE;

    while (Depth != 0)
    {
        Node = VfpWalk[--Depth];
        if (Node == Target)
            return TRUE;

        for (Edge = VfpNode[Node].FirstEdge; Edge != VF_LOCK_NONE; Edge = VfpEdge[Edge].Next)
        {
            if (VfpSeen[VfpEdge[Edge].To])
                continue;

            VfpSeen[VfpEdge[Edge].To] = TRUE;
            VfpWalk[Depth++] = VfpEdge[Edge].To;
        }
    }

    return FALSE;
}

static
VOID
VfpRemoveNodeLocked(
    _In_ USHORT Dead)
{
    USHORT *Link;
    USHORT Edge, i;

    for (i = 0; i < VF_LOCK_NODES; i++)
    {
        if (VfpNode[i].Lock == NULL)
            continue;

        Link = &VfpNode[i].FirstEdge;
        while (*Link != VF_LOCK_NONE)
        {
            Edge = *Link;
            if (i != Dead && VfpEdge[Edge].To != Dead)
            {
                Link = &VfpEdge[Edge].Next;
                continue;
            }

            *Link = VfpEdge[Edge].Next;
            VfpEdge[Edge].Next = VfpFreeEdge;
            VfpFreeEdge = Edge;
        }
    }

    VfpNode[Dead].Lock = NULL;
}

static
BOOLEAN
VfpIsDeadlockRuleFatal(
    _In_ ULONG Rule)
{
    switch (Rule)
    {
        case VF_DEADLOCK_SELF:
            return FALSE;
        case VF_DEADLOCK_HIERARCHY:
            return FALSE;
        case VF_DEADLOCK_WRONG_THREAD:
            return TRUE;
        case VF_DEADLOCK_UNACQUIRED:
            return FALSE;
        case VF_DEADLOCK_DELETED_OWNED:
            return FALSE;
        default:
            return FALSE;
    }
}

static
VOID
VfpGraphIsFullLocked(VOID)
{
    VfpResetGraphLocked();
    VfDeadlockGraphResets++;
    if (VfDeadlockGraphResets <= VF_LOCK_RESET_LINES)
    {
        DbgPrint("Driver Verifier: the lock order graph was full and was cleared (%lu so far), lock orders seen "
                 "before are forgotten\n", VfDeadlockGraphResets);
    }
}

static
VOID
VfpReportDeadlockRule(
    _In_ ULONG Rule,
    _In_ PVOID Lock,
    _In_ ULONG_PTR Second,
    _In_ ULONG_PTR Third,
    _In_ PVOID Caller)
{
    ULONG Hits = (ULONG)InterlockedIncrement((PLONG)&VfDeadlockRuleHits);

    if (VfpIsDeadlockRuleFatal(Rule))
    {
        if (Rule == VF_DEADLOCK_WRONG_THREAD || Rule == VF_DEADLOCK_DELETED_OWNED)
            KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, Rule, (ULONG_PTR)Lock, Second, Third);

        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, Rule, (ULONG_PTR)Lock, 0, 0);
    }

    if (Hits <= VF_LOCK_LINES_PRINTED)
    {
        DbgPrint("Driver Verifier (log only): deadlock rule 0x%lx, lock %p, %p, %p, caller %p\n",
                 Rule, Lock, (PVOID)Second, (PVOID)Third, Caller);
    }
}

static
BOOLEAN
VfpConsumeDroppedAcquire(VOID)
{
    LONG Dropped;

    do
    {
        Dropped = VfpHeldDropped;
        if (Dropped <= 0)
            return FALSE;
    } while (InterlockedCompareExchange(&VfpHeldDropped, Dropped - 1, Dropped) != Dropped);

    return TRUE;
}

static
BOOLEAN
VfpIsOnCurrentStack(
    _In_ PVOID Lock)
{
    ULONG_PTR Low, High;

    IoGetStackLimits(&Low, &High);
    return (BOOLEAN)((ULONG_PTR)Lock >= Low && (ULONG_PTR)Lock < High);
}

VOID
NTAPI
VfLockAcquiring(
    _In_ PVOID Lock,
    _In_ PVOID Caller)
{
    PKTHREAD Thread = KeGetCurrentThread();
    PVOID Recursive = NULL, Inverted = NULL, InvertedCaller = NULL;
    BOOLEAN IsRecursive = FALSE;
    USHORT From, To;
    KIRQL OldIrql;
    ULONG i;

    if (!VfDeadlockDetectionEnabled || KeGetCurrentIrql() > DISPATCH_LEVEL || VfpIsOnCurrentStack(Lock))
        return;

    KeAcquireSpinLock(&VfpGraphLock, &OldIrql);
    for (i = 0; i < VF_LOCK_HELD; i++)
    {
        if (VfpHeld[i].Lock == NULL || VfpHeld[i].Thread != Thread)
            continue;

        if (VfpHeld[i].Lock == Lock)
        {
            Recursive = VfpHeld[i].Caller;
            IsRecursive = TRUE;
            continue;
        }

        From = VfpFindNodeLocked(VfpHeld[i].Lock, TRUE);
        To = VfpFindNodeLocked(Lock, TRUE);
        if (From == VF_LOCK_NONE || To == VF_LOCK_NONE)
        {
            VfpGraphIsFullLocked();
            break;
        }

        if (VfpHasEdgeLocked(From, To))
            continue;

        if (VfpReachesLocked(To, From))
        {
            Inverted = VfpHeld[i].Lock;
            InvertedCaller = VfpHeld[i].Caller;
            continue;
        }

        if (!VfpAddEdgeLocked(From, To))
        {
            VfpGraphIsFullLocked();
            break;
        }
    }
    KeReleaseSpinLock(&VfpGraphLock, OldIrql);

    if (IsRecursive)
        VfpReportDeadlockRule(VF_DEADLOCK_SELF, Lock, (ULONG_PTR)Recursive, 0, Caller);

    if (Inverted != NULL)
        VfpReportDeadlockRule(VF_DEADLOCK_HIERARCHY, Lock, (ULONG_PTR)Inverted, (ULONG_PTR)InvertedCaller, Caller);
}

VOID
NTAPI
VfLockAcquired(
    _In_ PVOID Lock,
    _In_ PVOID Caller)
{
    PVF_LOCK_HELD_RECORD Record = NULL;
    KIRQL OldIrql;
    ULONG i;

    if (!VfDeadlockDetectionEnabled)
        return;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        InterlockedIncrement(&VfpHeldDropped);
        return;
    }

    if (VfpIsOnCurrentStack(Lock))
        return;

    KeAcquireSpinLock(&VfpGraphLock, &OldIrql);
    for (i = 0; i < VF_LOCK_HELD; i++)
    {
        if (VfpHeld[i].Lock == Lock)
        {
            Record = &VfpHeld[i];
            break;
        }

        if (VfpHeld[i].Lock == NULL && Record == NULL)
            Record = &VfpHeld[i];
    }

    if (Record != NULL)
    {
        Record->Lock = Lock;
        Record->Thread = KeGetCurrentThread();
        Record->Caller = Caller;
    }
    else
    {
        InterlockedIncrement(&VfpHeldDropped);
    }
    KeReleaseSpinLock(&VfpGraphLock, OldIrql);
}

VOID
NTAPI
VfLockReleasing(
    _In_ PVOID Lock,
    _In_ PVOID Caller)
{
    PKTHREAD Thread = KeGetCurrentThread();
    PKTHREAD Owner = NULL;
    BOOLEAN Found = FALSE, Dropped = FALSE;
    KIRQL OldIrql;
    ULONG i;

    if (!VfDeadlockDetectionEnabled)
        return;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        VfpConsumeDroppedAcquire();
        return;
    }

    if (VfpIsOnCurrentStack(Lock))
        return;

    KeAcquireSpinLock(&VfpGraphLock, &OldIrql);
    for (i = 0; i < VF_LOCK_HELD; i++)
    {
        if (VfpHeld[i].Lock != Lock)
            continue;

        Found = TRUE;
        Owner = VfpHeld[i].Thread;
        VfpHeld[i].Lock = NULL;
        break;
    }
    KeReleaseSpinLock(&VfpGraphLock, OldIrql);

    if (!Found)
        Dropped = VfpConsumeDroppedAcquire();

    if (!Found)
    {
        if (!Dropped)
            VfpReportDeadlockRule(VF_DEADLOCK_UNACQUIRED, Lock, 0, 0, Caller);
    }
    else if (Owner != Thread)
    {
        VfpReportDeadlockRule(VF_DEADLOCK_WRONG_THREAD, Lock, (ULONG_PTR)Owner, (ULONG_PTR)Thread, Caller);
    }
}

VOID
NTAPI
VfForgetLocksInRange(
    _In_ PVOID Base,
    _In_ SIZE_T Size)
{
    ULONG_PTR Start = (ULONG_PTR)Base;
    ULONG_PTR End = Start + Size;
    PVOID OwnedLock = NULL;
    PKTHREAD Owner = NULL;
    ULONG Owned = 0;
    KIRQL OldIrql;
    USHORT n;
    ULONG i;

    if (!VfDeadlockDetectionEnabled || KeGetCurrentIrql() > DISPATCH_LEVEL)
        return;

    KeAcquireSpinLock(&VfpGraphLock, &OldIrql);
    for (i = 0; i < VF_LOCK_HELD; i++)
    {
        if (VfpHeld[i].Lock == NULL || (ULONG_PTR)VfpHeld[i].Lock < Start || (ULONG_PTR)VfpHeld[i].Lock >= End)
            continue;

        OwnedLock = VfpHeld[i].Lock;
        Owner = VfpHeld[i].Thread;
        VfpHeld[i].Lock = NULL;
        Owned++;
    }

    for (n = 0; n < VF_LOCK_NODES; n++)
    {
        if (VfpNode[n].Lock != NULL && (ULONG_PTR)VfpNode[n].Lock >= Start && (ULONG_PTR)VfpNode[n].Lock < End)
            VfpRemoveNodeLocked(n);
    }
    KeReleaseSpinLock(&VfpGraphLock, OldIrql);

    if (OwnedLock != NULL)
        VfpReportDeadlockRule(VF_DEADLOCK_DELETED_OWNED, OwnedLock, (ULONG_PTR)Owner, Owned, _ReturnAddress());
}

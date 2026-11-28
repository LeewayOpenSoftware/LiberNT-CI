/* Host stand-in for <ntoskrnl.h>, for the LRM-RS Driver-Verifier FULL-STACK differential only.
 * Provides exactly what ntoskrnl/vf/vfirql.c uses so it compiles UNMODIFIED on the host; the real
 * header is the entire kernel SDK. The wrapped primitives (KeAcquireSpinLock*, ExAcquire*, KeWait*)
 * and KeGetCurrentIrql/KeBugCheckEx/KeGetCurrentThread are defined in the harness. Compiled with
 * -D_WIN64 so the x64 thunk set (the production config) is present and the thunk order is fixed.
 * Our test-support file. Leeway Open Software. */
#pragma once

#include <stdint.h>

typedef long          NTSTATUS;
typedef void          VOID;
typedef uint8_t       BOOLEAN, UCHAR, KIRQL;
typedef uint16_t      USHORT;
typedef uint32_t      ULONG;
typedef int32_t       LONG;
typedef int64_t       LONGLONG;
typedef uint64_t      ULONGLONG;
typedef void         *PVOID;
typedef uintptr_t     ULONG_PTR;

typedef PVOID PKSPIN_LOCK;
typedef PVOID PFAST_MUTEX;
typedef PVOID PKWAIT_BLOCK;
typedef int   KWAIT_REASON;
typedef int   KPROCESSOR_MODE;
typedef int   WAIT_TYPE;

typedef union _LARGE_INTEGER { LONGLONG QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _KTHREAD { LONG CombinedApcDisable; } KTHREAD, *PKTHREAD;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define NTAPI
#define FASTCALL
#define IN
#define OUT
#define OPTIONAL
#define _In_
#define _Inout_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _In_reads_(x)

#define APC_LEVEL      1
#define DISPATCH_LEVEL 2
#define DRIVER_VERIFIER_DETECTED_VIOLATION 0xC4u

#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))

/* The pristine/replacement thunk pair (field names as the kernel uses them). */
typedef struct _VF_THUNK
{
    ULONG_PTR Pristine;
    ULONG_PTR Replacement;
} VF_THUNK;

/* Kernel services (defined in the harness). */
KIRQL KeGetCurrentIrql(void);
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
    __attribute__((noreturn));
PKTHREAD KeGetCurrentThread(void);

/* Primitives the verifier thunks wrap (no-ops in the harness). */
KIRQL KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK SpinLock);
void  KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL OldIrql);
NTSTATUS KeWaitForSingleObject(PVOID Object, KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode,
                               BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS KeWaitForMultipleObjects(ULONG Count, PVOID Object[], WAIT_TYPE WaitType,
                                  KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode,
                                  BOOLEAN Alertable, PLARGE_INTEGER Timeout, PKWAIT_BLOCK WaitBlockArray);

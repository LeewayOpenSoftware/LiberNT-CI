/* Host stand-in for <ntoskrnl.h>, for the LRM-RS Driver-Verifier Stage 4 (deadlock / lock-order)
 * FULL-STACK differential only. Provides exactly what ntoskrnl/vf/vfdeadlock.c uses so it compiles
 * UNMODIFIED on the host; the real header is the entire kernel SDK. The Ke/Io services, KeBugCheckEx
 * and DbgPrint are defined in the harness; KeGetCurrentThread and IoGetStackLimits are controllable
 * so the harness can place locks off the "current stack" and switch the running thread.
 * Our test-support file. Leeway Open Software. */
#pragma once

#include <stdint.h>
#include <string.h>

typedef long          NTSTATUS;
typedef void          VOID;
typedef uint8_t       BOOLEAN, UCHAR, KIRQL;
typedef uint16_t      USHORT;
typedef uint32_t      ULONG;
typedef int32_t       LONG, *PLONG;
typedef void         *PVOID;
typedef uintptr_t     ULONG_PTR, SIZE_T;
typedef ULONG_PTR     KSPIN_LOCK, *PKSPIN_LOCK;
typedef void         *PKTHREAD;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define NTAPI
#define _In_

#define DISPATCH_LEVEL 2
#define DRIVER_VERIFIER_DETECTED_VIOLATION 0xC4u

#define _ReturnAddress() __builtin_return_address(0)
#define RtlZeroMemory(d, l) memset((d), 0, (size_t)(l))
#define InterlockedIncrement(p) (++(*(p)))

static inline LONG InterlockedCompareExchange(LONG volatile *Dest, LONG Exchange, LONG Comparand)
{
    LONG old = *Dest;
    if (old == Comparand)
        *Dest = Exchange;
    return old;
}

/* Spin lock: single-threaded host, no-ops. */
#define KeInitializeSpinLock(l) ((void)(*(l) = 0))
#define KeAcquireSpinLock(l, oi) ((void)(*(oi) = 0))
#define KeReleaseSpinLock(l, oi) ((void)0)

/* Kernel services (defined in the harness). */
KIRQL KeGetCurrentIrql(void);
PKTHREAD KeGetCurrentThread(void);
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
    __attribute__((noreturn));
unsigned long DbgPrint(const char *Format, ...);
void IoGetStackLimits(ULONG_PTR *LowLimit, ULONG_PTR *HighLimit);

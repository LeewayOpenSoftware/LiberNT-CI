#pragma once

#include <stddef.h>
#include <string.h>

typedef void VOID;
typedef void *PVOID;
typedef unsigned char UCHAR, BOOLEAN, KIRQL;
typedef unsigned short USHORT;
typedef unsigned int ULONG;
typedef int LONG, *PLONG;
typedef unsigned long long ULONG_PTR, *PULONG_PTR, SIZE_T;
typedef struct _KTHREAD *PKTHREAD;
typedef ULONG_PTR KSPIN_LOCK;

#define NTAPI
#define _In_
#define TRUE 1
#define FALSE 0
#define DISPATCH_LEVEL 2
#define DRIVER_VERIFIER_DETECTED_VIOLATION 0xC4

PKTHREAD KeGetCurrentThread(void);
KIRQL KeGetCurrentIrql(void);
void KeInitializeSpinLock(KSPIN_LOCK *Lock);
void KeAcquireSpinLock(KSPIN_LOCK *Lock, KIRQL *OldIrql);
void KeReleaseSpinLock(KSPIN_LOCK *Lock, KIRQL OldIrql);
LONG InterlockedIncrement(PLONG Value);
LONG InterlockedDecrement(PLONG Value);
void IoGetStackLimits(PULONG_PTR Low, PULONG_PTR High);
__attribute__((noreturn)) void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4);
ULONG DbgPrint(const char *Format, ...);

#define RtlZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#define _ReturnAddress() __builtin_return_address(0)

extern BOOLEAN VfDeadlockDetectionEnabled;
extern ULONG VfDeadlockRuleHits;
extern ULONG VfDeadlockGraphResets;

VOID NTAPI VfInitializeDeadlockDetection(VOID);
VOID NTAPI VfLockAcquiring(PVOID Lock, PVOID Caller);
VOID NTAPI VfLockAcquired(PVOID Lock, PVOID Caller);
VOID NTAPI VfLockReleasing(PVOID Lock, PVOID Caller);
VOID NTAPI VfForgetLocksInRange(PVOID Base, SIZE_T Size);

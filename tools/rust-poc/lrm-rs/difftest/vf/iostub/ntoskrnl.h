/* Host stand-in for <ntoskrnl.h>, for the LRM-RS Driver-Verifier Stage 3 (I/O) FULL-STACK
 * differential only. Provides exactly what ntoskrnl/vf/vfio.c uses so it compiles UNMODIFIED on the
 * host; the real header is the entire kernel SDK. IRP/DEVICE_OBJECT/DRIVER_OBJECT carry only the
 * fields vfio.c reads. The Io, Ke and Ex services, KeBugCheckEx and DbgPrint are defined in the
 * harness. IO_TYPE_* / STATUS_PENDING follow public NT and are fed identically to C and Rust.
 * Our test-support file. Leeway Open Software. */
#pragma once

#include <stdint.h>
#include <string.h>

typedef long          NTSTATUS;
typedef void          VOID;
typedef uint8_t       BOOLEAN, UCHAR, KIRQL;
typedef char          CCHAR;
typedef int16_t       CSHORT;
typedef uint16_t      USHORT, WCHAR;
typedef uint32_t      ULONG, *PULONG;
typedef int32_t       LONG, *PLONG;
typedef void         *PVOID;
typedef WCHAR        *PWSTR;
typedef uintptr_t     ULONG_PTR, SIZE_T;
typedef ULONG_PTR     KSPIN_LOCK, *PKSPIN_LOCK;

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
#define _In_
#define _In_opt_
#define _Inout_
#define _Out_

#define PAGE_SHIFT 12
#define DISPATCH_LEVEL 2
#define DRIVER_VERIFIER_IOMANAGER_VIOLATION 0xC9u
#define LDRP_IMAGE_VERIFYING 0x00020000u
#define STATUS_PENDING ((NTSTATUS)0x00000103L)

/* IO_TYPE tags (public NT). */
#define IO_TYPE_IRP    6
#define IO_TYPE_DEVICE 3

/* POOL_TYPE values vfio.c uses for its own entry allocation. */
typedef int POOL_TYPE;
#define NonPagedPoolNx 0x200

#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))
#define _ReturnAddress() __builtin_return_address(0)
#define InterlockedIncrement(p) (++(*(p)))
#define InterlockedDecrement(p) (--(*(p)))

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
#define InitializeListHead(h) ((h)->Flink = (h)->Blink = (h))
#define IsListEmpty(h) ((h)->Flink == (h))
#define InsertHeadList(h, e) do { PLIST_ENTRY h_ = (h), e_ = (e), f_ = h_->Flink; \
    e_->Flink = f_; e_->Blink = h_; f_->Blink = e_; h_->Flink = e_; } while (0)
#define RemoveEntryList(e) do { PLIST_ENTRY e_ = (e), f_ = e_->Flink, b_ = e_->Blink; \
    b_->Flink = f_; f_->Blink = b_; } while (0)
#define CONTAINING_RECORD(addr, type, field) \
    ((type *)((char *)(addr) - (char *)(&((type *)0)->field)))

typedef struct _IO_STATUS_BLOCK { NTSTATUS Status; ULONG_PTR Information; } IO_STATUS_BLOCK;

struct _DEVICE_OBJECT;

typedef struct _IRP
{
    CSHORT Type;
    CCHAR StackCount;
    CCHAR CurrentLocation;
    IO_STATUS_BLOCK IoStatus;
    PVOID CancelRoutine;
    LIST_ENTRY ThreadListEntry;
    /* IoGetCurrentIrpStackLocation(Irp) resolves to this (real NT: Tail.Overlay.CurrentStackLocation). */
    struct _IO_STACK_LOCATION *CurrentStackLocation;
} IRP, *PIRP;

typedef struct _DRIVER_OBJECT
{
    PVOID DriverStart;
    ULONG DriverSize;
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _DEVICE_OBJECT
{
    CSHORT Type;
    PDRIVER_OBJECT DriverObject;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef struct _IO_STACK_LOCATION
{
    PDEVICE_OBJECT DeviceObject;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

#define IoGetCurrentIrpStackLocation(Irp) ((Irp)->CurrentStackLocation)

typedef struct _LDR_DATA_TABLE_ENTRY
{
    PVOID DllBase;
    ULONG SizeOfImage;
    ULONG Flags;
    UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

typedef struct _VF_THUNK { ULONG_PTR Pristine; ULONG_PTR Replacement; } VF_THUNK;

/* Spin lock: single-threaded host, no-ops. */
#define KeInitializeSpinLock(l) ((void)(*(l) = 0))
#define KeAcquireSpinLock(l, oi) ((void)(*(oi) = 0))
#define KeReleaseSpinLock(l, oi) ((void)0)

/* Kernel services (defined in the harness). */
KIRQL KeGetCurrentIrql(void);
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
    __attribute__((noreturn));
unsigned long DbgPrint(const char *Format, ...);

PVOID ExAllocatePoolWithTag(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
void  ExFreePoolWithTag(PVOID P, ULONG Tag);

/* The IRP primitives the thunks wrap / vfio.c calls (defined in the harness). */
NTSTATUS NTAPI IoCallDriver(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
VOID NTAPI IoCompleteRequest(IN PIRP Irp, IN CCHAR PriorityBoost);
NTSTATUS FASTCALL IofCallDriver(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
VOID FASTCALL IofCompleteRequest(IN PIRP Irp, IN CCHAR PriorityBoost);
VOID NTAPI IoFreeIrp(IN PIRP Irp);
VOID NTAPI IoReuseIrp(IN PIRP Irp, IN NTSTATUS Status);
VOID NTAPI IoInitializeIrp(IN PIRP Irp, IN USHORT PacketSize, IN CCHAR StackSize);

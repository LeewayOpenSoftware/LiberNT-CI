/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Native Parity lote 02 probe-only rows: event pairs, retired system
 *              information classes, NT4 heap calls, registry reflection and
 *              DosPathToSessionPath. Records the statuses Windows 11 returns so
 *              Orosi can be compared row by row. Nothing here asserts a Windows
 *              behaviour as fact: every row is a trace line "PROBE ..." and the
 *              same executable runs on Windows 11 and on Orosi.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winreg.h>
#include <pseh/pseh2.h>

/* Classes are given as numbers on purpose: this is a probe of what the system
 * answers for them, whatever the local headers name them. */
#define CLASS_PATH          4
#define CLASS_CALLCOUNT     6
#define CLASS_CALLTIME      10
#define CLASS_PAGEDPOOL     14
#define CLASS_NONPAGEDPOOL  15
#define CLASS_VDMINSTEMUL   19
#define CLASS_VDMBOP        20
#define CLASS_OBSOLETE0     32

typedef NTSTATUS (NTAPI *PFN_CREATEPAIR)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
typedef NTSTATUS (NTAPI *PFN_H)(HANDLE);
typedef NTSTATUS (NTAPI *PFN_OPENPAIR)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
typedef ULONG    (NTAPI *PFN_USAGE)(PVOID, ULONG, PVOID);
typedef NTSTATUS (NTAPI *PFN_EXTEND)(PVOID, ULONG, PVOID, SIZE_T);
typedef BOOLEAN  (NTAPI *PFN_PROTECT)(PVOID, BOOLEAN);
typedef NTSTATUS (NTAPI *PFN_ZERO)(PVOID, ULONG);
typedef LONG     (WINAPI *PFN_REGKEYFN)(HKEY);
typedef LONG     (WINAPI *PFN_REGQUERY)(HKEY, BOOL *);
typedef DWORD    (WINAPI *PFN_DOSPATHW)(DWORD, LPWSTR, LPWSTR *);
typedef DWORD    (WINAPI *PFN_DOSPATHA)(DWORD, LPSTR, LPSTR *);

static HMODULE NtdllMod;

static PVOID Ntdll(const char *Name)
{
    PVOID Fn = (PVOID)GetProcAddress(NtdllMod, Name);
    if (!Fn)
        trace("PROBE export %s: absent in ntdll\n", Name);
    return Fn;
}

/* ---------------------------------------------------------------- event pairs */

typedef struct _PAIR_RUN
{
    HANDLE Pair;
    PFN_H Fn;
    NTSTATUS Status;
} PAIR_RUN;

static DWORD WINAPI PairThread(PVOID Param)
{
    PAIR_RUN *Run = Param;
    Run->Status = Run->Fn(Run->Pair);
    return 0;
}

/* A wait could block forever: run each call on a thread and give up after 5 s. */
static void ProbePairCall(const char *Name, HANDLE Pair, PFN_H Fn)
{
    PAIR_RUN Run;
    HANDLE Thread;

    if (!Fn)
        return;
    Run.Pair = Pair;
    Run.Fn = Fn;
    Run.Status = (NTSTATUS)0xDEADBEEF;
    Thread = CreateThread(NULL, 0, PairThread, &Run, 0, NULL);
    if (!Thread)
    {
        trace("PROBE %s: could not start thread\n", Name);
        return;
    }
    if (WaitForSingleObject(Thread, 5000) == WAIT_TIMEOUT)
    {
        trace("PROBE %s -> BLOCKED (5 s)\n", Name);
        TerminateThread(Thread, 0);
        /* TerminateThread is asynchronous: wait until the worker is really
         * gone before Run goes out of scope. */
        WaitForSingleObject(Thread, INFINITE);
    }
    else
    {
        trace("PROBE %s -> 0x%lx\n", Name, Run.Status);
    }
    CloseHandle(Thread);
}

static void TestEventPairs(void)
{
    PFN_CREATEPAIR Create = Ntdll("NtCreateEventPair");
    PFN_OPENPAIR Open = Ntdll("NtOpenEventPair");
    PFN_H SetLow = Ntdll("NtSetLowEventPair");
    PFN_H SetHigh = Ntdll("NtSetHighEventPair");
    PFN_H WaitLow = Ntdll("NtWaitLowEventPair");
    PFN_H WaitHigh = Ntdll("NtWaitHighEventPair");
    PFN_H SetLowWaitHigh = Ntdll("NtSetLowWaitHighEventPair");
    PFN_H SetHighWaitLow = Ntdll("NtSetHighWaitLowEventPair");
    OBJECT_ATTRIBUTES Oa;
    UNICODE_STRING Name;
    HANDLE Pair = NULL, Again = NULL;
    NTSTATUS Status;

    if (!Create)
        return;

    /* unnamed */
    InitializeObjectAttributes(&Oa, NULL, 0, NULL, NULL);
    Status = Create(&Pair, 0x1F03FF /* EVENT_PAIR_ALL_ACCESS */, &Oa);
    trace("PROBE NtCreateEventPair unnamed -> 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        /* Also try the exact minimal access and a NULL attribute block, so the
         * status is not an artefact of the access mask. */
        Status = Create(&Pair, SYNCHRONIZE, NULL);
        trace("PROBE NtCreateEventPair SYNCHRONIZE NULL oa -> 0x%lx handle=%p\n", Status, Pair);
    }

    /* named + open */
    /* The session's own BaseNamedObjects: the global one needs SeCreateGlobalPrivilege. */
    {
        static WCHAR NameBuf[96];
        DWORD Own = 0;

        ProcessIdToSessionId(GetCurrentProcessId(), &Own);
        if (Own == 0)
            wcscpy(NameBuf, L"\\BaseNamedObjects\\np02-evpair");
        else
            _snwprintf(NameBuf, _countof(NameBuf) - 1, L"\\Sessions\\%lu\\BaseNamedObjects\\np02-evpair", Own);
        RtlInitUnicodeString(&Name, NameBuf);
        trace("PROBE named pair path %ls\n", NameBuf);
    }
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    {
        HANDLE Named = NULL;
        Status = Create(&Named, 0x1F03FF, &Oa);
        trace("PROBE NtCreateEventPair named -> 0x%lx\n", Status);
        if (Open)
        {
            Status = Open(&Again, SYNCHRONIZE, &Oa);
            trace("PROBE NtOpenEventPair named -> 0x%lx\n", Status);
            if (Again)
                NtClose(Again);
        }
        if (Named)
            NtClose(Named);
    }

    if (Pair)
    {
        /* Set, then wait: each wait is preceded by the set that satisfies it,
         * so a working implementation does not block. */
        ProbePairCall("NtSetLowEventPair", Pair, SetLow);
        ProbePairCall("NtWaitLowEventPair", Pair, WaitLow);
        ProbePairCall("NtSetHighEventPair", Pair, SetHigh);
        ProbePairCall("NtWaitHighEventPair", Pair, WaitHigh);
        ProbePairCall("NtSetHighEventPair (pre-arm)", Pair, SetHigh);
        ProbePairCall("NtSetLowWaitHighEventPair", Pair, SetLowWaitHigh);
        ProbePairCall("NtSetLowEventPair (pre-arm)", Pair, SetLow);
        ProbePairCall("NtSetHighWaitLowEventPair", Pair, SetHighWaitLow);
        NtClose(Pair);
    }
    /* invalid handle */
    ProbePairCall("NtSetLowEventPair bogus handle", (HANDLE)(ULONG_PTR)0x12345678, SetLow);
    ProbePairCall("NtWaitHighEventPair bogus handle", (HANDLE)(ULONG_PTR)0x12345678, WaitHigh);
}

/* ------------------------------------------------------ retired sysinfo classes */

static void ProbeClass(const char *Name, ULONG Class)
{
    static const ULONG Sizes[] = { 0, 4, 64, 4096 };
    ULONG i;

    for (i = 0; i < _countof(Sizes); i++)
    {
        UCHAR Buffer[4096];
        ULONG Ret = 0xCAFE;
        NTSTATUS Status;

        RtlZeroMemory(Buffer, sizeof(Buffer));
        Status = NtQuerySystemInformation(Class, Buffer, Sizes[i], &Ret);
        trace("PROBE query %s (%lu) size %lu -> 0x%lx ret=%lu\n", Name, Class, Sizes[i], Status, Ret);
    }
    {
        ULONG Zero = 0;
        NTSTATUS Status = NtSetSystemInformation(Class, &Zero, sizeof(Zero));
        trace("PROBE set   %s (%lu) 4 bytes of 0 -> 0x%lx\n", Name, Class, Status);
    }
}

static void TestRetiredClasses(void)
{
    ProbeClass("Path", CLASS_PATH);
    ProbeClass("CallCount", CLASS_CALLCOUNT);
    ProbeClass("CallTime", CLASS_CALLTIME);
    ProbeClass("PagedPool", CLASS_PAGEDPOOL);
    ProbeClass("NonPagedPool", CLASS_NONPAGEDPOOL);
    ProbeClass("VdmInstemul", CLASS_VDMINSTEMUL);
    ProbeClass("VdmBop", CLASS_VDMBOP);
    ProbeClass("Obsolete0", CLASS_OBSOLETE0);
}

/* --------------------------------------------------------------- NT4 heap calls */

/* Run Call() under SEH; on an exception *Res gets the marker and *Exc the code. */
#define PROBE_EXC_MARK 0xE000E000

static void TestHeapCalls(void)
{
    PFN_USAGE Usage = Ntdll("RtlUsageHeap");
    PFN_EXTEND Extend = Ntdll("RtlExtendHeap");
    PFN_PROTECT Protect = Ntdll("RtlProtectHeap");
    PFN_ZERO Zero = Ntdll("RtlZeroHeap");
    PVOID Heap = RtlCreateHeap(HEAP_GROWABLE, NULL, 0, 0, NULL, NULL);
    PVOID Block;
    ULONG UsageBuf[64];

    if (!Heap)
    {
        trace("PROBE could not create a private heap\n");
        return;
    }
    Block = RtlAllocateHeap(Heap, 0, 128);

    if (Usage)
    {
        volatile ULONG Ret = 0;
        volatile ULONG Exc = 0;

        RtlZeroMemory(UsageBuf, sizeof(UsageBuf));
        UsageBuf[0] = sizeof(UsageBuf); /* leading Bytes field, as the contract is described */
        _SEH2_TRY
        {
            Ret = Usage(Heap, 0, UsageBuf);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Exc = _SEH2_GetExceptionCode();
            Ret = PROBE_EXC_MARK;
        }
        _SEH2_END;
        if (Ret == PROBE_EXC_MARK)
            trace("PROBE RtlUsageHeap EXCEPTION 0x%lx\n", Exc);
        trace("PROBE RtlUsageHeap(private, 0, buf) -> 0x%lx first=0x%lx\n", Ret, UsageBuf[0]);

        Exc = 0;
        _SEH2_TRY
        {
            Ret = Usage(Heap, 0, NULL);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Exc = _SEH2_GetExceptionCode();
            Ret = PROBE_EXC_MARK;
        }
        _SEH2_END;
        if (Ret == PROBE_EXC_MARK)
            trace("PROBE RtlUsageHeap NULL EXCEPTION 0x%lx\n", Exc);
        trace("PROBE RtlUsageHeap(private, 0, NULL) -> 0x%lx\n", Ret);
    }
    if (Extend)
    {
        volatile NTSTATUS Status = 0;
        volatile ULONG Exc = 0;

        _SEH2_TRY
        {
            Status = Extend(Heap, 0, NULL, 0);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Exc = _SEH2_GetExceptionCode();
            Status = PROBE_EXC_MARK;
        }
        _SEH2_END;
        if (Status == (NTSTATUS)PROBE_EXC_MARK)
            trace("PROBE RtlExtendHeap EXCEPTION 0x%lx\n", Exc);
        trace("PROBE RtlExtendHeap(private, 0, NULL, 0) -> 0x%lx\n", Status);
    }
    if (Protect)
    {
        volatile BOOLEAN R1 = 0xEE, R2 = 0xEE;
        volatile ULONG Exc = 0;

        _SEH2_TRY
        {
            R1 = Protect(Heap, FALSE);
            R2 = Protect(Heap, TRUE);
            Protect(Heap, FALSE);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Exc = _SEH2_GetExceptionCode();
            trace("PROBE RtlProtectHeap EXCEPTION 0x%lx\n", Exc);
        }
        _SEH2_END;
        trace("PROBE RtlProtectHeap(private, FALSE)=%u then TRUE=%u (restored FALSE)\n", R1, R2);
    }
    if (Zero)
    {
        volatile NTSTATUS Status = 0;
        volatile ULONG Exc = 0;

        _SEH2_TRY
        {
            Status = Zero(Heap, 0);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Exc = _SEH2_GetExceptionCode();
            Status = PROBE_EXC_MARK;
        }
        _SEH2_END;
        if (Status == (NTSTATUS)PROBE_EXC_MARK)
            trace("PROBE RtlZeroHeap EXCEPTION 0x%lx\n", Exc);
        trace("PROBE RtlZeroHeap(private, 0) -> 0x%lx\n", Status);
    }
    if (Block)
        RtlFreeHeap(Heap, 0, Block);
    RtlDestroyHeap(Heap);
}

/* ---------------------------------------------------------- registry reflection */

static void TestReflection(void)
{
    HMODULE Adv = GetModuleHandleW(L"advapi32.dll");
    PFN_REGKEYFN Disable, Enable;
    PFN_REGQUERY Query;
    HKEY Key = NULL;
    LONG Err;
    BOOL Disabled;
    BOOL InitialDisabled = 0;

    if (!Adv)
        Adv = LoadLibraryW(L"advapi32.dll");
    Disable = (PFN_REGKEYFN)GetProcAddress(Adv, "RegDisableReflectionKey");
    Enable = (PFN_REGKEYFN)GetProcAddress(Adv, "RegEnableReflectionKey");
    Query = (PFN_REGQUERY)GetProcAddress(Adv, "RegQueryReflectionKey");
    if (!Disable || !Enable || !Query)
    {
        trace("PROBE reflection exports: disable=%p enable=%p query=%p\n", Disable, Enable, Query);
        return;
    }
    Err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_READ, &Key);
    trace("PROBE RegOpenKeyEx HKLM\\SOFTWARE -> %ld\n", Err);
    if (Err != ERROR_SUCCESS)
        return;

    Disabled = 0x55;
    SetLastError(0xBEEF);
    Err = Query(Key, &Disabled);
    trace("PROBE RegQueryReflectionKey(HKLM\\SOFTWARE) -> %ld disabled=%d lasterr=%lu\n", Err, Disabled, GetLastError());
    InitialDisabled = (Err == ERROR_SUCCESS) ? Disabled : 0;
    Err = Disable(Key);
    trace("PROBE RegDisableReflectionKey(HKLM\\SOFTWARE) -> %ld\n", Err);
    Disabled = 0x55;
    Err = Query(Key, &Disabled);
    trace("PROBE RegQueryReflectionKey after disable -> %ld disabled=%d\n", Err, Disabled);
    Err = Enable(Key);
    trace("PROBE RegEnableReflectionKey(HKLM\\SOFTWARE) -> %ld\n", Err);
    if (InitialDisabled)
    {
        Err = Disable(Key);
        trace("PROBE restored the initial (disabled) state -> %ld\n", Err);
    }
    Disabled = 0x55;
    Err = Query(Key, &Disabled);
    trace("PROBE RegQueryReflectionKey after enable -> %ld disabled=%d\n", Err, Disabled);
    RegCloseKey(Key);

    Err = Query((HKEY)(ULONG_PTR)0x12345678, &Disabled);
    trace("PROBE RegQueryReflectionKey(bogus handle) -> %ld\n", Err);
    Err = Disable((HKEY)(ULONG_PTR)0x12345678);
    trace("PROBE RegDisableReflectionKey(bogus handle) -> %ld\n", Err);
    Err = Query(HKEY_LOCAL_MACHINE, &Disabled);
    trace("PROBE RegQueryReflectionKey(HKLM predefined) -> %ld disabled=%d\n", Err, Disabled);
}

/* ------------------------------------------------------- DosPathToSessionPath */

static void TestDosPathToSessionPath(void)
{
    HMODULE K32 = GetModuleHandleW(L"kernel32.dll");
    PFN_DOSPATHW W = (PFN_DOSPATHW)GetProcAddress(K32, "DosPathToSessionPathW");
    PFN_DOSPATHA A = (PFN_DOSPATHA)GetProcAddress(K32, "DosPathToSessionPathA");
    static WCHAR InW[] = L"C:\\Windows\\notepad.exe";
    static CHAR InA[] = "C:\\Windows\\notepad.exe";
    LPWSTR OutW = (LPWSTR)(ULONG_PTR)0x1;
    LPSTR OutA = (LPSTR)(ULONG_PTR)0x1;
    DWORD Ret, Session = 0, Own = 0;

    ProcessIdToSessionId(GetCurrentProcessId(), &Own);
    trace("PROBE own session = %lu\n", Own);
    if (!W || !A)
    {
        trace("PROBE DosPathToSessionPath exports: W=%p A=%p\n", W, A);
        return;
    }
    for (Session = 0; Session <= Own + 1; Session++)
    {
        OutW = (LPWSTR)(ULONG_PTR)0x1;
        SetLastError(0xBEEF);
        Ret = W(Session, InW, &OutW);
        trace("PROBE DosPathToSessionPathW(%lu, C:\\Windows\\notepad.exe) -> %lu err=%lu out=%ls\n",
              Session, Ret, GetLastError(), (OutW && OutW != (LPWSTR)(ULONG_PTR)0x1) ? OutW : L"(none)");
        if (OutW && OutW != (LPWSTR)(ULONG_PTR)0x1)
            LocalFree(OutW);
    }
    OutA = (LPSTR)(ULONG_PTR)0x1;
    SetLastError(0xBEEF);
    Ret = A(Own, InA, &OutA);
    trace("PROBE DosPathToSessionPathA(%lu, ...) -> %lu err=%lu out=%s\n", Own, Ret, GetLastError(),
          (OutA && OutA != (LPSTR)(ULONG_PTR)0x1) ? OutA : "(none)");
    if (OutA && OutA != (LPSTR)(ULONG_PTR)0x1)
        LocalFree(OutA);

    OutW = (LPWSTR)(ULONG_PTR)0x1;
    {
        volatile DWORD Rv = 0, Err2 = 0, Exc = 0;

        _SEH2_TRY
        {
            SetLastError(0xBEEF);
            Rv = W(Own, NULL, &OutW);
            Err2 = GetLastError();
            trace("PROBE DosPathToSessionPathW(own, NULL path) -> %lu err=%lu\n", Rv, Err2);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Exc = _SEH2_GetExceptionCode();
            trace("PROBE DosPathToSessionPathW(own, NULL path) EXCEPTION 0x%lx\n", Exc);
        }
        _SEH2_END;
        _SEH2_TRY
        {
            SetLastError(0xBEEF);
            Rv = W(Own, InW, NULL);
            Err2 = GetLastError();
            trace("PROBE DosPathToSessionPathW(own, path, NULL out) -> %lu err=%lu\n", Rv, Err2);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Exc = _SEH2_GetExceptionCode();
            trace("PROBE DosPathToSessionPathW(own, path, NULL out) EXCEPTION 0x%lx\n", Exc);
        }
        _SEH2_END;
    }
}

START_TEST(np02_legacy_probe)
{
    NtdllMod = GetModuleHandleW(L"ntdll.dll");
    trace("PROBE np02_legacy_probe start\n");
    TestEventPairs();
    TestRetiredClasses();
    TestHeapCalls();
    TestReflection();
    TestDosPathToSessionPath();
    trace("PROBE np02_legacy_probe end\n");
    ok(TRUE, "probe completed\n");
}

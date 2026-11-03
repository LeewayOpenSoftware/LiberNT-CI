/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     LdrRegisterDllNotification and LdrUnregisterDllNotification, as the
 *              published pages describe them: Flags must be zero, the callback gets
 *              LOADED (1) when a DLL is first loaded and UNLOADED (2) when it goes away,
 *              with the base name and base address of the module
 *
 * The documentation warns that a callback must not call functions of any other module,
 * so the callback below only copies data with its own code.
 */

#include "precomp.h"

typedef struct _LDR_NOTIFY_DATA
{
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_NOTIFY_DATA;

typedef VOID (NTAPI *PFN_NOTIFY)(ULONG, const LDR_NOTIFY_DATA *, PVOID);
typedef NTSTATUS (NTAPI *PFN_REGISTER)(ULONG, PFN_NOTIFY, PVOID, PVOID *);
typedef NTSTATUS (NTAPI *PFN_UNREGISTER)(PVOID);

#define REASON_LOADED   1
#define REASON_UNLOADED 2
#define MAX_EVENTS      64

typedef struct _EVENT
{
    ULONG Reason;
    PVOID Base;
    WCHAR Name[40];
    ULONG Callback;
} EVENT;

typedef struct _SINK
{
    ULONG Id;
    EVENT *Events;
    volatile LONG *Count;
} SINK;

static EVENT g_Events[MAX_EVENTS];
static volatile LONG g_Count;
static WCHAR g_DllPath[MAX_PATH];

static PFN_REGISTER pRegister;
static PFN_UNREGISTER pUnregister;

static
VOID
NTAPI
Callback(ULONG Reason, const LDR_NOTIFY_DATA *Data, PVOID Context)
{
    SINK *Sink = Context;
    LONG Index = *Sink->Count;
    ULONG i, Length;

    if (Index >= MAX_EVENTS)
        return;

    Sink->Events[Index].Reason = Reason;
    Sink->Events[Index].Base = Data->DllBase;
    Sink->Events[Index].Callback = Sink->Id;
    Length = Data->BaseDllName->Length / sizeof(WCHAR);
    if (Length > 39)
        Length = 39;
    for (i = 0; i < Length; i++)
        Sink->Events[Index].Name[i] = Data->BaseDllName->Buffer[i];
    Sink->Events[Index].Name[Length] = 0;

    *Sink->Count = Index + 1;
}

static
BOOL
Same(const WCHAR *A, const WCHAR *B)
{
    return CompareStringOrdinal(A, -1, B, -1, TRUE) == CSTR_EQUAL;
}

static
void
Test_Arguments(void)
{
    SINK Sink = { 1, g_Events, &g_Count };
    PVOID Cookie = NULL;
    NTSTATUS Status;

    Status = pRegister(1, Callback, &Sink, &Cookie);
    ok(!NT_SUCCESS(Status), "Flags other than zero were accepted: %lx\n", Status);

    Status = pRegister(0, NULL, &Sink, &Cookie);
    ok(!NT_SUCCESS(Status), "A NULL callback was accepted: %lx\n", Status);

    Status = pRegister(0, Callback, &Sink, NULL);
    ok(!NT_SUCCESS(Status), "A NULL cookie pointer was accepted: %lx\n", Status);
}

static
BOOL
MakeDll(void)
{
    WCHAR Temp[MAX_PATH], Source[MAX_PATH];

    GetTempPathW(ARRAYSIZE(Temp), Temp);
    StringCchPrintfW(g_DllPath, ARRAYSIZE(g_DllPath), L"%lsldrnotify.%lu.dll", Temp, GetCurrentProcessId());
    GetSystemDirectoryW(Source, ARRAYSIZE(Source));
    StringCchCatW(Source, ARRAYSIZE(Source), L"\\version.dll");
    return CopyFileW(Source, g_DllPath, FALSE);
}

static
void
Test_LoadUnload(void)
{
    SINK First = { 1, g_Events, &g_Count }, Second = { 2, g_Events, &g_Count };
    PVOID Cookie1 = NULL, Cookie2 = NULL;
    NTSTATUS Status;
    HMODULE Module, Again;
    WCHAR *Name;
    LONG i, Loaded1 = 0, Loaded2 = 0, Unloaded1 = 0, Unloaded2 = 0;
    LONG Before;

    Status = pRegister(0, Callback, &First, &Cookie1);
    ok(NT_SUCCESS(Status) && Cookie1, "LdrRegisterDllNotification failed: %lx\n", Status);
    Status = pRegister(0, Callback, &Second, &Cookie2);
    ok(NT_SUCCESS(Status) && Cookie2 && Cookie2 != Cookie1, "The second registration failed: %lx\n", Status);
    if (!Cookie1 || !Cookie2)
        return;

    g_Count = 0;
    Module = LoadLibraryW(g_DllPath);
    ok(Module != NULL, "LoadLibraryW failed with %lu\n", GetLastError());
    if (!Module)
        goto Done;

    Name = wcsrchr(g_DllPath, L'\\') + 1;
    for (i = 0; i < g_Count; i++)
    {
        if (!Same(g_Events[i].Name, Name))
            continue;
        ok(g_Events[i].Reason == REASON_LOADED, "Reason %lu on load\n", g_Events[i].Reason);
        ok(g_Events[i].Base == (PVOID)Module, "Base %p, expected %p\n", g_Events[i].Base, Module);
        if (g_Events[i].Callback == 1) Loaded1++; else Loaded2++;
    }
    ok(Loaded1 == 1 && Loaded2 == 1, "Each callback must hear about the load once (%ld, %ld)\n", Loaded1, Loaded2);

    /* Registration order is the order of the calls */
    for (i = 0; i < g_Count; i++)
        if (Same(g_Events[i].Name, Name))
        {
            ok(g_Events[i].Callback == 1, "The first registered callback must be called first\n");
            break;
        }

    /* Only the first load of a DLL is announced */
    Before = g_Count;
    Again = LoadLibraryW(g_DllPath);
    ok(Again == Module, "A second LoadLibraryW returned another module\n");
    ok(g_Count == Before, "A second load of the same DLL was announced again\n");
    FreeLibrary(Again);

    /* Unload: the last FreeLibrary announces it */
    Before = g_Count;
    ok(FreeLibrary(Module), "FreeLibraryW failed with %lu\n", GetLastError());
    for (i = Before; i < g_Count; i++)
    {
        if (!Same(g_Events[i].Name, Name))
            continue;
        ok(g_Events[i].Reason == REASON_UNLOADED, "Reason %lu on unload\n", g_Events[i].Reason);
        ok(g_Events[i].Base == (PVOID)Module, "Unload base %p, expected %p\n", g_Events[i].Base, Module);
        if (g_Events[i].Callback == 1) Unloaded1++; else Unloaded2++;
    }
    ok(Unloaded1 == 1 && Unloaded2 == 1, "Each callback must hear about the unload once (%ld, %ld)\n", Unloaded1, Unloaded2);

    /* After unregistering the first, only the second is called */
    Status = pUnregister(Cookie1);
    ok(NT_SUCCESS(Status), "LdrUnregisterDllNotification failed: %lx\n", Status);
    Cookie1 = NULL;
    Before = g_Count;
    Module = LoadLibraryW(g_DllPath);
    ok(Module != NULL, "LoadLibraryW failed with %lu\n", GetLastError());
    for (i = Before; i < g_Count; i++)
        ok(g_Events[i].Callback == 2, "The unregistered callback was called\n");
    if (Module)
        FreeLibrary(Module);

Done:
    if (Cookie1)
        pUnregister(Cookie1);
    pUnregister(Cookie2);
}

START_TEST(LdrDllNotification)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");

    pRegister = (PFN_REGISTER)GetProcAddress(Ntdll, "LdrRegisterDllNotification");
    pUnregister = (PFN_UNREGISTER)GetProcAddress(Ntdll, "LdrUnregisterDllNotification");
    ok(pRegister && pUnregister, "The notification functions are not exported\n");
    if (!pRegister || !pUnregister)
        return;

    Test_Arguments();

    if (!MakeDll())
    {
        skip("Could not prepare the test DLL\n");
        return;
    }

    Test_LoadUnload();
    DeleteFileW(g_DllPath);
}

/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Exceptions inside user-mode callbacks reached from the kernel
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <ndk/pstypes.h>

/* A same-thread SendMessage never leaves user mode, so the kernel callbacks of this test are the
   WM_NCCREATE/WM_CREATE that win32k sends from NtUserCreateWindowEx. */

#define CLASS_NAME      L"CallbackExceptionClass"
#define MARKER_CAUGHT   0x1234
#define MARKER_ALIVE    0x4321

/* Exit codes of the child processes; a child the kernel terminates exits with the exception code. */
#define CHILD_RETURNED      1   /* CreateWindowEx returned, nothing caught */
#define CHILD_CAUGHT        2   /* the __except around CreateWindowEx ran */
#define CHILD_WINDOW_MADE   3   /* CreateWindowEx returned a window although the table was NULL */
#define CHILD_BAD_ARGS      9

static volatile int *g_Fault;   /* NULL, kept volatile so the store below stays a real store */
static BOOL g_HandleInside;
static LONG g_CreateMarker;

static LRESULT CALLBACK CallbackWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE)
    {
        if (g_HandleInside)
        {
            /* a handler inside the callback's own frames: found before the walk reaches the callout frame */
            _SEH2_TRY
            {
                *g_Fault = 1;
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                g_CreateMarker = MARKER_CAUGHT;
            }
            _SEH2_END;
            return 0;
        }
        /* no handler anywhere inside the callback */
        *g_Fault = 1;
        return 0;
    }
    if (msg == WM_APP + 1)
        return MARKER_ALIVE;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL RegisterTestClass(void)
{
    WNDCLASSW wc = { 0 };

    wc.lpfnWndProc = CallbackWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = CLASS_NAME;
    return RegisterClassW(&wc) != 0;
}

static HWND CreateTestWindow(void)
{
    return CreateWindowExW(0, CLASS_NAME, L"", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

/* Child: the window procedure faults with no handler of its own; the only __try is around the call
   that made the kernel call back. */
static int ChildPropagate(void)
{
    HWND hwnd = NULL;
    int ret = CHILD_RETURNED;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (!RegisterTestClass())
        return CHILD_BAD_ARGS;
    g_HandleInside = FALSE;
    _SEH2_TRY
    {
        hwnd = CreateTestWindow();
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ret = CHILD_CAUGHT;
    }
    _SEH2_END;
    if (hwnd)
        DestroyWindow(hwnd);
    return ret;
}

/* Child: no callback table while the kernel sends WM_NCCREATE (the shape of a process without user32) */
static int ChildNullTable(void)
{
    PPEB Peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    PVOID Table;
    HWND hwnd;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (!RegisterTestClass())
        return CHILD_BAD_ARGS;
    Table = Peb->KernelCallbackTable;
    Peb->KernelCallbackTable = NULL;
    hwnd = CreateTestWindow();
    Peb->KernelCallbackTable = Table;
    if (hwnd)
    {
        DestroyWindow(hwnd);
        return CHILD_WINDOW_MADE;
    }
    return CHILD_RETURNED;
}

static DWORD RunChild(const char *Mode)
{
    char Module[MAX_PATH], Command[MAX_PATH + 64];
    STARTUPINFOA Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Child;
    DWORD Wait, ExitCode = (DWORD)-1;

    GetModuleFileNameA(NULL, Module, ARRAY_SIZE(Module));
    sprintf(Command, "\"%s\" CallbackException %s", Module, Mode);
    if (!CreateProcessA(NULL, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Child))
    {
        ok(0, "CreateProcess(%s) failed: %lu\n", Mode, GetLastError());
        return ExitCode;
    }
    Wait = WaitForSingleObject(Child.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "child '%s' did not end: %#lx\n", Mode, Wait);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(Child.hProcess, 0xDEAD);
    GetExitCodeProcess(Child.hProcess, &ExitCode);
    CloseHandle(Child.hThread);
    CloseHandle(Child.hProcess);
    trace("child '%s' exited with %#lx\n", Mode, ExitCode);
    return ExitCode;
}

START_TEST(CallbackException)
{
    int Argc;
    char **Argv;
    HWND hwnd;
    DWORD ExitCode;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 3 && !strcmp(Argv[2], "propagate"))
        ExitProcess(ChildPropagate());
    if (Argc >= 3 && !strcmp(Argv[2], "nulltable"))
        ExitProcess(ChildNullTable());

    ok(RegisterTestClass(), "RegisterClass failed: %lu\n", GetLastError());

    /* 1. handled inside the callback: the window is created and stays usable */
    g_HandleInside = TRUE;
    g_CreateMarker = 0;
    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindowEx failed: %lu\n", GetLastError());
    ok(g_CreateMarker == MARKER_CAUGHT, "the handler inside the window procedure did not run (%lx)\n", g_CreateMarker);
    if (hwnd)
    {
        ok(SendMessageW(hwnd, WM_APP + 1, 0, 0) == MARKER_ALIVE, "window unusable after the handled exception\n");
        DestroyWindow(hwnd);
    }

    /* 2. not handled inside the callback: the frames below the callout frame are out of reach of the
       walk (the kernel cleared the nonvolatile registers for the callback), so the exception is
       unhandled and the process ends with its code; the __except around CreateWindowEx never runs */
    ExitCode = RunChild("propagate");
    ok(ExitCode == (DWORD)STATUS_ACCESS_VIOLATION,
       "exception unhandled inside a callback: child exit %#lx, expected STATUS_ACCESS_VIOLATION\n", ExitCode);

    /* 3. the kernel calls back into a process whose table is NULL: the dispatcher answers the kernel,
       window creation fails, the process lives. Windows is not known to check the table; what it does
       is read off the lab run of this test, so the assertion is ours only and the exit code is traced. */
    ExitCode = RunChild("nulltable");
    ok(ExitCode == CHILD_RETURNED || (!is_reactos() && ExitCode == (DWORD)STATUS_ACCESS_VIOLATION),
       "callback with a NULL KernelCallbackTable: child exit %#lx, expected %u\n", ExitCode, CHILD_RETURNED);
}

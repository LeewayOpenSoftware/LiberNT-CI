/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS GUI/console setup
 * FILE:            base/setup/setup/setup.c
 * PURPOSE:         Second stage setup
 * PROGRAMMER:      Eric Kohl
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <strsafe.h>

#define NDEBUG
#include <debug.h>

typedef INT (WINAPI *PINSTALL_REACTOS)(INT argc, WCHAR** argv);

/* FUNCTIONS ****************************************************************/

/* The wallpaper host keeps the desktop background applied while setup runs;
 * winlogon starts setup.exe (setup -mini / -target) before userinit and
 * Explorer, so this process is the Explorer-less environment that owns the
 * host's lifecycle. A missing or failing host must never block the install.
 * The window class string mirrors base/applications/wallpaperhost/wallpaperhost.cpp. */
static HANDLE g_WallpaperHostProcess;

static VOID
StartWallpaperHost(VOID)
{
    WCHAR Path[MAX_PATH];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;

    if (!GetSystemDirectoryW(Path, ARRAYSIZE(Path)))
        return;
    StringCchCatW(Path, ARRAYSIZE(Path), L"\\wallpaperhost.exe");

    if (!CreateProcessW(Path, NULL, NULL, NULL, FALSE, 0, NULL, NULL,
                        &StartupInfo, &ProcessInfo))
    {
        DPRINT1("WallpaperHost: failed to start (%lu); continuing without it\n",
               GetLastError());
        return;
    }

    CloseHandle(ProcessInfo.hThread);
    g_WallpaperHostProcess = ProcessInfo.hProcess;
}

static VOID
StopWallpaperHost(VOID)
{
    HWND HostWindow;

    if (!g_WallpaperHostProcess)
        return;

    HostWindow = FindWindowW(L"OrosiWallpaperHostWindow", NULL);
    if (HostWindow)
    {
        DWORD WindowProcessId;

        /* Only signal a window that belongs to our own host process. */
        GetWindowThreadProcessId(HostWindow, &WindowProcessId);
        if (WindowProcessId == GetProcessId(g_WallpaperHostProcess))
            PostMessageW(HostWindow, WM_CLOSE, 0, 0);
    }

    if (WaitForSingleObject(g_WallpaperHostProcess, 5000) == WAIT_TIMEOUT)
        TerminateProcess(g_WallpaperHostProcess, 0);

    CloseHandle(g_WallpaperHostProcess);
    g_WallpaperHostProcess = NULL;
}

static
INT
RunInstallReactOS(INT argc, WCHAR* argv[])
{
    INT RetVal;
    HMODULE hDll;
    PINSTALL_REACTOS InstallReactOS;

    hDll = LoadLibraryW(L"syssetup.dll");
    if (hDll == NULL)
    {
        DPRINT("Failed to load 'syssetup.dll'!\n");
        return GetLastError();
    }
    DPRINT("Loaded 'syssetup.dll'!\n");

    /* Call the standard Windows-compatible export */
    InstallReactOS = (PINSTALL_REACTOS)GetProcAddress(hDll, "InstallWindowsNt");
    if (InstallReactOS == NULL)
    {
        RetVal = GetLastError();
        DPRINT("Failed to get address for 'InstallWindowsNt()'!\n");
    }
    else
    {
        RetVal = InstallReactOS(argc, argv);
    }

    return RetVal;
}


/* Called from wmainCRTStartup */
INT wmain(INT argc, WCHAR* argv[])
{
    LPWSTR Switch;

    /* Read the setup switch from argv instead of scanning the raw command line.
     * The second stage is launched as "<sysdir>\oobe\setup.exe" -target (see
     * base/setup/libernt/libernt.c, RunTargetConfiguration), so the command
     * line begins with a quoted full path. A bare wcschr(CmdLine, L'-') would
     * match the first '-' inside that path when the install directory contains
     * one, making -target parse as an unknown switch; setup would then skip the
     * install and the wallpaper host would never start or stop. argv[0] is the
     * program name, so the switch is argv[1]. */
    if (argc < 2)
        return ERROR_INVALID_PARAMETER;

    Switch = argv[1];
    if (*Switch == L'-' || *Switch == L'/')
        Switch++;
    DPRINT("Setup switch: <%S>\n", Switch);

    if (_wcsicmp(Switch, L"target") == 0 || _wcsicmp(Switch, L"mini") == 0)
    {
        INT RetVal;

        StartWallpaperHost();
        RetVal = RunInstallReactOS(argc, argv);
        StopWallpaperHost();
        return RetVal;
    }

#if 0
    /* Add new setup types here */
    else if (...)
    {

    }
#endif

    return 0;
}

/* EOF */

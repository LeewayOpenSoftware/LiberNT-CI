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
        DPRINT("WallpaperHost: failed to start (%lu); continuing without it\n",
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
        PostMessageW(HostWindow, WM_CLOSE, 0, 0);

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
    LPWSTR CmdLine, p;

    // NOTE: Temporary, until we correctly use argc/argv.
    CmdLine = GetCommandLineW();
    DPRINT("CmdLine: <%S>\n", CmdLine);

    p = wcschr(CmdLine, L'-');
    if (p == NULL)
        return ERROR_INVALID_PARAMETER;
    p++;

    if (_wcsicmp(p, L"target") == 0 || _wcsicmp(p, L"mini") == 0)
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

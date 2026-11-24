/*
 * PROJECT:     Orosi Wallpaper Host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Thin host for the shell32 ActiveDesktop wallpaper object.
 *              Direct-launch activation: no Explorer, service or COM registration
 *              of its own is required. Initializes COM, creates CLSID_ActiveDesktop
 *              in-proc, applies the configured wallpaper through IActiveDesktop,
 *              and stays alive until the session ends (or it is told to stop).
 *              Intended for setup, recovery and other Explorer-less environments,
 *              which control its lifecycle by launching and stopping it.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <ole2.h>
#include <wininet.h>
#include <shlobj.h>
#include <stdarg.h>
#include <wchar.h>

#include "wallpaperhost.h"

#ifndef WPH_FAULT_INJECTION
static const WCHAR g_szWindowClass[] = L"OrosiWallpaperHostWindow";
#endif
static const WCHAR g_szSingleInstanceMutex[] = L"Local\\OrosiWallpaperHost";

static void
Log(LPCWSTR Format, ...)
{
    WCHAR Buffer[512];
    va_list Args;

    va_start(Args, Format);
    _vsnwprintf(Buffer, ARRAYSIZE(Buffer), Format, Args);
    Buffer[ARRAYSIZE(Buffer) - 1] = UNICODE_NULL;
    va_end(Args);
    OutputDebugStringW(Buffer);
}

/* Exit codes: 0 clean exit (session end, stop request, or another instance
 * already running); 1 COM init failed; 2 ActiveDesktop activation failed;
 * 3 applying the wallpaper failed; 4 the session window could not be
 * registered or created; 5 the single-instance mutex failed. */
#define EXIT_COM_INIT     1
#define EXIT_ACTIVATE     2
#define EXIT_APPLY        3
#define EXIT_WINDOW       4
#define EXIT_MUTEX        5

#ifndef WPH_FAULT_INJECTION
static LRESULT CALLBACK
SessionWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CLOSE:
            /* A launcher stops the host by posting WM_CLOSE to this window. */
            Log(L"[Orosi.WallpaperHost] stop requested (WM_CLOSE)\n");
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wParam)
            {
                Log(L"[Orosi.WallpaperHost] session ending\n");
                PostQuitMessage(0);
            }
            return 0;

        default:
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
}
#endif /* !WPH_FAULT_INJECTION */

/*
 * Each fallible step of the host runs through a seam helper. In the production
 * build the helper is simply the real call. Under WPH_FAULT_INJECTION (the test
 * build only) it first honours g_WphInject, so every exit code can be reached
 * deterministically without real COM, a real desktop or a real session end.
 */

static HANDLE
WphAcquireSingleInstance(BOOL *pbAlreadyRunning)
{
    HANDLE hMutex;

    *pbAlreadyRunning = FALSE;

#ifdef WPH_FAULT_INJECTION
    if (g_WphInject.bMutexNull)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
#endif

    hMutex = CreateMutexW(NULL, FALSE, g_szSingleInstanceMutex);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS)
        *pbAlreadyRunning = TRUE;

#ifdef WPH_FAULT_INJECTION
    if (g_WphInject.bMutexAlready)
        *pbAlreadyRunning = TRUE;
#endif

    return hMutex;
}

static HRESULT
WphComInitialize(void)
{
#ifdef WPH_FAULT_INJECTION
    return g_WphInject.hrComInit;   /* S_OK means "succeeded"; COM is not touched */
#else
    return CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
#endif
}

static void
WphComUninitialize(void)
{
#ifndef WPH_FAULT_INJECTION
    CoUninitialize();
#endif
}

static HRESULT
WphActivate(IActiveDesktop **ppActiveDesktop)
{
#ifdef WPH_FAULT_INJECTION
    if (FAILED(g_WphInject.hrActivate))
    {
        *ppActiveDesktop = NULL;
        return g_WphInject.hrActivate;
    }
    /* Sentinel: never dereferenced, because WphApply/WphRelease are stubbed too. */
    *ppActiveDesktop = reinterpret_cast<IActiveDesktop *>(static_cast<ULONG_PTR>(1));
    return g_WphInject.hrActivate;
#else
    return CoCreateInstance(CLSID_ActiveDesktop, NULL, CLSCTX_INPROC_SERVER,
                            IID_IActiveDesktop, reinterpret_cast<void **>(ppActiveDesktop));
#endif
}

static HRESULT
WphApply(IActiveDesktop *pActiveDesktop)
{
#ifdef WPH_FAULT_INJECTION
    UNREFERENCED_PARAMETER(pActiveDesktop);
    return g_WphInject.hrApply;
#else
    return pActiveDesktop->ApplyChanges(AD_APPLY_ALL);
#endif
}

static void
WphRelease(IActiveDesktop *pActiveDesktop)
{
#ifdef WPH_FAULT_INJECTION
    UNREFERENCED_PARAMETER(pActiveDesktop);
#else
    pActiveDesktop->Release();
#endif
}

static HWND
WphCreateSessionWindow(HINSTANCE hInstance)
{
#ifdef WPH_FAULT_INJECTION
    UNREFERENCED_PARAMETER(hInstance);
    if (g_WphInject.bWindowFail)
        return NULL;
    /* Sentinel: the test does not pump messages, so the window is never used. */
    return reinterpret_cast<HWND>(static_cast<ULONG_PTR>(1));
#else
    WNDCLASSW WndClass;
    HWND hWnd;

    ZeroMemory(&WndClass, sizeof(WndClass));
    WndClass.lpfnWndProc = SessionWindowProc;
    WndClass.hInstance = hInstance;
    WndClass.lpszClassName = g_szWindowClass;
    if (!RegisterClassW(&WndClass))
    {
        Log(L"[Orosi.WallpaperHost] RegisterClassW failed: %lu\n", GetLastError());
        return NULL;
    }

    hWnd = CreateWindowExW(0, g_szWindowClass, g_szWindowClass, 0,
                           0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hWnd)
        Log(L"[Orosi.WallpaperHost] CreateWindowExW failed: %lu\n", GetLastError());
    return hWnd;
#endif
}

static void
WphRunSessionLoop(void)
{
#ifdef WPH_FAULT_INJECTION
    /* The test does not run a message pump; return as if the session ended. */
    return;
#else
    MSG Msg;

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
#endif
}

/* Host core. wWinMain is a thin wrapper; the fault test drives this directly. */
extern "C" int
WallpaperHostRun(HINSTANCE hInstance, PWSTR pCmdLine)
{
    IActiveDesktop *pActiveDesktop = NULL;
    HANDLE hSingleInstance;
    BOOL bAlreadyRunning;
    HWND hWnd;
    HRESULT hr;

    Log(L"[Orosi.WallpaperHost] starting (%ls)\n", pCmdLine ? pCmdLine : L"");

    hSingleInstance = WphAcquireSingleInstance(&bAlreadyRunning);
    if (!hSingleInstance)
    {
        Log(L"[Orosi.WallpaperHost] CreateMutexW failed: %lu\n", GetLastError());
        return EXIT_MUTEX;
    }
    if (bAlreadyRunning)
    {
        Log(L"[Orosi.WallpaperHost] another instance is already running; exiting\n");
        CloseHandle(hSingleInstance);
        return 0;
    }

    hr = WphComInitialize();
    if (FAILED(hr))
    {
        Log(L"[Orosi.WallpaperHost] CoInitializeEx failed: 0x%08lx\n", hr);
        CloseHandle(hSingleInstance);
        return EXIT_COM_INIT;
    }

    /* The wallpaper contracts live in shell32's CLSID_ActiveDesktop; hosting that
     * object in this process is the entire purpose of the executable. */
    hr = WphActivate(&pActiveDesktop);
    if (FAILED(hr))
    {
        Log(L"[Orosi.WallpaperHost] CoCreateInstance(CLSID_ActiveDesktop) failed: 0x%08lx\n", hr);
        WphComUninitialize();
        CloseHandle(hSingleInstance);
        return EXIT_ACTIVATE;
    }
    Log(L"[Orosi.WallpaperHost] ActiveDesktop object activated\n");

    /* ApplyChanges(AD_APPLY_ALL) persists pending options and refreshes the
     * wallpaper on the user32 SPI path; with no prior SetWallpaper call it
     * applies the wallpaper configured in the registry. */
    hr = WphApply(pActiveDesktop);
    if (hr == S_FALSE)
    {
        /* Nothing was configured; this is not an error. */
        Log(L"[Orosi.WallpaperHost] nothing to apply (no wallpaper configured)\n");
    }
    else if (FAILED(hr))
    {
        Log(L"[Orosi.WallpaperHost] ApplyChanges failed: 0x%08lx\n", hr);
        WphRelease(pActiveDesktop);
        WphComUninitialize();
        CloseHandle(hSingleInstance);
        return EXIT_APPLY;
    }
    else
    {
        Log(L"[Orosi.WallpaperHost] wallpaper applied\n");
    }

    /* Hidden top-level window (HWND_MESSAGE would never see the end of the
     * session): receives WM_QUERYENDSESSION/WM_ENDSESSION at session end and
     * WM_CLOSE from a launcher, and advertises the host with a fixed class.
     * Without it the host could never be told to stop. */
    hWnd = WphCreateSessionWindow(hInstance);
    if (!hWnd)
    {
        WphRelease(pActiveDesktop);
        WphComUninitialize();
        CloseHandle(hSingleInstance);
        return EXIT_WINDOW;
    }

    Log(L"[Orosi.WallpaperHost] entering session loop\n");
    WphRunSessionLoop();

    WphRelease(pActiveDesktop);
    WphComUninitialize();
    CloseHandle(hSingleInstance);
    Log(L"[Orosi.WallpaperHost] exiting\n");
    return 0;
}

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    return WallpaperHostRun(hInstance, pCmdLine);
}

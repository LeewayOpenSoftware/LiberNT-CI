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

static const WCHAR g_szWindowClass[] = L"OrosiWallpaperHostWindow";
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

/* Exit codes: 0 clean exit (session end or stop request, or another instance
 * already running); 1 COM init failed; 2 ActiveDesktop activation failed;
 * 3 applying the wallpaper failed. */
#define EXIT_COM_INIT     1
#define EXIT_ACTIVATE     2
#define EXIT_APPLY        3

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

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    IActiveDesktop *pActiveDesktop = NULL;
    HANDLE hSingleInstance;
    WNDCLASSW WndClass = { 0 };
    HWND hWnd;
    HRESULT hr;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    Log(L"[Orosi.WallpaperHost] starting (%ls)\n", pCmdLine);

    hSingleInstance = CreateMutexW(NULL, FALSE, g_szSingleInstanceMutex);
    if (!hSingleInstance)
    {
        Log(L"[Orosi.WallpaperHost] CreateMutexW failed: %lu\n", GetLastError());
        return EXIT_COM_INIT;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Log(L"[Orosi.WallpaperHost] another instance is already running; exiting\n");
        CloseHandle(hSingleInstance);
        return 0;
    }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        Log(L"[Orosi.WallpaperHost] CoInitializeEx failed: 0x%08lx\n", hr);
        CloseHandle(hSingleInstance);
        return EXIT_COM_INIT;
    }

    /* The wallpaper contracts live in shell32's CLSID_ActiveDesktop; hosting that
     * object in this process is the entire purpose of the executable. */
    hr = CoCreateInstance(CLSID_ActiveDesktop, NULL, CLSCTX_INPROC_SERVER,
                          IID_IActiveDesktop, reinterpret_cast<void **>(&pActiveDesktop));
    if (FAILED(hr))
    {
        Log(L"[Orosi.WallpaperHost] CoCreateInstance(CLSID_ActiveDesktop) failed: 0x%08lx\n", hr);
        CoUninitialize();
        CloseHandle(hSingleInstance);
        return EXIT_ACTIVATE;
    }
    Log(L"[Orosi.WallpaperHost] ActiveDesktop object activated\n");

    /* ApplyChanges(AD_APPLY_ALL) persists pending options and refreshes the
     * wallpaper on the user32 SPI path; with no prior SetWallpaper call it
     * applies the wallpaper configured in the registry. */
    hr = pActiveDesktop->ApplyChanges(AD_APPLY_ALL);
    if (FAILED(hr))
    {
        Log(L"[Orosi.WallpaperHost] ApplyChanges failed: 0x%08lx\n", hr);
        pActiveDesktop->Release();
        CoUninitialize();
        CloseHandle(hSingleInstance);
        return EXIT_APPLY;
    }
    Log(L"[Orosi.WallpaperHost] wallpaper applied\n");

    /* Hidden top-level window (HWND_MESSAGE would never see the end of the
     * session): receives WM_QUERYENDSESSION/WM_ENDSESSION at session end and
     * WM_CLOSE from a launcher, and advertises the host with a fixed class. */
    WndClass.lpfnWndProc = SessionWindowProc;
    WndClass.hInstance = hInstance;
    WndClass.lpszClassName = g_szWindowClass;
    if (!RegisterClassW(&WndClass))
    {
        Log(L"[Orosi.WallpaperHost] RegisterClassW failed: %lu\n", GetLastError());
    }
    else
    {
        hWnd = CreateWindowExW(0, g_szWindowClass, g_szWindowClass, 0,
                               0, 0, 0, 0, NULL, NULL, hInstance, NULL);
        if (!hWnd)
            Log(L"[Orosi.WallpaperHost] CreateWindowExW failed: %lu\n", GetLastError());
    }

    Log(L"[Orosi.WallpaperHost] entering session loop\n");
    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    pActiveDesktop->Release();
    CoUninitialize();
    CloseHandle(hSingleInstance);
    Log(L"[Orosi.WallpaperHost] exiting\n");
    return 0;
}

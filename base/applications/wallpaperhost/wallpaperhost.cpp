/*
 * PROJECT:     Orosi Wallpaper Host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Thin host for the shell32 ActiveDesktop wallpaper object.
 *              Direct-launch activation: no Explorer, service or COM registration
 *              of its own is required. Initializes COM, creates CLSID_ActiveDesktop
 *              in-proc, applies the configured wallpaper through IActiveDesktop,
 *              and stays alive until the session ends. Intended for setup,
 *              recovery and other Explorer-less environments.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <ole2.h>
#include <wininet.h>
#include <shlobj.h>
#include <stdarg.h>
#include <wchar.h>

static const WCHAR g_szMsgWindowClass[] = L"OrosiWallpaperHostMsgWindow";

/* Public, documented IActiveDesktop apply flags (see CActiveDesktop.cpp). */
#ifndef AD_APPLY_ALL
#define AD_APPLY_SAVE 0x00000001
#define AD_APPLY_ALL  0x00000002
#endif

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

/* Exit codes: 0 clean session end; 1 COM init failed; 2 ActiveDesktop activation
 * failed; 3 applying the wallpaper failed. */
#define EXIT_COM_INIT     1
#define EXIT_ACTIVATE     2
#define EXIT_APPLY        3

static LRESULT CALLBACK
SessionWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wParam)
                PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
}

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    IActiveDesktop *pActiveDesktop = NULL;
    WNDCLASSW WndClass = { 0 };
    HWND hWnd;
    HRESULT hr;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    Log(L"[Orosi.WallpaperHost] starting (%ls)\n", pCmdLine);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        Log(L"[Orosi.WallpaperHost] CoInitializeEx failed: 0x%08lx\n", hr);
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
        return EXIT_ACTIVATE;
    }
    Log(L"[Orosi.WallpaperHost] ActiveDesktop object activated\n");

    /* ApplyChanges commits the pending state on the user32 SPI path; with no
     * prior SetWallpaper call it applies the wallpaper configured in the registry. */
    hr = pActiveDesktop->ApplyChanges(AD_APPLY_ALL);
    if (FAILED(hr))
    {
        Log(L"[Orosi.WallpaperHost] ApplyChanges failed: 0x%08lx\n", hr);
        pActiveDesktop->Release();
        CoUninitialize();
        return EXIT_APPLY;
    }
    Log(L"[Orosi.WallpaperHost] wallpaper applied\n");

    /* Hidden message sink so the session manager can end the process cleanly. */
    WndClass.lpfnWndProc = SessionWindowProc;
    WndClass.hInstance = hInstance;
    WndClass.lpszClassName = g_szMsgWindowClass;
    RegisterClassW(&WndClass);
    hWnd = CreateWindowW(g_szMsgWindowClass, g_szMsgWindowClass, 0,
                         0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (!hWnd)
        Log(L"[Orosi.WallpaperHost] message window unavailable: %lu\n", GetLastError());

    Log(L"[Orosi.WallpaperHost] entering session loop\n");
    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    if (hWnd)
        DestroyWindow(hWnd);
    pActiveDesktop->Release();
    CoUninitialize();
    Log(L"[Orosi.WallpaperHost] exiting\n");
    return 0;
}

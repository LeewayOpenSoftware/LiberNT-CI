/*
 * PROJECT:     LiberNT Photo Viewer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Win7 Photo Viewer standalone harness using real shimgvw.c
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Harness exe that calls ImageView_Main from shimgvw.c
 * Usage: photoviewer.exe [image]
 */

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

/* Forward declarations from shimgvw */
extern VOID ImageView_Main(HWND hwnd, LPCWSTR path);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"PhotoViewerHarness";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Windows Photo Viewer",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760,
                                NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
    {
        ImageView_Main(hwnd, argv[1]);
    }
    LocalFree(argv);

    ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

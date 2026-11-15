/*
 * PROJECT:     LiberNT Consent UI
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Asks the user whether an administrative program may run
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Started by the secondary logon service as SYSTEM in the user's session, on the
 * user's desktop. Command line: consent.exe <owner hwnd in hex> "<final path>" <command line>
 * The exit code is IDYES or IDNO.
 */

#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <wchar.h>
#include <stdlib.h>
#include "resource.h"

#ifndef OIC_SHIELD
#define OIC_SHIELD 32518
#endif

typedef struct _CONSENT_ARGS
{
    HWND hwndOwner;
    PCWSTR ImagePath;
    PCWSTR CommandLine;
} CONSENT_ARGS, *PCONSENT_ARGS;

static
INT_PTR
CALLBACK
ConsentDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            PCONSENT_ARGS Args = (PCONSENT_ARGS)lParam;
            HICON hIcon = LoadImageW(NULL, MAKEINTRESOURCEW(OIC_SHIELD), IMAGE_ICON, 32, 32, LR_SHARED);

            if (hIcon)
                SendDlgItemMessageW(hwnd, IDC_SHIELD, STM_SETICON, (WPARAM)hIcon, 0);
            SetDlgItemTextW(hwnd, IDC_IMAGE, Args->ImagePath);
            SetDlgItemTextW(hwnd, IDC_COMMAND, Args->CommandLine);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDYES || LOWORD(wParam) == IDNO || LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hwnd, LOWORD(wParam) == IDYES ? IDYES : IDNO);
                return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(hwnd, IDNO);
            return TRUE;
    }
    return FALSE;
}

/* Skips one argument (quoted or bare) and the blanks after it */
static
PWSTR
SkipArgument(PWSTR p)
{
    if (*p == L'"')
    {
        p++;
        while (*p && *p != L'"')
            p++;
        if (*p == L'"')
            p++;
    }
    else
    {
        while (*p && *p != L' ' && *p != L'\t')
            p++;
    }
    while (*p == L' ' || *p == L'\t')
        p++;
    return p;
}

int
WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
    CONSENT_ARGS Args = { NULL, L"", L"" };
    PWSTR p = GetCommandLineW(), q;
    INT_PTR Answer;

    /* consent.exe <hwnd> "<path>" <command line> */
    p = SkipArgument(p);
    if (*p == UNICODE_NULL)
        return IDNO;
    Args.hwndOwner = (HWND)(ULONG_PTR)wcstoull(p, &q, 16);
    p = SkipArgument(p);
    if (*p != L'"')
        return IDNO;
    Args.ImagePath = p + 1;
    q = wcschr(p + 1, L'"');
    if (q == NULL)
        return IDNO;
    *q = UNICODE_NULL;
    p = q + 1;
    while (*p == L' ' || *p == L'\t')
        p++;
    Args.CommandLine = p;

    if (Args.hwndOwner != NULL && !IsWindow(Args.hwndOwner))
        Args.hwndOwner = NULL;

    Answer = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_CONSENT), Args.hwndOwner, ConsentDlgProc, (LPARAM)&Args);
    return (Answer == IDYES) ? IDYES : IDNO;
}

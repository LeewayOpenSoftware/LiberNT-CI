/*
 * PROJECT:     LiberNT Consent UI
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Asks the user whether an administrative program may run
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Started by the secondary logon service as SYSTEM in the user's session, on the
 * user's desktop. Command line: consent.exe <caller hwnd in hex> "<final path>" <command line>
 * The exit code is IDYES or IDNO.
 */

#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <wchar.h>
#include <stdlib.h>
#include <strsafe.h>
#include "resource.h"

/* Longer command lines are cut for display; the length is shown so a cut is visible */
#define CONSENT_MAX_COMMAND 160

#ifndef OIC_SHIELD
#define OIC_SHIELD 32518
#endif

typedef struct _CONSENT_ARGS
{
    HWND hwndCaller;
    PCWSTR ImagePath;
    PCWSTR CommandLine;
} CONSENT_ARGS, *PCONSENT_ARGS;

/* One line, single blanks, no control characters, cut with its length when long */
static
void
FormatCommandLine(PCWSTR CommandLine, PWSTR Display, size_t DisplayChars)
{
    size_t Length = 0, Total = wcslen(CommandLine), Pos;
    BOOL Blank = TRUE;

    for (Pos = 0; Pos < Total && Length + 1 < DisplayChars; Pos++)
    {
        WCHAR c = CommandLine[Pos];

        if (c <= L' ' || c == 0x7F)
        {
            if (!Blank)
                Display[Length++] = L' ';
            Blank = TRUE;
            continue;
        }
        Display[Length++] = c;
        Blank = FALSE;
        if (Length >= CONSENT_MAX_COMMAND)
            break;
    }
    while (Length > 0 && Display[Length - 1] == L' ')
        Length--;
    Display[Length] = UNICODE_NULL;
    if (Pos < Total)
    {
        StringCchCatW(Display, DisplayChars, L"\x2026");
        StringCchPrintfW(Display + wcslen(Display), DisplayChars - wcslen(Display),
                         L" (%Iu characters)", Total);
    }
}

/* The caller's window only says where the prompt goes; it never owns the prompt */
static
void
PlaceNearCaller(HWND hwnd, HWND hwndCaller)
{
    RECT Caller, Self, Work;
    int x, y;

    if (hwndCaller == NULL || !IsWindow(hwndCaller) || !GetWindowRect(hwndCaller, &Caller) ||
        !GetWindowRect(hwnd, &Self) || !SystemParametersInfoW(SPI_GETWORKAREA, 0, &Work, 0))
    {
        return;
    }
    x = (Caller.left + Caller.right - (Self.right - Self.left)) / 2;
    y = (Caller.top + Caller.bottom - (Self.bottom - Self.top)) / 2;
    x = max(Work.left, min(x, Work.right - (Self.right - Self.left)));
    y = max(Work.top, min(y, Work.bottom - (Self.bottom - Self.top)));
    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

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
            WCHAR Display[CONSENT_MAX_COMMAND + 48];

            if (hIcon)
                SendDlgItemMessageW(hwnd, IDC_SHIELD, STM_SETICON, (WPARAM)hIcon, 0);
            SetDlgItemTextW(hwnd, IDC_IMAGE, Args->ImagePath);
            FormatCommandLine(Args->CommandLine, Display, _countof(Display));
            SetDlgItemTextW(hwnd, IDC_COMMAND, Display);
            PlaceNearCaller(hwnd, Args->hwndCaller);
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

    /* consent.exe <caller hwnd> "<path>" <command line> */
    p = SkipArgument(p);
    if (*p == UNICODE_NULL)
        return IDNO;
    Args.hwndCaller = (HWND)(ULONG_PTR)wcstoull(p, &q, 16);
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

    if (Args.hwndCaller != NULL && !IsWindow(Args.hwndCaller))
        Args.hwndCaller = NULL;

    /* No owner: an owned dialog can be reached through its owner's thread, this one cannot */
    Answer = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_CONSENT), NULL, ConsentDlgProc, (LPARAM)&Args);
    return (Answer == IDYES) ? IDYES : IDNO;
}

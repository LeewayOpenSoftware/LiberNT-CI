/*
 * PROJECT:     LiberNT lab diagnostics
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Lab-only trace of the font and text calls of one process
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

typedef ULONG (__cdecl *PLNT_DBGPRINT)(PCSTR Format, ...);

static LONG LntFontDiagState;
static PLNT_DBGPRINT LntFontDiagPrint;

static __inline
WCHAR
LntFontDiagFold(
    _In_ WCHAR Char)
{
    return (Char >= L'a' && Char <= L'z') ? (WCHAR)(Char - (L'a' - L'A')) : Char;
}

static __inline
BOOLEAN
LntFontDiagMatch(
    _In_ PCWSTR Image,
    _In_ PCWSTR Want)
{
    SIZE_T i, j;

    for (i = 0; Image[i] != 0; i++)
    {
        for (j = 0; Want[j] != 0 && Image[i + j] != 0; j++)
        {
            if (LntFontDiagFold(Image[i + j]) != LntFontDiagFold(Want[j]))
                break;
        }
        if (Want[j] == 0)
            return TRUE;
    }
    return FALSE;
}

static __inline
BOOLEAN
LntFontDiagOn(VOID)
{
    WCHAR Want[64], Image[MAX_PATH];
    LONG State = LntFontDiagState;
    DWORD Length;

    if (State != 0)
        return State == 1;

    State = 2;
    Length = GetEnvironmentVariableW(L"LNT_FONTDIAG", Want, ARRAYSIZE(Want));
    if (Length != 0 && Length < ARRAYSIZE(Want) &&
        GetModuleFileNameW(NULL, Image, ARRAYSIZE(Image)) != 0)
    {
        LntFontDiagPrint = (PLNT_DBGPRINT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "DbgPrint");
        if (LntFontDiagPrint != NULL && (Want[0] == L'*' || LntFontDiagMatch(Image, Want)))
            State = 1;
    }

    LntFontDiagState = State;
    return State == 1;
}

static __inline
VOID
LntFontDiagDescribeFont(
    _In_ HDC hdc,
    _Out_ PLOGFONTW Font)
{
    HGDIOBJ Object;

    ZeroMemory(Font, sizeof(*Font));
    Object = GetCurrentObject(hdc, OBJ_FONT);
    if (Object != NULL)
        GetObjectW(Object, sizeof(*Font), Font);
}

#define LNT_FONTDIAG_TEXT(Count) ((Count) < 0 ? 64 : ((Count) > 64 ? 64 : (Count)))

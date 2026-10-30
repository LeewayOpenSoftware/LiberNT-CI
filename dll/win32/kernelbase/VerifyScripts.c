/*
 * PROJECT:     LiberNT KernelBase
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     VerifyScripts: compares two lists of four letter script names
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <winternl.h>

#define SCRIPT_NAME_LENGTH 4
#define SCRIPT_ENTRY_LENGTH (SCRIPT_NAME_LENGTH + 1)

/* Length of a list, or -1 when its size or shape is not valid */
static
int
ScriptListLength(
    _In_ LPCWSTR List,
    _In_ int Count)
{
    int Length;

    if (Count == 0)
        return -1;

    if (Count < 0)
    {
        if (Count != -1)
            return -1;
        Length = (int)wcslen(List);
    }
    else
    {
        /* A count that includes the terminating null is accepted */
        Length = 0;
        while (Length < Count && List[Length])
            Length++;
    }

    /* Every name is four characters followed by a semicolon */
    if (Length % SCRIPT_ENTRY_LENGTH)
        return -1;

    return Length;
}

static
BOOL
ListContains(
    _In_ LPCWSTR List,
    _In_ int Length,
    _In_ LPCWSTR Name)
{
    int Offset;

    for (Offset = 0; Offset < Length; Offset += SCRIPT_ENTRY_LENGTH)
    {
        if (List[Offset + SCRIPT_NAME_LENGTH] == L';' &&
            _wcsnicmp(List + Offset, Name, SCRIPT_NAME_LENGTH) == 0)
            return TRUE;
    }

    return FALSE;
}

BOOL
WINAPI
VerifyScripts(
    _In_ DWORD Flags,
    _In_ LPCWSTR LocaleScripts,
    _In_ int LocaleScriptsLength,
    _In_ LPCWSTR TestScripts,
    _In_ int TestScriptsLength)
{
    int LocaleLength, TestLength, Offset;

    if (Flags & ~VS_ALLOW_LATIN)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }

    if (!LocaleScripts || !TestScripts)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    LocaleLength = ScriptListLength(LocaleScripts, LocaleScriptsLength);
    TestLength = ScriptListLength(TestScripts, TestScriptsLength);
    if (LocaleLength < 0 || TestLength < 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (Offset = 0; Offset < TestLength; Offset += SCRIPT_ENTRY_LENGTH)
    {
        if (TestScripts[Offset + SCRIPT_NAME_LENGTH] != L';')
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        if ((Flags & VS_ALLOW_LATIN) && _wcsnicmp(TestScripts + Offset, L"Latn", SCRIPT_NAME_LENGTH) == 0)
            continue;

        if (!ListContains(LocaleScripts, LocaleLength, TestScripts + Offset))
        {
            /* A valid answer, not an error */
            SetLastError(ERROR_SUCCESS);
            return FALSE;
        }
    }

    return TRUE;
}

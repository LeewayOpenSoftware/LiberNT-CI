/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/environ.c
 * PURPOSE:         Environment functions
 * PROGRAMMER:      Ariadne ( ariadne@xs4all.nl)
 *                  Emanuele Aliberti
 *                  Thomas Weidenmueller
 * UPDATE HISTORY:
 *                  Created 01/11/98
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
SetEnvironmentStringsA(IN LPCH NewEnvironment)
{
    LPCH Current;
    LPWSTR NewEnvironmentW;
    INT Count;
    BOOL Result;

    for (Current = NewEnvironment; *Current; Current += strlen(Current) + 1);

    Count = MultiByteToWideChar(CP_ACP, 0, NewEnvironment, (INT)(Current - NewEnvironment) + 1, NULL, 0);
    NewEnvironmentW = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count * sizeof(WCHAR));
    if (!NewEnvironmentW)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    MultiByteToWideChar(CP_ACP, 0, NewEnvironment, (INT)(Current - NewEnvironment) + 1, NewEnvironmentW, Count);
    Result = SetEnvironmentStringsW(NewEnvironmentW);
    RtlFreeHeap(RtlGetProcessHeap(), 0, NewEnvironmentW);
    return Result;
}

/* EOF */

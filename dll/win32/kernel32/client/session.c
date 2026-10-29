/*
 * PROJECT:         ReactOS Win32 Base API
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            dll/win32/kernel32/client/session.c
 * PURPOSE:         Session Support APIs
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/*
 * @unimplemented
 */
DWORD
WINAPI
DosPathToSessionPathW(IN DWORD SessionID,
                      IN LPWSTR InPath,
                      OUT LPWSTR *OutPath)
{
    /* The call has no public contract to implement: fail explicitly and hand
       back no path instead of reporting an unexplained zero */
    UNIMPLEMENTED;
    if (OutPath)
        *OutPath = NULL;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

/*
 * @unimplemented
 */
DWORD
WINAPI
DosPathToSessionPathA(IN DWORD SessionId,
                      IN LPSTR InPath,
                      OUT LPSTR *OutPath)
{
    UNIMPLEMENTED;
    if (OutPath)
        *OutPath = NULL;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

/*
 * @implemented
 */
DWORD
WINAPI
WTSGetActiveConsoleSessionId(VOID)
{
    return SharedUserData->ActiveConsoleId;
}

/* EOF */

/*
 * PROJECT:     ReactOS Services
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        base/applications/sc/sidtype.c
 * PURPOSE:     Query the service SID type
 */

#include "sc.h"

BOOL QuerySidType(LPCTSTR ServiceName)
{
    SC_HANDLE hManager = NULL;
    SC_HANDLE hService = NULL;
    BOOL bResult = TRUE;
    DWORD cbBytesNeeded = 0;
    SERVICE_SID_INFO SidInfo;
    LPCTSTR SidType;

#ifdef SCDBG
    _tprintf(_T("service to show sid type - %s\n\n"), ServiceName);
#endif

    hManager = OpenSCManager(NULL,
                             NULL,
                             SC_MANAGER_CONNECT);
    if (hManager == NULL)
    {
        _tprintf(_T("[SC] OpenSCManager FAILED %lu:\n\n"), GetLastError());
        bResult = FALSE;
        goto done;
    }

    hService = OpenService(hManager, ServiceName, SERVICE_QUERY_CONFIG);
    if (hService == NULL)
    {
        _tprintf(_T("[SC] OpenService FAILED %lu:\n\n"), GetLastError());
        bResult = FALSE;
        goto done;
    }

    if (!QueryServiceConfig2(hService,
                             SERVICE_CONFIG_SERVICE_SID_INFO,
                             (LPBYTE)&SidInfo,
                             sizeof(SidInfo),
                             &cbBytesNeeded))
    {
        _tprintf(_T("[SC] QueryServiceConfig2 FAILED %lu:\n\n"), GetLastError());
        bResult = FALSE;
        goto done;
    }

    _tprintf(_T("[SC] QueryServiceConfig2 SUCCESS\n\n"));

    switch (SidInfo.dwServiceSidType)
    {
        case SERVICE_SID_TYPE_NONE:
            SidType = _T("NONE");
            break;
        case SERVICE_SID_TYPE_UNRESTRICTED:
            SidType = _T("UNRESTRICTED");
            break;
        case SERVICE_SID_TYPE_RESTRICTED:
            SidType = _T("RESTRICTED");
            break;
        default:
            SidType = _T("UNKNOWN");
            break;
    }

    _tprintf(_T("SERVICE_NAME: %s\n"), ServiceName);
    _tprintf(_T("SERVICE_SID_TYPE:  %s\n"), SidType);

done:
    if (bResult == FALSE)
        ReportLastError();

    if (hService)
        CloseServiceHandle(hService);

    if (hManager)
        CloseServiceHandle(hManager);

    return bResult;
}

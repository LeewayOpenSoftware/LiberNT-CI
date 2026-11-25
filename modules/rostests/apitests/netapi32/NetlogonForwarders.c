/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later.html)
 * PURPOSE:     Verify Netlogon export forwarding and buffer compatibility
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <lm.h>

typedef NET_API_STATUS (WINAPI *PNetApiBufferAllocate)(DWORD, LPVOID *);
typedef NET_API_STATUS (WINAPI *PNetApiBufferFree)(LPVOID);
typedef NET_API_STATUS (WINAPI *PNetGetDCName)(LPCWSTR, LPCWSTR, LPBYTE *);

static const char * const NetlogonExports[] =
{
    "DsAddressToSiteNamesA",
    "DsAddressToSiteNamesExA",
    "DsAddressToSiteNamesExW",
    "DsAddressToSiteNamesW",
    "DsDeregisterDnsHostRecordsA",
    "DsDeregisterDnsHostRecordsW",
    "DsEnumerateDomainTrustsA",
    "DsEnumerateDomainTrustsW",
    "DsGetDcNameA",
    "DsGetDcNameW",
    "DsGetDcNameWithAccountA",
    "DsGetDcNameWithAccountW",
    "DsGetDcOpenA",
    "DsGetDcOpenW",
    "DsGetDcSiteCoverageA",
    "DsGetDcSiteCoverageW",
    "DsGetForestTrustInformationW",
    "DsGetSiteNameA",
    "DsGetSiteNameW",
    "DsMergeForestTrustInformationW",
    "DsValidateSubnetNameA",
    "DsValidateSubnetNameW",
    "NetEnumerateTrustedDomains",
    "NetGetAnyDCName",
    "NetGetDCName",
    "NetLogonGetTimeServiceParentDomain",
    "NetLogonSetServiceBits"
};

START_TEST(NetlogonForwarders)
{
    HMODULE Advapi32, Netapi32, Logoncli, Netutils;
    PNetApiBufferAllocate NetapiAllocate, NetutilsAllocate;
    PNetApiBufferFree NetapiFree, NetutilsFree;
    PNetGetDCName LogoncliGetDCName, NetapiGetDCName;
    LPVOID Buffer;
    LPBYTE DcName;
    NET_API_STATUS Status;
    FARPROC NetapiProc, LogoncliProc;
    ULONG i;

    Advapi32 = LoadLibraryW(L"advapi32.dll");
    ok(Advapi32 != NULL, "LoadLibraryW(advapi32) failed: %lu\n", GetLastError());

    Netapi32 = LoadLibraryW(L"netapi32.dll");
    ok(Netapi32 != NULL, "LoadLibraryW(netapi32) failed: %lu\n", GetLastError());
    Logoncli = LoadLibraryW(L"logoncli.dll");
    ok(Logoncli != NULL, "LoadLibraryW(logoncli) failed: %lu\n", GetLastError());
    Netutils = LoadLibraryW(L"netutils.dll");
    ok(Netutils != NULL, "LoadLibraryW(netutils) failed: %lu\n", GetLastError());
    if (!Netapi32 || !Logoncli || !Netutils)
        goto done;

    for (i = 0; i < ARRAYSIZE(NetlogonExports); ++i)
    {
        NetapiProc = GetProcAddress(Netapi32, NetlogonExports[i]);
        LogoncliProc = GetProcAddress(Logoncli, NetlogonExports[i]);
        ok(NetapiProc != NULL, "netapi32!%s is missing\n", NetlogonExports[i]);
        ok(LogoncliProc != NULL, "logoncli!%s is missing\n", NetlogonExports[i]);
        ok(NetapiProc == LogoncliProc,
           "netapi32!%s %p does not resolve to logoncli %p\n",
           NetlogonExports[i], NetapiProc, LogoncliProc);
    }

    NetapiAllocate = (PNetApiBufferAllocate)GetProcAddress(Netapi32, "NetApiBufferAllocate");
    NetapiFree = (PNetApiBufferFree)GetProcAddress(Netapi32, "NetApiBufferFree");
    NetutilsAllocate = (PNetApiBufferAllocate)GetProcAddress(Netutils, "NetApiBufferAllocate");
    NetutilsFree = (PNetApiBufferFree)GetProcAddress(Netutils, "NetApiBufferFree");
    ok(NetapiAllocate != NULL && NetapiFree != NULL,
       "netapi32 buffer API is incomplete\n");
    ok(NetutilsAllocate != NULL && NetutilsFree != NULL,
       "netutils buffer API is incomplete\n");

    if (NetapiAllocate && NetapiFree && NetutilsAllocate && NetutilsFree)
    {
        Buffer = NULL;
        Status = NetutilsAllocate(64, &Buffer);
        ok(Status == NERR_Success && Buffer != NULL,
           "netutils allocation failed: %lu, %p\n", Status, Buffer);
        if (Buffer)
            ok(NetapiFree(Buffer) == NERR_Success,
               "netapi32 could not free a netutils buffer\n");

        Buffer = NULL;
        Status = NetapiAllocate(64, &Buffer);
        ok(Status == NERR_Success && Buffer != NULL,
           "netapi32 allocation failed: %lu, %p\n", Status, Buffer);
        if (Buffer)
            ok(NetutilsFree(Buffer) == NERR_Success,
               "netutils could not free a netapi32 buffer\n");
    }

    LogoncliGetDCName = (PNetGetDCName)GetProcAddress(Logoncli, "NetGetDCName");
    NetapiGetDCName = (PNetGetDCName)GetProcAddress(Netapi32, "NetGetDCName");
    if (LogoncliGetDCName && NetapiFree)
    {
        DcName = NULL;
        Status = LogoncliGetDCName(NULL, NULL, &DcName);
        if (Status == NERR_Success)
        {
            ok(DcName != NULL, "logoncli returned success without a buffer\n");
            if (DcName)
                ok(NetapiFree(DcName) == NERR_Success,
                   "netapi32 could not free a logoncli result\n");
        }
        else
        {
            skip("No local domain controller for logoncli cross-free test: %lu\n", Status);
        }
    }

    if (NetapiGetDCName && NetutilsFree)
    {
        DcName = NULL;
        Status = NetapiGetDCName(NULL, NULL, &DcName);
        if (Status == NERR_Success)
        {
            ok(DcName != NULL, "netapi32 returned success without a buffer\n");
            if (DcName)
                ok(NetutilsFree(DcName) == NERR_Success,
                   "netutils could not free a forwarded netapi32 result\n");
        }
        else
        {
            skip("No local domain controller for netapi32 cross-free test: %lu\n", Status);
        }
    }

done:
    if (Netutils) FreeLibrary(Netutils);
    if (Logoncli) FreeLibrary(Logoncli);
    if (Netapi32) FreeLibrary(Netapi32);
    if (Advapi32) FreeLibrary(Advapi32);
}

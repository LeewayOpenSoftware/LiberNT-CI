/*
 * PROJECT:     LiberNT Logon Server Test Utility
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     nltest.exe: secure channel, DC locator, site and trust queries over
 *              the Netlogon client API exported by logoncli
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sddl.h>
#include <rpc.h>
#include <lm.h>
#include <dsgetdc.h>
#include <ntsecapi.h>
#include <strsafe.h>

#include <conutils.h>

#include "resource.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#define MAX_ADDRESSES 64

static PCWSTR g_ServerName = NULL;

/* The symbolic name printed after every status, as the Windows tool does */
static const struct
{
    DWORD Code;
    PCWSTR Name;
} StatusNames[] =
{
    { NERR_Success, L"NERR_Success" },
    { ERROR_FILE_NOT_FOUND, L"ERROR_FILE_NOT_FOUND" },
    { ERROR_PATH_NOT_FOUND, L"ERROR_PATH_NOT_FOUND" },
    { ERROR_ACCESS_DENIED, L"ERROR_ACCESS_DENIED" },
    { ERROR_NOT_ENOUGH_MEMORY, L"ERROR_NOT_ENOUGH_MEMORY" },
    { ERROR_INVALID_DATA, L"ERROR_INVALID_DATA" },
    { ERROR_NOT_SUPPORTED, L"ERROR_NOT_SUPPORTED" },
    { ERROR_BAD_NETPATH, L"ERROR_BAD_NETPATH" },
    { ERROR_NETWORK_BUSY, L"ERROR_NETWORK_BUSY" },
    { ERROR_DEV_NOT_EXIST, L"ERROR_DEV_NOT_EXIST" },
    { ERROR_BAD_NET_NAME, L"ERROR_BAD_NET_NAME" },
    { ERROR_INVALID_PARAMETER, L"ERROR_INVALID_PARAMETER" },
    { ERROR_CALL_NOT_IMPLEMENTED, L"ERROR_CALL_NOT_IMPLEMENTED" },
    { ERROR_SEM_TIMEOUT, L"ERROR_SEM_TIMEOUT" },
    { ERROR_INVALID_NAME, L"ERROR_INVALID_NAME" },
    { ERROR_INVALID_LEVEL, L"ERROR_INVALID_LEVEL" },
    { ERROR_MORE_DATA, L"ERROR_MORE_DATA" },
    { ERROR_INVALID_FLAGS, L"ERROR_INVALID_FLAGS" },
    { ERROR_NO_NETWORK, L"ERROR_NO_NETWORK" },
    { ERROR_SHUTDOWN_IN_PROGRESS, L"ERROR_SHUTDOWN_IN_PROGRESS" },
    { ERROR_NO_SHUTDOWN_IN_PROGRESS, L"ERROR_NO_SHUTDOWN_IN_PROGRESS" },
    { ERROR_SERVICE_DOES_NOT_EXIST, L"ERROR_SERVICE_DOES_NOT_EXIST" },
    { ERROR_SERVICE_NOT_ACTIVE, L"ERROR_SERVICE_NOT_ACTIVE" },
    { ERROR_NOT_FOUND, L"ERROR_NOT_FOUND" },
    { ERROR_INVALID_COMPUTERNAME, L"ERROR_INVALID_COMPUTERNAME" },
    { ERROR_INVALID_DOMAINNAME, L"ERROR_INVALID_DOMAINNAME" },
    { ERROR_NO_SUCH_DOMAIN, L"ERROR_NO_SUCH_DOMAIN" },
    { ERROR_NO_LOGON_SERVERS, L"ERROR_NO_LOGON_SERVERS" },
    { ERROR_NO_SUCH_USER, L"ERROR_NO_SUCH_USER" },
    { ERROR_LOGON_FAILURE, L"ERROR_LOGON_FAILURE" },
    { ERROR_NO_TRUST_LSA_SECRET, L"ERROR_NO_TRUST_LSA_SECRET" },
    { ERROR_NO_TRUST_SAM_ACCOUNT, L"ERROR_NO_TRUST_SAM_ACCOUNT" },
    { ERROR_TRUSTED_DOMAIN_FAILURE, L"ERROR_TRUSTED_DOMAIN_FAILURE" },
    { ERROR_TRUSTED_RELATIONSHIP_FAILURE, L"ERROR_TRUSTED_RELATIONSHIP_FAILURE" },
    { ERROR_TRUST_FAILURE, L"ERROR_TRUST_FAILURE" },
    { ERROR_NOLOGON_WORKSTATION_TRUST_ACCOUNT, L"ERROR_NOLOGON_WORKSTATION_TRUST_ACCOUNT" },
    { ERROR_NOLOGON_SERVER_TRUST_ACCOUNT, L"ERROR_NOLOGON_SERVER_TRUST_ACCOUNT" },
    { ERROR_DOMAIN_TRUST_INCONSISTENT, L"ERROR_DOMAIN_TRUST_INCONSISTENT" },
    { ERROR_TIME_SKEW, L"ERROR_TIME_SKEW" },
    { ERROR_INTERNAL_ERROR, L"ERROR_INTERNAL_ERROR" },
    { ERROR_TIMEOUT, L"ERROR_TIMEOUT" },
    { ERROR_DOMAIN_CONTROLLER_NOT_FOUND, L"ERROR_DOMAIN_CONTROLLER_NOT_FOUND" },
    { ERROR_NO_SITENAME, L"ERROR_NO_SITENAME" },
    { ERROR_DS_NOT_INSTALLED, L"ERROR_DS_NOT_INSTALLED" },
    { RPC_S_INVALID_BINDING, L"RPC_S_INVALID_BINDING" },
    { RPC_S_INVALID_NET_ADDR, L"RPC_S_INVALID_NET_ADDR" },
    { RPC_S_SERVER_UNAVAILABLE, L"RPC_S_SERVER_UNAVAILABLE" },
    { RPC_S_SERVER_TOO_BUSY, L"RPC_S_SERVER_TOO_BUSY" },
    { RPC_S_CALL_FAILED, L"RPC_S_CALL_FAILED" },
    { RPC_S_CALL_FAILED_DNE, L"RPC_S_CALL_FAILED_DNE" },
    { RPC_S_PROTOCOL_ERROR, L"RPC_S_PROTOCOL_ERROR" },
    { RPC_S_UNKNOWN_IF, L"RPC_S_UNKNOWN_IF" },
    { RPC_S_PROCNUM_OUT_OF_RANGE, L"RPC_S_PROCNUM_OUT_OF_RANGE" },
    { EPT_S_NOT_REGISTERED, L"EPT_S_NOT_REGISTERED" },
    { RPC_X_NULL_REF_POINTER, L"RPC_X_NULL_REF_POINTER" },
    { RPC_S_SEC_PKG_ERROR, L"RPC_S_SEC_PKG_ERROR" },
    { NERR_NetNotStarted, L"NERR_NetNotStarted" },
    { NERR_WkstaNotStarted, L"NERR_WkstaNotStarted" },
    { NERR_ServerNotStarted, L"NERR_ServerNotStarted" },
    { NERR_ServiceNotInstalled, L"NERR_ServiceNotInstalled" },
    { NERR_BufTooSmall, L"NERR_BufTooSmall" },
    { NERR_InvalidComputer, L"NERR_InvalidComputer" },
    { NERR_DCNotFound, L"NERR_DCNotFound" },
    { NERR_SetupNotJoined, L"NERR_SetupNotJoined" },
};

static PCWSTR
StatusName(DWORD Status)
{
    UINT i;

    for (i = 0; i < ARRAYSIZE(StatusNames); i++)
    {
        if (StatusNames[i].Code == Status)
            return StatusNames[i].Name;
    }
    return L"";
}

static VOID
PrintStatus(DWORD Status)
{
    ConResPrintf(StdOut, IDS_STATUS, Status, Status, StatusName(Status));
}

/* Prints "<what> Status = n 0xn NAME" and returns the status as the exit code */
static DWORD
Fail(UINT MessageId, DWORD Status)
{
    ConResPuts(StdOut, MessageId);
    PrintStatus(Status);
    return Status;
}

static VOID
AppendFlagNames(PWSTR Buffer, SIZE_T Count, DWORD Flags, const DWORD *Bits, PCWSTR *Names, UINT NameCount)
{
    UINT i;

    Buffer[0] = UNICODE_NULL;
    for (i = 0; i < NameCount; i++)
    {
        if (Flags & Bits[i])
            StringCchCatW(Buffer, Count, Names[i]);
    }
}

static VOID
FormatNetlogonFlags(DWORD Flags, PWSTR Buffer, SIZE_T Count)
{
    static const DWORD Bits[] =
    {
        NETLOGON_REPLICATION_NEEDED, NETLOGON_REPLICATION_IN_PROGRESS,
        NETLOGON_FULL_SYNC_REPLICATION, NETLOGON_REDO_NEEDED,
        NETLOGON_HAS_IP, NETLOGON_HAS_TIMESERV,
        NETLOGON_DNS_UPDATE_FAILURE, NETLOGON_VERIFY_STATUS_RETURNED
    };
    static PCWSTR Names[] =
    {
        L" REPLICATION_NEEDED", L" REPLICATION_IN_PROGRESS",
        L" FULL_SYNC", L" REDO_NEEDED",
        L" HAS_IP", L" HAS_TIMESERV",
        L" DNS_UPDATE_FAILURE", L" VERIFY_STATUS_RETURNED"
    };

    AppendFlagNames(Buffer, Count, Flags, Bits, Names, ARRAYSIZE(Bits));
}

static VOID
FormatDcFlags(DWORD Flags, PWSTR Buffer, SIZE_T Count)
{
    /* Same names and order as the Windows tool */
    static const DWORD Bits[] =
    {
        DS_NDNC_FLAG, DS_PDC_FLAG, DS_GC_FLAG, DS_DS_FLAG, DS_LDAP_FLAG, DS_KDC_FLAG,
        DS_TIMESERV_FLAG, DS_GOOD_TIMESERV_FLAG, DS_WRITABLE_FLAG,
        DS_DNS_CONTROLLER_FLAG, DS_DNS_DOMAIN_FLAG, DS_DNS_FOREST_FLAG, DS_CLOSEST_FLAG,
        DS_SELECT_SECRET_DOMAIN_6_FLAG, DS_FULL_SECRET_DOMAIN_6_FLAG, DS_WS_FLAG,
        DS_DS_8_FLAG, DS_DS_9_FLAG, DS_DS_10_FLAG, DS_KEY_LIST_FLAG, DS_DS_13_FLAG
    };
    static PCWSTR Names[] =
    {
        L" NDNC", L" PDC", L" GC", L" DS", L" LDAP", L" KDC",
        L" TIMESERV", L" GTIMESERV", L" WRITABLE",
        L" DNS_DC", L" DNS_DOMAIN", L" DNS_FOREST", L" CLOSE_SITE",
        L" PARTIAL_SECRET", L" FULL_SECRET", L" WS",
        L" DS_8", L" DS_9", L" DS_10", L" KEYLIST", L" DS_13"
    };

    AppendFlagNames(Buffer, Count, Flags, Bits, Names, ARRAYSIZE(Bits));
}

static PWSTR
GuidToString(const GUID *Guid)
{
    RPC_WSTR String = NULL;

    if (UuidToStringW((UUID *)Guid, &String) != RPC_S_OK)
        return NULL;
    return (PWSTR)String;
}

static VOID
FreeGuidString(PWSTR String)
{
    RPC_WSTR RpcString = (RPC_WSTR)String;

    if (RpcString)
        RpcStringFreeW(&RpcString);
}

/* ---- I_NetLogonControl family ------------------------------------------ */

static DWORD
NetlogonControl(DWORD FunctionCode, DWORD QueryLevel, PBYTE Data, PBYTE *Buffer)
{
    DWORD Status;

    *Buffer = NULL;
    if (Data != NULL)
        Status = I_NetLogonControl2(g_ServerName, FunctionCode, QueryLevel, Data, Buffer);
    else
        Status = I_NetLogonControl(g_ServerName, FunctionCode, QueryLevel, Buffer);

    if (Status != NERR_Success)
        return Fail(IDS_CONTROL_FAILED, Status);

    /* A server that reports success without an information block is not trusted */
    if (*Buffer == NULL)
    {
        ConResPuts(StdOut, IDS_CONTROL_NO_DATA);
        return ERROR_INVALID_DATA;
    }
    return NERR_Success;
}

static VOID
PrintInfo1(const NETLOGON_INFO_1 *Info)
{
    WCHAR Flags[256];

    FormatNetlogonFlags(Info->netlog1_flags, Flags, ARRAYSIZE(Flags));
    ConResPrintf(StdOut, IDS_FLAGS, Info->netlog1_flags, Flags);
    ConResPuts(StdOut, IDS_CONNECTION);
    PrintStatus(Info->netlog1_pdc_connection_status);
}

static DWORD
DoControlLevel1(DWORD FunctionCode, PBYTE Data)
{
    PBYTE Buffer;
    PNETLOGON_INFO_1 Info;
    DWORD Status;

    Status = NetlogonControl(FunctionCode, 1, Data, &Buffer);
    if (Status != NERR_Success)
        return Status;

    Info = (PNETLOGON_INFO_1)Buffer;
    PrintInfo1(Info);
    if (FunctionCode == NETLOGON_CONTROL_QUERY_DNS_REG)
    {
        ConResPuts(StdOut, (Info->netlog1_flags & NETLOGON_DNS_UPDATE_FAILURE)
                           ? IDS_DNS_UPDATE_FAILED : IDS_DNS_UPDATE_OK);
    }
    NetApiBufferFree(Buffer);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoControlWithName(DWORD FunctionCode, PCWSTR Name)
{
    LPWSTR Data = (LPWSTR)Name;   /* the data block carries a pointer to the string */

    return DoControlLevel1(FunctionCode, (PBYTE)&Data);
}

static DWORD
DoSecureChannel(DWORD FunctionCode, PCWSTR DomainName)
{
    LPWSTR Data = (LPWSTR)DomainName;
    PBYTE Buffer;
    PNETLOGON_INFO_2 Info;
    WCHAR Flags[256];
    DWORD Status;

    Status = NetlogonControl(FunctionCode, 2, (PBYTE)&Data, &Buffer);
    if (Status != NERR_Success)
        return Status;

    Info = (PNETLOGON_INFO_2)Buffer;
    FormatNetlogonFlags(Info->netlog2_flags, Flags, ARRAYSIZE(Flags));
    ConResPrintf(StdOut, IDS_FLAGS, Info->netlog2_flags, Flags);
    ConResPrintf(StdOut, IDS_TRUSTED_DC_NAME,
                 Info->netlog2_trusted_dc_name ? Info->netlog2_trusted_dc_name : L"");
    ConResPuts(StdOut, IDS_TRUSTED_DC_STATUS);
    PrintStatus(Info->netlog2_tc_connection_status);
    if (Info->netlog2_flags & NETLOGON_VERIFY_STATUS_RETURNED)
    {
        ConResPuts(StdOut, IDS_TRUST_VERIFICATION);
        PrintStatus(Info->netlog2_pdc_connection_status);
    }
    NetApiBufferFree(Buffer);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoLogonQuery(VOID)
{
    PBYTE Buffer;
    DWORD Status;

    Status = NetlogonControl(NETLOGON_CONTROL_QUERY, 3, NULL, &Buffer);
    if (Status != NERR_Success)
        return Status;

    ConResPrintf(StdOut, IDS_LOGON_ATTEMPTS, ((PNETLOGON_INFO_3)Buffer)->netlog3_logon_attempts);
    NetApiBufferFree(Buffer);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoFindUser(PCWSTR UserName)
{
    LPWSTR Data = (LPWSTR)UserName;
    PBYTE Buffer;
    PNETLOGON_INFO_4 Info;
    DWORD Status;

    Status = NetlogonControl(NETLOGON_CONTROL_FIND_USER, 4, (PBYTE)&Data, &Buffer);
    if (Status != NERR_Success)
        return Status;

    Info = (PNETLOGON_INFO_4)Buffer;
    ConResPrintf(StdOut, IDS_TRUSTED_DC_NAME,
                 Info->netlog4_trusted_dc_name ? Info->netlog4_trusted_dc_name : L"");
    ConResPrintf(StdOut, IDS_DOMAIN_NAME,
                 Info->netlog4_trusted_domain_name ? Info->netlog4_trusted_domain_name : L"");
    NetApiBufferFree(Buffer);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoDbFlag(PCWSTR HexFlags)
{
    DWORD Flags = wcstoul(HexFlags, NULL, 16);

    return DoControlLevel1(NETLOGON_CONTROL_SET_DBFLAG, (PBYTE)&Flags);
}

/* ---- DC locator --------------------------------------------------------- */

static DWORD
DoDcName(PCWSTR DomainName)
{
    PBYTE Buffer = NULL;
    DWORD Status;

    Status = NetGetDCName(g_ServerName, DomainName, &Buffer);
    if (Status != NERR_Success)
        return Fail(IDS_GETDCNAME_FAILED, Status);

    ConResPrintf(StdOut, IDS_PDC_FOR_DOMAIN, DomainName, Buffer ? (PWSTR)Buffer : L"");
    NetApiBufferFree(Buffer);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoDsGetDc(PCWSTR DomainName, PCWSTR SiteName, PCWSTR AccountName, ULONG Flags)
{
    PDOMAIN_CONTROLLER_INFOW Info = NULL;
    ULONG AccountBits = 0;
    PWSTR Guid;
    WCHAR FlagNames[512];
    DWORD Status;

    /* /ACCOUNT names a machine account, as in the Windows tool */
    if (AccountName != NULL)
        AccountBits = UF_WORKSTATION_TRUST_ACCOUNT | UF_SERVER_TRUST_ACCOUNT;

    Status = DsGetDcNameWithAccountW(g_ServerName, AccountName, AccountBits,
                                     DomainName, NULL, SiteName, Flags, &Info);
    if (Status != NERR_Success)
        return Fail(IDS_DSGETDC_FAILED, Status);
    if (Info == NULL)
    {
        ConResPuts(StdOut, IDS_CONTROL_NO_DATA);
        return ERROR_INVALID_DATA;
    }

    Guid = GuidToString(&Info->DomainGuid);
    FormatDcFlags(Info->Flags, FlagNames, ARRAYSIZE(FlagNames));
    ConResPrintf(StdOut, IDS_DC_INFO,
                 Info->DomainControllerName ? Info->DomainControllerName : L"",
                 Info->DomainControllerAddress ? Info->DomainControllerAddress : L"",
                 Guid ? Guid : L"(unavailable)",
                 Info->DomainName ? Info->DomainName : L"",
                 Info->DnsForestName ? Info->DnsForestName : L"",
                 Info->DcSiteName ? Info->DcSiteName : L"",
                 Info->ClientSiteName ? Info->ClientSiteName : L"",
                 FlagNames);
    FreeGuidString(Guid);
    NetApiBufferFree(Info);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoDcList(PCWSTR DomainName)
{
    PDOMAIN_CONTROLLER_INFOW Dc = NULL;
    PSERVER_INFO_101 Servers = NULL;
    DWORD Read = 0, Total = 0, i;
    DWORD Status;

    Status = DsGetDcNameW(g_ServerName, DomainName, NULL, NULL, 0, &Dc);
    if (Status != NERR_Success)
        return Fail(IDS_DCLIST_NO_DC, Status);
    if (Dc == NULL)
    {
        ConResPuts(StdOut, IDS_CONTROL_NO_DATA);
        return ERROR_INVALID_DATA;
    }

    if (Dc->Flags & DS_DS_FLAG)
        ConResPrintf(StdOut, IDS_DCLIST_FROM, DomainName, Dc->DomainControllerName);
    else
        ConResPrintf(StdOut, IDS_DCLIST_PRE2000, DomainName);

    /* The browser enumeration is the one DC list source available (no DsGetDomainControllerInfo yet) */
    Status = NetServerEnum(Dc->DomainControllerName, 101, (PBYTE *)&Servers, MAX_PREFERRED_LENGTH,
                           &Read, &Total, SV_TYPE_DOMAIN_CTRL | SV_TYPE_DOMAIN_BAKCTRL,
                           Dc->DomainName, NULL);
    if (Status != NERR_Success && Status != ERROR_MORE_DATA)
    {
        NetApiBufferFree(Dc);
        return Fail(IDS_DCLIST_ENUM_FAILED, Status);
    }

    ConResPrintf(StdOut, IDS_DCLIST_HEADER, DomainName);
    for (i = 0; i < Read; i++)
    {
        ConResPrintf(StdOut, IDS_DCLIST_ENTRY,
                     Servers[i].sv101_name ? Servers[i].sv101_name : L"",
                     (Servers[i].sv101_type & SV_TYPE_DOMAIN_CTRL) ? L" (PDC)" : L"");
    }
    if (Servers)
        NetApiBufferFree(Servers);
    NetApiBufferFree(Dc);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoDsGetSite(VOID)
{
    LPWSTR SiteName = NULL;
    DWORD Status;

    Status = DsGetSiteNameW(g_ServerName, &SiteName);
    if (Status != NERR_Success)
        return Fail(IDS_SITE_FAILED, Status);

    ConResPrintf(StdOut, IDS_NAME_LINE, SiteName ? SiteName : L"");
    NetApiBufferFree(SiteName);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoDsGetSiteCoverage(VOID)
{
    LPWSTR *SiteNames = NULL;
    ULONG Count = 0, i;
    DWORD Status;

    Status = DsGetDcSiteCoverageW(g_ServerName, &Count, &SiteNames);
    if (Status != NERR_Success)
        return Fail(IDS_SITECOV_FAILED, Status);

    for (i = 0; i < Count && SiteNames; i++)
        ConResPrintf(StdOut, IDS_NAME_LINE, SiteNames[i] ? SiteNames[i] : L"");
    if (SiteNames)
        NetApiBufferFree(SiteNames);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoAddressToSite(PCWSTR MachineName, PCWSTR AddressList)
{
    WSADATA WsaData;
    SOCKADDR_STORAGE Storage[MAX_ADDRESSES];
    SOCKET_ADDRESS Addresses[MAX_ADDRESSES];
    PCWSTR Tokens[MAX_ADDRESSES];
    PWSTR Copy, Token, Next;
    LPWSTR *SiteNames = NULL, *SubnetNames = NULL;
    DWORD Count = 0, i, Status;
    INT Length;

    if (AddressList == NULL || *AddressList == UNICODE_NULL)
    {
        ConResPuts(StdOut, IDS_ADDRESSES_REQUIRED);
        return ERROR_INVALID_PARAMETER;
    }

    Status = WSAStartup(MAKEWORD(2, 2), &WsaData);
    if (Status != 0)
        return Fail(IDS_WINSOCK_FAILED, Status);

    Copy = _wcsdup(AddressList);
    if (Copy == NULL)
    {
        WSACleanup();
        ConResPuts(StdOut, IDS_NOT_ENOUGH_MEMORY);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    for (Token = Copy; Token != NULL && *Token != UNICODE_NULL && Count < MAX_ADDRESSES; Token = Next)
    {
        Next = wcschr(Token, L',');
        if (Next != NULL)
            *Next++ = UNICODE_NULL;

        Length = sizeof(Storage[Count]);
        if (WSAStringToAddressW((LPWSTR)Token, AF_INET, NULL, (LPSOCKADDR)&Storage[Count], &Length) != 0)
        {
            Length = sizeof(Storage[Count]);
            if (WSAStringToAddressW((LPWSTR)Token, AF_INET6, NULL, (LPSOCKADDR)&Storage[Count], &Length) != 0)
            {
                Status = WSAGetLastError();
                ConResPrintf(StdOut, IDS_ADDR_INVALID, Token);
                PrintStatus(Status);
                free(Copy);
                WSACleanup();
                return Status;
            }
        }
        Addresses[Count].lpSockaddr = (LPSOCKADDR)&Storage[Count];
        Addresses[Count].iSockaddrLength = Length;
        Tokens[Count] = Token;
        Count++;
    }

    Status = DsAddressToSiteNamesExW(MachineName, Count, Addresses, &SiteNames, &SubnetNames);
    if (Status != NERR_Success)
    {
        free(Copy);
        WSACleanup();
        return Fail(IDS_ADDR_TO_SITE_FAILED, Status);
    }

    for (i = 0; i < Count; i++)
    {
        ConResPrintf(StdOut, IDS_ADDR_TO_SITE_LINE, Tokens[i],
                     (SiteNames && SiteNames[i]) ? SiteNames[i] : L"",
                     (SubnetNames && SubnetNames[i]) ? SubnetNames[i] : L"");
    }
    if (SiteNames)
        NetApiBufferFree(SiteNames);
    if (SubnetNames)
        NetApiBufferFree(SubnetNames);
    free(Copy);
    WSACleanup();
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoParentDomain(VOID)
{
    LPWSTR DomainName = NULL;
    BOOL PdcSameSite = FALSE;
    DWORD Status;

    Status = NetLogonGetTimeServiceParentDomain((LPWSTR)g_ServerName, &DomainName, &PdcSameSite);
    if (Status != NERR_Success)
        return Fail(IDS_PARENT_FAILED, Status);

    ConResPrintf(StdOut, IDS_PARENT_DOMAIN, DomainName ? DomainName : L"", (ULONG)PdcSameSite);
    NetApiBufferFree(DomainName);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

/* ---- Trusts ------------------------------------------------------------- */

static VOID
FormatTrustSuffix(const DS_DOMAIN_TRUSTSW *Trust, PWSTR Buffer, SIZE_T Count)
{
    WCHAR Item[64];

    Buffer[0] = UNICODE_NULL;
    switch (Trust->TrustType)
    {
        case TRUST_TYPE_DOWNLEVEL: StringCchCatW(Buffer, Count, L" (NT 4)"); break;
        case TRUST_TYPE_UPLEVEL:   StringCchCatW(Buffer, Count, L" (NT 5)"); break;
        case TRUST_TYPE_MIT:       StringCchCatW(Buffer, Count, L" (MIT)"); break;
        default:
            StringCchPrintfW(Item, ARRAYSIZE(Item), L" (Unknown Trust Type: %lu)", Trust->TrustType);
            StringCchCatW(Buffer, Count, Item);
            break;
    }

    if ((Trust->Flags & (DS_DOMAIN_IN_FOREST | DS_DOMAIN_TREE_ROOT)) == (DS_DOMAIN_IN_FOREST | DS_DOMAIN_TREE_ROOT))
    {
        StringCchCatW(Buffer, Count, L" (Forest Tree Root)");
    }
    else if (Trust->Flags & DS_DOMAIN_IN_FOREST)
    {
        StringCchPrintfW(Item, ARRAYSIZE(Item), L" (Forest: %lu)", Trust->ParentIndex);
        StringCchCatW(Buffer, Count, Item);
    }
    else if (Trust->Flags & DS_DOMAIN_TREE_ROOT)
    {
        StringCchCatW(Buffer, Count, L" (Tree Root but not in forest!!!!)");
    }

    if (Trust->Flags & DS_DOMAIN_DIRECT_OUTBOUND)
        StringCchCatW(Buffer, Count, L" (Direct Outbound)");
    if (Trust->Flags & DS_DOMAIN_DIRECT_INBOUND)
        StringCchCatW(Buffer, Count, L" (Direct Inbound)");
    if (Trust->Flags & DS_DOMAIN_PRIMARY)
        StringCchCatW(Buffer, Count, L" (Primary Domain)");
    if (Trust->Flags & DS_DOMAIN_NATIVE_MODE)
        StringCchCatW(Buffer, Count, L" (Native)");
    if (Trust->TrustAttributes != 0)
    {
        StringCchPrintfW(Item, ARRAYSIZE(Item), L" ( Attr: 0x%lx )", Trust->TrustAttributes);
        StringCchCatW(Buffer, Count, Item);
    }
}

static DWORD
DoDomainTrusts(ULONG Flags, BOOL Verbose)
{
    PDS_DOMAIN_TRUSTSW Trusts = NULL;
    ULONG Count = 0, i;
    WCHAR Suffix[256];
    DWORD Status;

    if (Flags == 0)
        Flags = DS_DOMAIN_PRIMARY | DS_DOMAIN_IN_FOREST | DS_DOMAIN_DIRECT_OUTBOUND | DS_DOMAIN_DIRECT_INBOUND;

    Status = DsEnumerateDomainTrustsW((LPWSTR)g_ServerName, Flags, &Trusts, &Count);
    if (Status != NERR_Success)
        return Fail(IDS_TRUSTS_FAILED, Status);

    ConResPuts(StdOut, IDS_TRUSTS_HEADER);
    for (i = 0; i < Count && Trusts; i++)
    {
        FormatTrustSuffix(&Trusts[i], Suffix, ARRAYSIZE(Suffix));
        ConResPrintf(StdOut, IDS_TRUST_ENTRY, i,
                     Trusts[i].NetbiosDomainName ? Trusts[i].NetbiosDomainName : L"",
                     Trusts[i].DnsDomainName ? Trusts[i].DnsDomainName : L"",
                     Suffix);
        if (Verbose)
        {
            PWSTR Guid = GuidToString(&Trusts[i].DomainGuid);
            LPWSTR Sid = NULL;

            ConResPrintf(StdOut, IDS_TRUST_GUID, Guid ? Guid : L"(unavailable)");
            FreeGuidString(Guid);
            if (Trusts[i].DomainSid && ConvertSidToStringSidW(Trusts[i].DomainSid, &Sid))
            {
                ConResPrintf(StdOut, IDS_TRUST_SID, Sid);
                LocalFree(Sid);
            }
        }
    }
    if (Trusts)
        NetApiBufferFree(Trusts);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static VOID
PrintForestTrustInfo(const LSA_FOREST_TRUST_INFORMATION *Info)
{
    ULONG i;

    for (i = 0; i < Info->RecordCount; i++)
    {
        const LSA_FOREST_TRUST_RECORD *Record = Info->Entries[i];

        if (Record == NULL)
            continue;
        switch (Record->ForestTrustType)
        {
            case ForestTrustTopLevelName:
                ConResPrintf(StdOut, IDS_FTI_TLN,
                             Record->ForestTrustData.TopLevelName.Length / sizeof(WCHAR),
                             Record->ForestTrustData.TopLevelName.Buffer);
                break;
            case ForestTrustTopLevelNameEx:
                ConResPrintf(StdOut, IDS_FTI_TLN_EX,
                             Record->ForestTrustData.TopLevelName.Length / sizeof(WCHAR),
                             Record->ForestTrustData.TopLevelName.Buffer);
                break;
            case ForestTrustDomainInfo:
                ConResPrintf(StdOut, IDS_FTI_DOMAIN,
                             Record->ForestTrustData.DomainInfo.DnsName.Length / sizeof(WCHAR),
                             Record->ForestTrustData.DomainInfo.DnsName.Buffer,
                             Record->ForestTrustData.DomainInfo.NetbiosName.Length / sizeof(WCHAR),
                             Record->ForestTrustData.DomainInfo.NetbiosName.Buffer);
                break;
            default:
                ConResPrintf(StdOut, IDS_FTI_INVALID, (LONG)Record->ForestTrustType);
                break;
        }
    }
}

static DWORD
DoDsGetForestTrustInfo(PCWSTR DomainName, BOOL UpdateTdo)
{
    PLSA_FOREST_TRUST_INFORMATION Info = NULL;
    DWORD Status;

    Status = DsGetForestTrustInformationW(g_ServerName, DomainName, UpdateTdo ? DS_GFTI_UPDATE_TDO : 0, &Info);
    if (Status != NERR_Success)
        return Fail(IDS_FTI_FAILED, Status);

    if (Info)
    {
        PrintForestTrustInfo(Info);
        NetApiBufferFree(Info);
    }
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static VOID
InitLsaString(PLSA_UNICODE_STRING String, PCWSTR Source)
{
    String->Buffer = (PWSTR)Source;
    String->Length = (USHORT)(wcslen(Source) * sizeof(WCHAR));
    String->MaximumLength = String->Length + sizeof(WCHAR);
}

static DWORD
DoLsaQueryForestTrustInfo(PCWSTR ForestName)
{
    LSA_OBJECT_ATTRIBUTES Attributes;
    LSA_UNICODE_STRING Server, Forest;
    LSA_HANDLE Policy = NULL;
    PLSA_FOREST_TRUST_INFORMATION Info = NULL;
    NTSTATUS NtStatus;
    DWORD Status;

    if (ForestName == NULL || *ForestName == UNICODE_NULL)
    {
        ConResPuts(StdOut, IDS_LSA_FOREST_REQUIRED);
        return ERROR_INVALID_PARAMETER;
    }

    ZeroMemory(&Attributes, sizeof(Attributes));
    if (g_ServerName)
        InitLsaString(&Server, g_ServerName);
    NtStatus = LsaOpenPolicy(g_ServerName ? &Server : NULL, &Attributes,
                             POLICY_VIEW_LOCAL_INFORMATION, &Policy);
    if (!NT_SUCCESS(NtStatus))
    {
        Status = LsaNtStatusToWinError(NtStatus);
        ConResPuts(StdOut, IDS_LSA_CONNECT_FAILED);
        PrintStatus(Status);
        return Status;
    }

    InitLsaString(&Forest, ForestName);
    NtStatus = LsaQueryForestTrustInformation(Policy, &Forest, &Info);
    if (!NT_SUCCESS(NtStatus))
    {
        Status = LsaNtStatusToWinError(NtStatus);
        ConResPuts(StdOut, IDS_LSA_QUERY_FAILED);
        PrintStatus(Status);
        LsaClose(Policy);
        return Status;
    }

    if (Info)
    {
        PrintForestTrustInfo(Info);
        LsaFreeMemory(Info);
    }
    LsaClose(Policy);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

/* ---- Misc --------------------------------------------------------------- */

static DWORD
DoTime(PCWSTR LowPart, PCWSTR HighPart)
{
    FILETIME FileTime, LocalTime;
    SYSTEMTIME SystemTime;

    FileTime.dwLowDateTime = wcstoul(LowPart, NULL, 16);
    FileTime.dwHighDateTime = wcstoul(HighPart, NULL, 16);
    if (!FileTimeToLocalFileTime(&FileTime, &LocalTime) ||
        !FileTimeToSystemTime(&LocalTime, &SystemTime))
    {
        ConResPuts(StdOut, IDS_TIME_CONVERT_FAILED);
        return ERROR_INVALID_PARAMETER;
    }

    ConResPrintf(StdOut, IDS_TIME_LINE, FileTime.dwLowDateTime, FileTime.dwHighDateTime,
                 SystemTime.wMonth, SystemTime.wDay, SystemTime.wYear,
                 SystemTime.wHour, SystemTime.wMinute, SystemTime.wSecond);
    return NERR_Success;
}

static VOID
EnablePrivilege(PCWSTR Name)
{
    HANDLE Token;
    TOKEN_PRIVILEGES Privileges;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return;
    if (LookupPrivilegeValueW(NULL, Name, &Privileges.Privileges[0].Luid))
    {
        Privileges.PrivilegeCount = 1;
        Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL);
    }
    CloseHandle(Token);
}

static DWORD
DoShutdown(PCWSTR Reason, PCWSTR Seconds)
{
    DWORD Timeout = 0;
    PWSTR End;

    if (Seconds != NULL)
    {
        Timeout = wcstoul(Seconds, &End, 10);
        if (*Seconds == UNICODE_NULL || *End != UNICODE_NULL)
        {
            ConResPuts(StdOut, IDS_SHUTDOWN_SECONDS);
            return ERROR_INVALID_PARAMETER;
        }
    }

    EnablePrivilege(g_ServerName ? SE_REMOTE_SHUTDOWN_NAME : SE_SHUTDOWN_NAME);
    if (!InitiateSystemShutdownExW((LPWSTR)g_ServerName, (LPWSTR)Reason, Timeout, TRUE, FALSE,
                                   SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
    {
        return Fail(IDS_SHUTDOWN_FAILED, GetLastError());
    }
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoShutdownAbort(VOID)
{
    EnablePrivilege(g_ServerName ? SE_REMOTE_SHUTDOWN_NAME : SE_SHUTDOWN_NAME);
    if (!AbortSystemShutdownW((LPWSTR)g_ServerName))
        return Fail(IDS_SHUTDOWN_ABORT_FAILED, GetLastError());
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

static DWORD
DoDeregisterDns(PCWSTR HostName, PCWSTR DomainName, PCWSTR DomainGuidText, PCWSTR DsaGuidText)
{
    GUID DomainGuid, DsaGuid;
    GUID *DomainGuidPtr = NULL, *DsaGuidPtr = NULL;
    DWORD Status;

    if (DomainGuidText != NULL)
    {
        if (UuidFromStringW((RPC_WSTR)DomainGuidText, &DomainGuid) != RPC_S_OK)
        {
            ConResPuts(StdOut, IDS_INVALID_DOMAIN_GUID);
            return ERROR_INVALID_PARAMETER;
        }
        DomainGuidPtr = &DomainGuid;
    }
    if (DsaGuidText != NULL)
    {
        if (UuidFromStringW((RPC_WSTR)DsaGuidText, &DsaGuid) != RPC_S_OK)
        {
            ConResPuts(StdOut, IDS_INVALID_DSA_GUID);
            return ERROR_INVALID_PARAMETER;
        }
        DsaGuidPtr = &DsaGuid;
    }

    Status = DsDeregisterDnsHostRecordsW((LPWSTR)g_ServerName, (LPWSTR)DomainName,
                                         DomainGuidPtr, DsaGuidPtr, (LPWSTR)HostName);
    if (Status != NERR_Success)
        return Fail(IDS_DEREGDNS_FAILED, Status);
    ConResPuts(StdOut, IDS_COMPLETED);
    return NERR_Success;
}

/* ---- Command line ------------------------------------------------------- */

typedef enum _VERB
{
    VERB_NONE,
    VERB_QUERY, VERB_REPL, VERB_SYNC, VERB_PDC_REPL,
    VERB_SC_QUERY, VERB_SC_RESET, VERB_SC_VERIFY, VERB_SC_CHANGE_PWD,
    VERB_DCLIST, VERB_DCNAME, VERB_DSGETDC, VERB_DSGETFTI, VERB_LSAQUERYFTI,
    VERB_DSGETSITE, VERB_DSGETSITECOV, VERB_DSADDRESSTOSITE, VERB_PARENTDOMAIN,
    VERB_FINDUSER, VERB_TRANSPORT_NOTIFY, VERB_DBFLAG, VERB_TIME, VERB_LOGON_QUERY,
    VERB_DOMAIN_TRUSTS, VERB_DSREGDNS, VERB_DSDEREGDNS, VERB_DSQUERYDNS,
    VERB_SHUTDOWN, VERB_SHUTDOWN_ABORT
} VERB;

/* "/NAME" or "/NAME:value", either switch character, case-insensitive */
static BOOL
MatchOption(PCWSTR Arg, PCWSTR Name, PCWSTR *Value)
{
    SIZE_T Length = wcslen(Name);

    if (Arg[0] != L'/' && Arg[0] != L'-')
        return FALSE;
    if (_wcsnicmp(Arg + 1, Name, Length) != 0)
        return FALSE;
    if (Arg[1 + Length] == UNICODE_NULL)
    {
        *Value = NULL;
        return TRUE;
    }
    if (Arg[1 + Length] == L':')
    {
        *Value = Arg + 2 + Length;
        return TRUE;
    }
    return FALSE;
}

static const struct
{
    PCWSTR Name;
    VERB Verb;
    BOOL NeedsValue;
} Verbs[] =
{
    { L"QUERY", VERB_QUERY, FALSE },
    { L"REPL", VERB_REPL, FALSE },
    { L"SYNC", VERB_SYNC, FALSE },
    { L"PDC_REPL", VERB_PDC_REPL, FALSE },
    { L"SC_QUERY", VERB_SC_QUERY, TRUE },
    { L"SC_RESET", VERB_SC_RESET, TRUE },
    { L"SC_VERIFY", VERB_SC_VERIFY, TRUE },
    { L"SC_CHANGE_PWD", VERB_SC_CHANGE_PWD, TRUE },
    { L"DCLIST", VERB_DCLIST, TRUE },
    { L"DCNAME", VERB_DCNAME, TRUE },
    { L"DSGETDC", VERB_DSGETDC, TRUE },
    { L"DSGETFTI", VERB_DSGETFTI, TRUE },
    { L"LSAQUERYFTI", VERB_LSAQUERYFTI, FALSE },
    { L"DSGETSITECOV", VERB_DSGETSITECOV, FALSE },
    { L"DSGETSITE", VERB_DSGETSITE, FALSE },
    { L"DSADDRESSTOSITE", VERB_DSADDRESSTOSITE, FALSE },
    { L"PARENTDOMAIN", VERB_PARENTDOMAIN, FALSE },
    { L"FINDUSER", VERB_FINDUSER, TRUE },
    { L"TRANSPORT_NOTIFY", VERB_TRANSPORT_NOTIFY, FALSE },
    { L"DBFLAG", VERB_DBFLAG, TRUE },
    { L"TIME", VERB_TIME, TRUE },
    { L"LOGON_QUERY", VERB_LOGON_QUERY, FALSE },
    { L"DOMAIN_TRUSTS", VERB_DOMAIN_TRUSTS, FALSE },
    { L"DSREGDNS", VERB_DSREGDNS, FALSE },
    { L"DSDEREGDNS", VERB_DSDEREGDNS, TRUE },
    { L"DSQUERYDNS", VERB_DSQUERYDNS, FALSE },
    { L"SHUTDOWN_ABORT", VERB_SHUTDOWN_ABORT, FALSE },
    { L"SHUTDOWN", VERB_SHUTDOWN, TRUE },
};

static const struct
{
    PCWSTR Name;
    ULONG Flag;
} DcFlagOptions[] =
{
    { L"PDC", DS_PDC_REQUIRED },
    { L"DSP", DS_DIRECTORY_SERVICE_PREFERRED },
    { L"DS_6", DS_DIRECTORY_SERVICE_6_REQUIRED },
    { L"DS_8", DS_DIRECTORY_SERVICE_8_REQUIRED },
    { L"DS_9", DS_DIRECTORY_SERVICE_9_REQUIRED },
    { L"DS_10", DS_DIRECTORY_SERVICE_10_REQUIRED },
    { L"DS_13", DS_DIRECTORY_SERVICE_13_REQUIRED },
    { L"DS", DS_DIRECTORY_SERVICE_REQUIRED },
    { L"GC", DS_GC_SERVER_REQUIRED },
    { L"KDC", DS_KDC_REQUIRED },
    { L"TIMESERV", DS_TIMESERV_REQUIRED },
    { L"GTIMESERV", DS_GOOD_TIMESERV_PREFERRED },
    { L"WS", DS_WEB_SERVICE_REQUIRED },
    { L"NETBIOS", DS_IS_FLAT_NAME },
    { L"DNS", DS_IS_DNS_NAME },
    { L"IP", DS_IP_REQUIRED },
    { L"FORCE", DS_FORCE_REDISCOVERY },
    { L"WRITABLE", DS_WRITABLE_REQUIRED },
    { L"AVOIDSELF", DS_AVOID_SELF },
    { L"LDAPONLY", DS_ONLY_LDAP_NEEDED },
    { L"BACKG", DS_BACKGROUND_ONLY },
    { L"KEYLIST", DS_KEY_LIST_SUPPORT_REQUIRED },
    { L"TRY_NEXT_CLOSEST_SITE", DS_TRY_NEXTCLOSEST_SITE },
    { L"RET_DNS", DS_RETURN_DNS_NAME },
    { L"RET_NETBIOS", DS_RETURN_FLAT_NAME },
};

static const struct
{
    PCWSTR Name;
    ULONG Flag;
} TrustFlagOptions[] =
{
    { L"PRIMARY", DS_DOMAIN_PRIMARY },
    { L"FOREST", DS_DOMAIN_IN_FOREST },
    { L"DIRECT_OUT", DS_DOMAIN_DIRECT_OUTBOUND },
    { L"DIRECT_IN", DS_DOMAIN_DIRECT_INBOUND },
    { L"ALL_TRUSTS", DS_DOMAIN_PRIMARY | DS_DOMAIN_IN_FOREST | DS_DOMAIN_DIRECT_OUTBOUND | DS_DOMAIN_DIRECT_INBOUND },
};

static int
Usage(PCON_STREAM Stream, int ExitCode)
{
    ConResPuts(Stream, IDS_USAGE);
    return ExitCode;
}

int
wmain(int argc, WCHAR *argv[])
{
    VERB Verb = VERB_NONE;
    PCWSTR VerbArg = NULL, SecondArg = NULL;
    PCWSTR SiteName = NULL, AccountName = NULL, Addresses = NULL;
    PCWSTR DnsDomain = NULL, DomainGuid = NULL, DsaGuid = NULL;
    ULONG DcFlags = 0, TrustFlags = 0;
    BOOL Verbose = FALSE, UpdateTdo = FALSE;
    int i;

    ConInitStdStreams();

    if (argc < 2)
    {
        ConResPuts(StdOut, IDS_NO_PARAMETERS);
        return 1;
    }

    for (i = 1; i < argc; i++)
    {
        PCWSTR Arg = argv[i], Value;
        UINT j;
        BOOL Matched = FALSE;

        if (!wcscmp(Arg, L"/?") || !wcscmp(Arg, L"-?") || MatchOption(Arg, L"HELP", &Value))
            return Usage(StdOut, 0);

        if (MatchOption(Arg, L"SERVER", &Value))
        {
            if (Value == NULL || *Value == UNICODE_NULL)
            {
                ConResPuts(StdOut, IDS_INVALID_SERVER);
                return 1;
            }
            g_ServerName = Value;
            continue;
        }

        for (j = 0; j < ARRAYSIZE(Verbs) && !Matched; j++)
        {
            if (MatchOption(Arg, Verbs[j].Name, &Value))
            {
                if (Verbs[j].NeedsValue && (Value == NULL || *Value == UNICODE_NULL))
                    return Usage(StdOut, 1);
                Verb = Verbs[j].Verb;
                VerbArg = Value;
                /* /TIME and /SHUTDOWN take a plain second argument */
                if ((Verb == VERB_TIME || Verb == VERB_SHUTDOWN) &&
                    i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-')
                {
                    SecondArg = argv[++i];
                }
                Matched = TRUE;
            }
        }
        if (Matched)
            continue;

        for (j = 0; j < ARRAYSIZE(DcFlagOptions) && !Matched; j++)
        {
            if (MatchOption(Arg, DcFlagOptions[j].Name, &Value) && Value == NULL)
            {
                DcFlags |= DcFlagOptions[j].Flag;
                Matched = TRUE;
            }
        }
        for (j = 0; j < ARRAYSIZE(TrustFlagOptions) && !Matched; j++)
        {
            if (MatchOption(Arg, TrustFlagOptions[j].Name, &Value) && Value == NULL)
            {
                TrustFlags |= TrustFlagOptions[j].Flag;
                Matched = TRUE;
            }
        }
        if (Matched)
            continue;

        if (MatchOption(Arg, L"SITE", &Value) && Value)
            SiteName = Value;
        else if (MatchOption(Arg, L"ACCOUNT", &Value) && Value)
            AccountName = Value;
        else if (MatchOption(Arg, L"ADDRESSES", &Value) && Value)
            Addresses = Value;
        else if (MatchOption(Arg, L"DOMGUID", &Value) && Value)
            DomainGuid = Value;
        else if (MatchOption(Arg, L"DSAGUID", &Value) && Value)
            DsaGuid = Value;
        else if (MatchOption(Arg, L"DOM", &Value) && Value)
            DnsDomain = Value;
        else if (MatchOption(Arg, L"UPDATE_TDO", &Value) && !Value)
            UpdateTdo = TRUE;
        else if (MatchOption(Arg, L"V", &Value) && !Value)
            Verbose = TRUE;
        else
            return Usage(StdOut, 1);
    }

    switch (Verb)
    {
        case VERB_QUERY:            return DoControlLevel1(NETLOGON_CONTROL_QUERY, NULL);
        case VERB_REPL:             return DoControlLevel1(NETLOGON_CONTROL_REPLICATE, NULL);
        case VERB_SYNC:             return DoControlLevel1(NETLOGON_CONTROL_SYNCHRONIZE, NULL);
        case VERB_PDC_REPL:         return DoControlLevel1(NETLOGON_CONTROL_PDC_REPLICATE, NULL);
        case VERB_TRANSPORT_NOTIFY: return DoControlLevel1(NETLOGON_CONTROL_TRANSPORT_NOTIFY, NULL);
        case VERB_DSREGDNS:         return DoControlLevel1(NETLOGON_CONTROL_FORCE_DNS_REG, NULL);
        case VERB_DSQUERYDNS:       return DoControlLevel1(NETLOGON_CONTROL_QUERY_DNS_REG, NULL);
        case VERB_SC_QUERY:         return DoSecureChannel(NETLOGON_CONTROL_TC_QUERY, VerbArg);
        case VERB_SC_RESET:         return DoSecureChannel(NETLOGON_CONTROL_REDISCOVER, VerbArg);
        case VERB_SC_VERIFY:        return DoSecureChannel(NETLOGON_CONTROL_TC_VERIFY, VerbArg);
        case VERB_SC_CHANGE_PWD:    return DoControlWithName(NETLOGON_CONTROL_CHANGE_PASSWORD, VerbArg);
        case VERB_FINDUSER:         return DoFindUser(VerbArg);
        case VERB_DBFLAG:           return DoDbFlag(VerbArg);
        case VERB_LOGON_QUERY:      return DoLogonQuery();
        case VERB_DCNAME:           return DoDcName(VerbArg);
        case VERB_DCLIST:           return DoDcList(VerbArg);
        case VERB_DSGETDC:          return DoDsGetDc(VerbArg, SiteName, AccountName, DcFlags);
        case VERB_DSGETSITE:        return DoDsGetSite();
        case VERB_DSGETSITECOV:     return DoDsGetSiteCoverage();
        case VERB_DSADDRESSTOSITE:  return DoAddressToSite(VerbArg, Addresses);
        case VERB_PARENTDOMAIN:     return DoParentDomain();
        case VERB_DOMAIN_TRUSTS:    return DoDomainTrusts(TrustFlags, Verbose);
        case VERB_DSGETFTI:         return DoDsGetForestTrustInfo(VerbArg, UpdateTdo);
        case VERB_LSAQUERYFTI:      return DoLsaQueryForestTrustInfo(VerbArg);
        case VERB_TIME:
            if (SecondArg == NULL)
                return Usage(StdOut, 1);
            return DoTime(VerbArg, SecondArg);
        case VERB_DSDEREGDNS:       return DoDeregisterDns(VerbArg, DnsDomain, DomainGuid, DsaGuid);
        case VERB_SHUTDOWN:         return DoShutdown(VerbArg, SecondArg);
        case VERB_SHUTDOWN_ABORT:   return DoShutdownAbort();
        case VERB_NONE:
        default:
            ConResPuts(StdOut, IDS_NO_PARAMETERS);
            return 1;
    }
}

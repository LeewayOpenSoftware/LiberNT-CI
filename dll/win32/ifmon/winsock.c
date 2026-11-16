/*
 * PROJECT:    ReactOS IF Monitor DLL
 * LICENSE:    GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:    NetSh Helper interface context functions
 * COPYRIGHT:  Copyright 2025 Eric Kohl <eric.kohl@reactos.org>
 */

#include "precomp.h"

#include <guiddef.h>
#include <ws2spi.h>
#include <sporder.h>

#define NDEBUG
#include <debug.h>

#include "guid.h"
#include "resource.h"

static FN_HANDLE_CMD ResetCommand;
static FN_HANDLE_CMD ShowCatalogCommand;

static
CMD_ENTRY g_TestCmdTable[] = 
{
    {L"reset", ResetCommand, IDS_HLP_WINSOCK_RESET, IDS_HLP_WINSOCK_RESET_EX, 0}
};

static
CMD_ENTRY g_WinsockShowCmdTable[] = 
{
    {L"catalog", ShowCatalogCommand, IDS_HLP_WINSOCK_SHOW_CATALOG, IDS_HLP_WINSOCK_SHOW_CATALOG_EX, 0}
};

static
CMD_GROUP_ENTRY g_WinsockGroupCmds[] = 
{
    {L"show", IDS_HLP_WINSOCK_SHOW, sizeof(g_WinsockShowCmdTable)/sizeof(CMD_ENTRY), 0, g_WinsockShowCmdTable, NULL},
};


/* The providers that the installation of TCP/IP puts into the catalog
   (media/inf/nettcpip.inf). Everything else is removed by a reset. The name
   space catalog is not touched */
static const GUID g_DefaultProviders[] =
{
    /* MSAFD Tcpip: TCP, UDP and raw, IPv4 and IPv6 */
    {0xE70F1AA0, 0xAB8B, 0x11CF, {0x8C, 0xA3, 0x00, 0x80, 0x5F, 0x48, 0xA1, 0x92}},
    /* AF_UNIX */
    {0xA00943D9, 0x9C2E, 0x4633, {0x9B, 0x59, 0x00, 0x57, 0xA3, 0x16, 0x09, 0x94}},
    /* MSAFD RfComm [Bluetooth] */
    {0x9FC48064, 0x7298, 0x43E4, {0xB7, 0xBD, 0x18, 0x1F, 0x20, 0x89, 0x79, 0x2A}}
};

static
BOOL
WinsockIsDefaultProvider(
    _In_ const GUID *pProviderId)
{
    DWORD i;

    for (i = 0; i < ARRAYSIZE(g_DefaultProviders); i++)
    {
        if (IsEqualGUID(pProviderId, &g_DefaultProviders[i]))
            return TRUE;
    }
    return FALSE;
}

static
VOID
WinsockFormatGuid(
    _In_ const GUID *pGuid,
    _Out_writes_(cchGuid) PWSTR pszGuid,
    _In_ SIZE_T cchGuid)
{
    StringCchPrintfW(pszGuid, cchGuid,
                     L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                     pGuid->Data1, pGuid->Data2, pGuid->Data3,
                     pGuid->Data4[0], pGuid->Data4[1], pGuid->Data4[2], pGuid->Data4[3],
                     pGuid->Data4[4], pGuid->Data4[5], pGuid->Data4[6], pGuid->Data4[7]);
}

/* The protocol entries of the catalog, to be freed with HeapFree. NULL on failure */
static
LPWSAPROTOCOL_INFOW
WinsockEnumCatalog(
    _Out_ PDWORD pdwCount,
    _Out_ PDWORD pdwError)
{
    LPWSAPROTOCOL_INFOW pProtocols = NULL;
    DWORD dwSize = 0;
    INT nCount, nError = 0;

    *pdwCount = 0;
    *pdwError = ERROR_SUCCESS;

    /* The first call tells the size, the list can grow before the second one */
    for (;;)
    {
        nCount = WSCEnumProtocols(NULL, pProtocols, &dwSize, &nError);
        if (nCount != SOCKET_ERROR)
            break;

        if (pProtocols)
            HeapFree(GetProcessHeap(), 0, pProtocols);
        pProtocols = NULL;
        if (nError != WSAENOBUFS || dwSize == 0)
        {
            *pdwError = nError ? (DWORD)nError : ERROR_GEN_FAILURE;
            return NULL;
        }

        pProtocols = HeapAlloc(GetProcessHeap(), 0, dwSize);
        if (pProtocols == NULL)
        {
            *pdwError = ERROR_NOT_ENOUGH_MEMORY;
            return NULL;
        }
    }

    if (pProtocols == NULL)
    {
        /* An empty catalog: give the caller something to free */
        pProtocols = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WSAPROTOCOL_INFOW));
        if (pProtocols == NULL)
        {
            *pdwError = ERROR_NOT_ENOUGH_MEMORY;
            return NULL;
        }
        nCount = 0;
    }

    *pdwCount = (DWORD)nCount;
    return pProtocols;
}

static
DWORD
WINAPI
ResetCommand(
    LPCWSTR pwszMachine,
    LPWSTR *argv,
    DWORD dwCurrentIndex,
    DWORD dwArgCount,
    DWORD dwFlags,
    LPCVOID pvData,
    BOOL *pbDone)
{
    LPWSAPROTOCOL_INFOW pProtocols;
    DWORD dwCount, dwIds, i, j, dwError = ERROR_SUCCESS;
    LPDWORD pIds;
    GUID Foreign;
    INT nError;
    WCHAR szGuid[40], szLine[512];
    BOOL bMissing = FALSE;

    /* Remove every provider the system did not install. The catalog is read
       again after each removal: one provider can have several entries */
    for (;;)
    {
        pProtocols = WinsockEnumCatalog(&dwCount, &dwError);
        if (pProtocols == NULL)
        {
            PrintMessage(L"The Winsock Catalog could not be read (error %lu).\n", dwError);
            return dwError;
        }

        for (i = 0; i < dwCount; i++)
        {
            if (!WinsockIsDefaultProvider(&pProtocols[i].ProviderId))
                break;
        }
        if (i == dwCount)
            break;

        Foreign = pProtocols[i].ProviderId;
        WinsockFormatGuid(&Foreign, szGuid, ARRAYSIZE(szGuid));
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"Removing provider %s (%s)\n",
                         szGuid, pProtocols[i].szProtocol);
        HeapFree(GetProcessHeap(), 0, pProtocols);
        PrintMessage(L"%s", szLine);

        nError = 0;
        if (WSCDeinstallProvider(&Foreign, &nError) == SOCKET_ERROR)
        {
            PrintMessage(L"The provider could not be removed (error %d).\n", nError);
            return nError ? (DWORD)nError : ERROR_GEN_FAILURE;
        }
    }

    /* What is left are the entries of the system. Their ids were given in the
       order of their installation, and that is the order to search them in */
    pIds = HeapAlloc(GetProcessHeap(), 0, (dwCount + 1) * sizeof(DWORD));
    if (pIds == NULL)
    {
        HeapFree(GetProcessHeap(), 0, pProtocols);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    dwIds = 0;
    for (i = 0; i < dwCount; i++)
    {
        DWORD dwId = pProtocols[i].dwCatalogEntryId;

        for (j = dwIds; j > 0 && pIds[j - 1] > dwId; j--)
            pIds[j] = pIds[j - 1];
        pIds[j] = dwId;
        dwIds++;
    }

    for (i = 0; i < ARRAYSIZE(g_DefaultProviders); i++)
    {
        for (j = 0; j < dwCount; j++)
        {
            if (IsEqualGUID(&pProtocols[j].ProviderId, &g_DefaultProviders[i]))
                break;
        }
        if (j == dwCount)
        {
            /* Only the installation of TCP/IP has the data of these entries */
            WinsockFormatGuid(&g_DefaultProviders[i], szGuid, ARRAYSIZE(szGuid));
            PrintMessage(L"A provider of the system is missing from the catalog: %s\n", szGuid);
            bMissing = TRUE;
        }
    }
    HeapFree(GetProcessHeap(), 0, pProtocols);

    if (dwIds != 0)
    {
        nError = WSCWriteProviderOrder(pIds, dwIds);
        if (nError != ERROR_SUCCESS)
        {
            HeapFree(GetProcessHeap(), 0, pIds);
            PrintMessage(L"The order of the catalog could not be written (error %d).\n", nError);
            return (DWORD)nError;
        }
    }
    HeapFree(GetProcessHeap(), 0, pIds);

    if (bMissing)
    {
        PrintMessage(L"The Winsock Catalog was not restored completely. Install TCP/IP again to restore the missing entries.\n");
        return ERROR_NOT_FOUND;
    }

    PrintMessage(L"\nSuccessfully reset the Winsock Catalog.\n");
    PrintMessage(L"You must restart the computer in order to complete the reset.\n\n");
    return ERROR_SUCCESS;
}


static
DWORD
WINAPI
ShowCatalogCommand(
    LPCWSTR pwszMachine,
    LPWSTR *argv,
    DWORD dwCurrentIndex,
    DWORD dwArgCount,
    DWORD dwFlags,
    LPCVOID pvData,
    BOOL *pbDone)
{
    LPWSAPROTOCOL_INFOW pProtocols = NULL;
    DWORD dwSize = 0, i;
    INT nCount, nError = 0, nPathLen;
    WCHAR szPath[MAX_PATH], szGuid[40], szLine[512];

    /* The first call tells the size, the list can grow before the second one */
    for (;;)
    {
        nCount = WSCEnumProtocols(NULL, pProtocols, &dwSize, &nError);
        if (nCount != SOCKET_ERROR)
            break;

        if (pProtocols)
            HeapFree(GetProcessHeap(), 0, pProtocols);
        pProtocols = NULL;
        if (nError != WSAENOBUFS || dwSize == 0)
            return nError ? (DWORD)nError : ERROR_GEN_FAILURE;

        pProtocols = HeapAlloc(GetProcessHeap(), 0, dwSize);
        if (pProtocols == NULL)
            return ERROR_NOT_ENOUGH_MEMORY;
    }

    for (i = 0; i < (DWORD)nCount; i++)
    {
        const WSAPROTOCOL_INFOW *p = &pProtocols[i];
        PCWSTR pszType;

        if (p->ProtocolChain.ChainLen == BASE_PROTOCOL)
            pszType = L"Base Service Provider";
        else if (p->ProtocolChain.ChainLen == LAYERED_PROTOCOL)
            pszType = L"Layered Service Provider";
        else
            pszType = L"Layered Chain Entry";

        szPath[0] = UNICODE_NULL;
        nPathLen = ARRAYSIZE(szPath);
        if (WSCGetProviderPath((LPGUID)&p->ProviderId, szPath, &nPathLen, &nError) == SOCKET_ERROR)
            szPath[0] = UNICODE_NULL;

        StringCchPrintfW(szGuid, ARRAYSIZE(szGuid),
                         L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                         p->ProviderId.Data1, p->ProviderId.Data2, p->ProviderId.Data3,
                         p->ProviderId.Data4[0], p->ProviderId.Data4[1], p->ProviderId.Data4[2],
                         p->ProviderId.Data4[3], p->ProviderId.Data4[4], p->ProviderId.Data4[5],
                         p->ProviderId.Data4[6], p->ProviderId.Data4[7]);

        PrintMessage(L"\nWinsock Catalog Provider Entry\n");
        PrintMessage(L"------------------------------------------------------\n");
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%s\n", L"Entry Type:", pszType);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%s\n", L"Description:", p->szProtocol);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%s\n", L"Provider ID:", szGuid);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%s\n", L"Provider Path:", szPath);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%lu\n", L"Catalog Entry ID:", p->dwCatalogEntryId);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%d\n", L"Version:", p->iVersion);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%d\n", L"Address Family:", p->iAddressFamily);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%d\n", L"Max Address Length:", p->iMaxSockAddr);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%d\n", L"Min Address Length:", p->iMinSockAddr);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%d\n", L"Socket Type:", p->iSocketType);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%d\n", L"Protocol:", p->iProtocol);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s0x%lx\n", L"Service Flags:", p->dwServiceFlags1);
        PrintMessage(L"%s", szLine);
        StringCchPrintfW(szLine, ARRAYSIZE(szLine), L"%-36s%d\n", L"Protocol Chain Length:", p->ProtocolChain.ChainLen);
        PrintMessage(L"%s", szLine);
    }
    PrintMessage(L"\n");

    if (pProtocols)
        HeapFree(GetProcessHeap(), 0, pProtocols);

    return ERROR_SUCCESS;
}


static
DWORD
WINAPI
WinsockStart(
    _In_ const GUID *pguidParent,
    _In_ DWORD dwVersion)
{
    NS_CONTEXT_ATTRIBUTES ContextAttributes;

    DPRINT1("WinsockStart()\n");

    ZeroMemory(&ContextAttributes, sizeof(ContextAttributes));
    ContextAttributes.dwVersion = 1;
    ContextAttributes.pwszContext = L"winsock";
    ContextAttributes.guidHelper = GUID_IFMON_WINSOCK;

    ContextAttributes.ulNumTopCmds = 1;
    ContextAttributes.pTopCmds = g_TestCmdTable;

    ContextAttributes.ulNumGroups = 1;
    ContextAttributes.pCmdGroups = g_WinsockGroupCmds;

    RegisterContext(&ContextAttributes);

    return ERROR_SUCCESS;
}


DWORD
WINAPI
RegisterWinsockHelper(VOID)
{
    NS_HELPER_ATTRIBUTES HelperAttributes;

    ZeroMemory(&HelperAttributes, sizeof(HelperAttributes));
    HelperAttributes.dwVersion  = 1;
    HelperAttributes.guidHelper = GUID_IFMON_WINSOCK;
    HelperAttributes.pfnStart = WinsockStart;
    HelperAttributes.pfnStop = NULL;
    RegisterHelper(NULL, &HelperAttributes);

    return ERROR_SUCCESS;
}

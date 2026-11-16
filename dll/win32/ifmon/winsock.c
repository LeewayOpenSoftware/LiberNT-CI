/*
 * PROJECT:    ReactOS IF Monitor DLL
 * LICENSE:    GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:    NetSh Helper interface context functions
 * COPYRIGHT:  Copyright 2025 Eric Kohl <eric.kohl@reactos.org>
 */

#include "precomp.h"

#include <guiddef.h>
#include <ws2spi.h>

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
    PrintMessage(L"ResetCommand(): Not implemented yet!\n");
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

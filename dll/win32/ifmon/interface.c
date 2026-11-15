/*
 * PROJECT:    ReactOS IF Monitor DLL
 * LICENSE:    GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:    NetSh Helper interface context functions
 * COPYRIGHT:  Copyright 2025 Eric Kohl <eric.kohl@reactos.org>
 */

#include "precomp.h"

#include <guiddef.h>

#define NDEBUG
#include <debug.h>

#include "guid.h"
#include "resource.h"

static FN_HANDLE_CMD InterfaceShowInterface;

static
CMD_ENTRY
InterfaceShowCommands[] = 
{
    {L"interface", InterfaceShowInterface, IDS_HLP_INTERFACE_SHOW_INTERFACE, IDS_HLP_INTERFACE_SHOW_INTERFACE_EX, 0}
};

static
CMD_GROUP_ENTRY
InterfaceGroups[] = 
{
    {L"show", IDS_HLP_INTERFACE_SHOW, sizeof(InterfaceShowCommands) / sizeof(CMD_ENTRY), 0, InterfaceShowCommands, NULL},
};


static
DWORD
WINAPI
InterfaceShowInterface(
    LPCWSTR pwszMachine,
    LPWSTR *argv,
    DWORD dwCurrentIndex,
    DWORD dwArgCount,
    DWORD dwFlags,
    LPCVOID pvData,
    BOOL *pbDone)
{
    PIP_ADAPTER_ADDRESSES pList = NULL, pAdapter;
    ULONG Size = 16384, Error;
    WCHAR Line[512];

    for (;;)
    {
        pList = HeapAlloc(GetProcessHeap(), 0, Size);
        if (pList == NULL)
            return ERROR_NOT_ENOUGH_MEMORY;

        Error = GetAdaptersAddresses(AF_UNSPEC,
                                     GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST |
                                     GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                     NULL, pList, &Size);
        if (Error == ERROR_SUCCESS)
            break;

        HeapFree(GetProcessHeap(), 0, pList);
        pList = NULL;
        if (Error == ERROR_NO_DATA)
            break;
        if (Error != ERROR_BUFFER_OVERFLOW)
            return Error;
    }

    PrintMessageFromModule(hDllInstance, IDS_INTERFACE_HEADER);

    for (pAdapter = pList; pAdapter; pAdapter = pAdapter->Next)
    {
        /* Windows lists the network connections here, not the loopback or the tunnels */
        if (pAdapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || pAdapter->IfType == IF_TYPE_TUNNEL)
            continue;

        /* A disabled connection has no adapter in this list, so every row is an enabled one */
        StringCchPrintfW(Line, ARRAYSIZE(Line), L"%-15s%-15s%-17s%s\n",
                         L"Enabled",
                         (pAdapter->OperStatus == IfOperStatusUp) ? L"Connected" : L"Disconnected",
                         L"Dedicated",
                         pAdapter->FriendlyName ? pAdapter->FriendlyName : L"");
        PrintMessage(L"%s", Line);
    }
    PrintMessage(L"\n");

    if (pList)
        HeapFree(GetProcessHeap(), 0, pList);

    return ERROR_SUCCESS;
}


static
DWORD
WINAPI
InterfaceDumpFn(
    _In_ LPCWSTR pwszRouter,
    _In_ LPWSTR *ppwcArguments,
    _In_ DWORD dwArgCount,
    _In_ LPCVOID pvData)
{
    DPRINT("InterfaceDumpFn(%S %p %lu %p)\n", pwszRouter, ppwcArguments, dwArgCount, pvData);

    PrintMessageFromModule(hDllInstance, IDS_DUMP_HEADERLINE);
    PrintMessage(L"# Interface Configuration\n");
    PrintMessageFromModule(hDllInstance, IDS_DUMP_HEADERLINE);
    PrintMessage(L"pushd interface\n");
    PrintMessageFromModule(hDllInstance, IDS_DUMP_NEWLINE);

    PrintMessageFromModule(hDllInstance, IDS_DUMP_NEWLINE);
    PrintMessage(L"popd\n");
    PrintMessage(L"# End of Interface Configuration\n");
    PrintMessageFromModule(hDllInstance, IDS_DUMP_NEWLINE);

    return ERROR_SUCCESS;
}


DWORD
WINAPI
InterfaceStart(
    _In_ const GUID *pguidParent,
    _In_ DWORD dwVersion)
{
    NS_CONTEXT_ATTRIBUTES ContextAttributes;

    DPRINT("InterfaceStart()\n");

    ZeroMemory(&ContextAttributes, sizeof(ContextAttributes));
    ContextAttributes.dwVersion = 1;
    ContextAttributes.pwszContext = L"interface";
    ContextAttributes.guidHelper = GUID_IFMON_INTERFACE;

    ContextAttributes.ulNumTopCmds = 0;
    ContextAttributes.pTopCmds = NULL;

    ContextAttributes.ulNumGroups = sizeof(InterfaceGroups) / sizeof(CMD_GROUP_ENTRY);
    ContextAttributes.pCmdGroups = InterfaceGroups;

    ContextAttributes.pfnDumpFn = InterfaceDumpFn;

    RegisterContext(&ContextAttributes);

    return ERROR_SUCCESS;
}


DWORD
WINAPI
RegisterInterfaceHelper(VOID)
{
    NS_HELPER_ATTRIBUTES HelperAttributes;

    DPRINT("RegisterInterfaceHelper()\n");

    ZeroMemory(&HelperAttributes, sizeof(HelperAttributes));
    HelperAttributes.dwVersion = 1;
    HelperAttributes.guidHelper = GUID_IFMON_INTERFACE;
    HelperAttributes.pfnStart = InterfaceStart;
    HelperAttributes.pfnStop = NULL;
    RegisterHelper(NULL, &HelperAttributes);

    return ERROR_SUCCESS;
}

/*
 * PROJECT:     ws2_32.dll API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for WSCDeinstallProvider and WSCWriteProviderOrder
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ws2_32.h"
#include <winreg.h>
#include <ws2spi.h>
#include <sporder.h>
#include <strsafe.h>

/* The test provider: never loaded, an address family nobody asks for */
static GUID TestProviderId = { 0x5f6b4c1e, 0x7a0d, 0x4d53, { 0x9e, 0x21, 0x4c, 0x45, 0x45, 0x57, 0x41, 0x59 } };
static const WCHAR TestProviderPath[] = L"%SystemRoot%\\system32\\lnt_catalog_test_provider.dll";
#define TEST_ADDRESS_FAMILY 0x7e
#define TEST_ENTRIES        2

static
LPWSAPROTOCOL_INFOW
EnumCatalog(
    _Out_ PDWORD Count)
{
    LPWSAPROTOCOL_INFOW Buffer = NULL;
    DWORD Size = 0;
    INT Result, Error = 0;

    *Count = 0;
    for (;;)
    {
        Result = WSCEnumProtocols(NULL, Buffer, &Size, &Error);
        if (Result != SOCKET_ERROR)
        {
            *Count = (DWORD)Result;
            return Buffer;
        }

        if (Buffer)
            HeapFree(GetProcessHeap(), 0, Buffer);
        Buffer = NULL;
        if (Error != WSAENOBUFS || Size == 0)
            return NULL;

        Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
        if (!Buffer)
            return NULL;
    }
}

static
DWORD
ReadCatalogDword(
    _In_ PCSTR Name)
{
    HKEY Key;
    DWORD Value = 0xFFFFFFFF, Size = sizeof(Value);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Services\\WinSock2\\Parameters\\Protocol_Catalog9",
                      0,
                      KEY_READ,
                      &Key) != ERROR_SUCCESS)
    {
        return 0xFFFFFFFF;
    }

    RegQueryValueExA(Key, Name, NULL, NULL, (LPBYTE)&Value, &Size);
    RegCloseKey(Key);
    return Value;
}

static
DWORD
CountOfProvider(
    _In_reads_(Count) const WSAPROTOCOL_INFOW *List,
    _In_ DWORD Count)
{
    DWORD i, Found = 0;

    for (i = 0; i < Count; i++)
    {
        if (IsEqualGUID(&List[i].ProviderId, &TestProviderId))
            Found++;
    }
    return Found;
}

START_TEST(catalog_write)
{
    WSAPROTOCOL_INFOW Install[TEST_ENTRIES];
    LPWSAPROTOCOL_INFOW Before, With, Ordered, After;
    DWORD BeforeCount, WithCount, OrderedCount, AfterCount;
    DWORD Ids[TEST_ENTRIES], Bad[2], *Full;
    DWORD SerialBefore, Serial, NextIdBefore, i;
    INT Result, Error;

    /* A run that was stopped half way may have left the test provider */
    Error = 0;
    WSCDeinstallProvider(&TestProviderId, &Error);

    /* Arguments that are refused before anything is touched */
    Error = 0xdeadbeef;
    Result = WSCDeinstallProvider(NULL, &Error);
    ok(Result == SOCKET_ERROR, "WSCDeinstallProvider(NULL) returned %d\n", Result);
    ok(Error == WSAEINVAL, "WSCDeinstallProvider(NULL) set error %d\n", Error);

    Result = WSCWriteProviderOrder(NULL, 1);
    ok(Result == WSAEINVAL, "WSCWriteProviderOrder(NULL, 1) returned %d\n", Result);
    Bad[0] = 0;
    Result = WSCWriteProviderOrder(Bad, 0);
    ok(Result == WSAEINVAL, "WSCWriteProviderOrder(list, 0) returned %d\n", Result);

    Before = EnumCatalog(&BeforeCount);
    if (!Before || BeforeCount == 0)
    {
        skip("The protocol catalog cannot be read\n");
        return;
    }
    ok(CountOfProvider(Before, BeforeCount) == 0, "The test provider is in the catalog before the test\n");

    SerialBefore = ReadCatalogDword("Serial_Access_Num");
    NextIdBefore = ReadCatalogDword("Next_Catalog_Entry_ID");

    /* A provider that is not there */
    Error = 0xdeadbeef;
    Result = WSCDeinstallProvider(&TestProviderId, &Error);
    ok(Result == SOCKET_ERROR, "Deinstall of an unknown provider returned %d\n", Result);
    ok(Error == WSAEINVAL, "Deinstall of an unknown provider set error %d\n", Error);
    ok(ReadCatalogDword("Serial_Access_Num") == SerialBefore, "A refused deinstall changed the counter\n");

    /* Install the test provider */
    RtlZeroMemory(Install, sizeof(Install));
    for (i = 0; i < TEST_ENTRIES; i++)
    {
        Install[i].dwServiceFlags1 = XP1_CONNECTIONLESS | XP1_MESSAGE_ORIENTED;
        Install[i].ProtocolChain.ChainLen = BASE_PROTOCOL;
        Install[i].iVersion = 2;
        Install[i].iAddressFamily = TEST_ADDRESS_FAMILY;
        Install[i].iMaxSockAddr = 16;
        Install[i].iMinSockAddr = 16;
        Install[i].iSocketType = SOCK_DGRAM;
        Install[i].iProtocol = 200 + i;
        StringCchPrintfW(Install[i].szProtocol, ARRAYSIZE(Install[i].szProtocol),
                         L"Catalog write test entry %lu", i + 1);
    }

    Error = 0;
    Result = WSCInstallProvider(&TestProviderId, TestProviderPath, Install, TEST_ENTRIES, &Error);
    if (Result == SOCKET_ERROR)
    {
        skip("The test provider cannot be installed (error %d): not an administrator?\n", Error);
        HeapFree(GetProcessHeap(), 0, Before);
        return;
    }

    With = EnumCatalog(&WithCount);
    ok(With != NULL, "The catalog cannot be read after the install\n");
    if (!With)
        goto Remove;
    ok(WithCount == BeforeCount + TEST_ENTRIES, "%lu entries after the install, expected %lu\n",
       WithCount, BeforeCount + TEST_ENTRIES);
    ok(CountOfProvider(With, WithCount) == TEST_ENTRIES, "The test provider has %lu entries\n",
       CountOfProvider(With, WithCount));
    if (WithCount != BeforeCount + TEST_ENTRIES)
    {
        HeapFree(GetProcessHeap(), 0, With);
        goto Remove;
    }
    for (i = 0; i < TEST_ENTRIES; i++)
    {
        const WSAPROTOCOL_INFOW *Entry = &With[BeforeCount + i];

        ok(IsEqualGUID(&Entry->ProviderId, &TestProviderId), "Entry %lu from the end is not the test provider\n", i);
        ok(Entry->iProtocol == (INT)(200 + i), "Test entry %lu has protocol %d\n", i, Entry->iProtocol);
        ok(Entry->dwCatalogEntryId >= NextIdBefore, "Test entry %lu has the used id %lu\n", i, Entry->dwCatalogEntryId);
        Ids[i] = Entry->dwCatalogEntryId;
    }
    Serial = ReadCatalogDword("Serial_Access_Num");

    /* Orders that are refused change nothing */
    Bad[0] = Ids[0];
    Bad[1] = Ids[0];
    Result = WSCWriteProviderOrder(Bad, 2);
    ok(Result == WSAEINVAL, "An order with an id twice returned %d\n", Result);
    Bad[0] = 0x7ffffff1;
    Result = WSCWriteProviderOrder(Bad, 1);
    ok(Result == WSAEINVAL, "An order with an unknown id returned %d\n", Result);
    ok(ReadCatalogDword("Serial_Access_Num") == Serial, "A refused order changed the counter\n");

    /* The order the catalog has: accepted, and nothing is written */
    Full = HeapAlloc(GetProcessHeap(), 0, WithCount * sizeof(DWORD));
    if (Full)
    {
        for (i = 0; i < WithCount; i++)
            Full[i] = With[i].dwCatalogEntryId;
        Result = WSCWriteProviderOrder(Full, WithCount);
        ok(Result == ERROR_SUCCESS, "The present order returned %d\n", Result);
        ok(ReadCatalogDword("Serial_Access_Num") == Serial, "Writing the present order changed the counter\n");
    }

    /* The two test entries first, the second before the first */
    Bad[0] = Ids[1];
    Bad[1] = Ids[0];
    Result = WSCWriteProviderOrder(Bad, 2);
    ok(Result == ERROR_SUCCESS, "Moving the test entries to the front returned %d\n", Result);
    ok(ReadCatalogDword("Serial_Access_Num") == Serial + 1, "The counter is %lu after one order write, expected %lu\n",
       ReadCatalogDword("Serial_Access_Num"), Serial + 1);
    ok(ReadCatalogDword("Num_Catalog_Entries") == WithCount, "The registry counts %lu entries, expected %lu\n",
       ReadCatalogDword("Num_Catalog_Entries"), WithCount);

    Ordered = EnumCatalog(&OrderedCount);
    ok(Ordered != NULL && OrderedCount == WithCount, "%lu entries after the order write, expected %lu\n",
       OrderedCount, WithCount);
    if (Ordered && OrderedCount == WithCount)
    {
        ok(Ordered[0].dwCatalogEntryId == Ids[1], "First entry has id %lu, expected %lu\n",
           Ordered[0].dwCatalogEntryId, Ids[1]);
        ok(Ordered[1].dwCatalogEntryId == Ids[0], "Second entry has id %lu, expected %lu\n",
           Ordered[1].dwCatalogEntryId, Ids[0]);
        /* The others keep their order and every byte */
        for (i = 0; i < BeforeCount; i++)
        {
            ok(!memcmp(&Ordered[TEST_ENTRIES + i], &Before[i], sizeof(WSAPROTOCOL_INFOW)),
               "Entry %lu changed when the order was written\n", i);
        }
    }
    if (Ordered)
        HeapFree(GetProcessHeap(), 0, Ordered);

    /* Back to the order before, with the full list */
    if (Full)
    {
        Result = WSCWriteProviderOrder(Full, WithCount);
        ok(Result == ERROR_SUCCESS, "Writing the first order back returned %d\n", Result);
        HeapFree(GetProcessHeap(), 0, Full);
    }
    HeapFree(GetProcessHeap(), 0, With);

Remove:
    Serial = ReadCatalogDword("Serial_Access_Num");
    Error = 0;
    Result = WSCDeinstallProvider(&TestProviderId, &Error);
    ok(Result == ERROR_SUCCESS, "Deinstall of the test provider returned %d, error %d\n", Result, Error);
    ok(ReadCatalogDword("Serial_Access_Num") == Serial + 1, "The counter is %lu after the deinstall, expected %lu\n",
       ReadCatalogDword("Serial_Access_Num"), Serial + 1);
    ok(ReadCatalogDword("Num_Catalog_Entries") == BeforeCount, "The registry counts %lu entries, expected %lu\n",
       ReadCatalogDword("Num_Catalog_Entries"), BeforeCount);
    ok(ReadCatalogDword("Next_Catalog_Entry_ID") >= NextIdBefore + TEST_ENTRIES,
       "The next id is %lu: ids of removed entries would be given again\n",
       ReadCatalogDword("Next_Catalog_Entry_ID"));

    After = EnumCatalog(&AfterCount);
    ok(After != NULL && AfterCount == BeforeCount, "%lu entries at the end, %lu at the start\n",
       AfterCount, BeforeCount);
    if (After && AfterCount == BeforeCount)
    {
        ok(!memcmp(After, Before, BeforeCount * sizeof(WSAPROTOCOL_INFOW)),
           "The catalog at the end differs from the catalog at the start\n");
    }
    if (After)
        HeapFree(GetProcessHeap(), 0, After);

    /* A second deinstall finds nothing */
    Error = 0xdeadbeef;
    Result = WSCDeinstallProvider(&TestProviderId, &Error);
    ok(Result == SOCKET_ERROR, "A second deinstall returned %d\n", Result);
    ok(Error == WSAEINVAL, "A second deinstall set error %d\n", Error);

    HeapFree(GetProcessHeap(), 0, Before);
}

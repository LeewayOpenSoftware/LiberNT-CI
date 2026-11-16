/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS WinSock 2 API
 * FILE:        dll/win32/ws2_32/src/spinstal.c
 * PURPOSE:     Transport Service Provider Installation
 * PROGRAMMER:  Alex Ionescu (alex@relsoft.net)
 */

/* INCLUDES ******************************************************************/

#include <ws2_32.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

typedef struct _WS_PACKED_CATALOG_ITEM
{
    CHAR DllPath[MAX_PATH];
    WSAPROTOCOL_INFOW ProtocolInfo;
} WS_PACKED_CATALOG_ITEM;

static
INT
WsTcInstallProvider(IN LPGUID lpProviderId,
                    IN CONST WCHAR *lpszProviderDllPath,
                    IN CONST WSAPROTOCOL_INFOW *lpProtocolInfoList,
                    IN DWORD dwNumberOfEntries)
{
    WS_PACKED_CATALOG_ITEM PackedItem;
    PTCATALOG_ENTRY CatalogEntry;
    PTCATALOG Catalog;
    HKEY RootKey, EntriesKey = NULL, EntryKey, LockKey;
    CHAR LockName[32];
    CHAR EntryName[13];
    DWORD Serial, CurrentSerial, EntryCount, NextId, Disposition, RegType, RegSize, i;
    BOOL Locked = FALSE;
    LONG Status;
    INT ErrorCode = WSANO_RECOVERY;

    if (!lpProviderId || !lpszProviderDllPath || !lpProtocolInfoList || !dwNumberOfEntries)
        return WSAEINVAL;

    RtlZeroMemory(&PackedItem, sizeof(PackedItem));
    if (!WideCharToMultiByte(CP_ACP,
                             0,
                             lpszProviderDllPath,
                             -1,
                             PackedItem.DllPath,
                             sizeof(PackedItem.DllPath),
                             NULL,
                             NULL))
    {
        return WSAEINVAL;
    }

    RootKey = WsOpenRegistryRoot();
    if (!RootKey)
        return WSANO_RECOVERY;

    Catalog = WsTcAllocate();
    if (!Catalog)
    {
        RegCloseKey(RootKey);
        return WSA_NOT_ENOUGH_MEMORY;
    }

    if (!WsTcOpen(Catalog, RootKey))
    {
        RegCloseKey(RootKey);
        WsTcDelete(Catalog);
        return WSANO_RECOVERY;
    }
    RegCloseKey(RootKey);

    if (RegOpenKeyEx(Catalog->CatalogKey,
                     "Catalog_Entries",
                     0,
                     KEY_ALL_ACCESS,
                     &EntriesKey) != ERROR_SUCCESS)
    {
        EntriesKey = NULL;
        goto Cleanup;
    }

    RegType = REG_DWORD;
    RegSize = sizeof(EntryCount);
    if (RegQueryValueEx(Catalog->CatalogKey,
                        "Num_Catalog_Entries",
                        0,
                        &RegType,
                        (LPBYTE)&EntryCount,
                        &RegSize) != ERROR_SUCCESS)
    {
        goto Cleanup;
    }

    for (i = 1; i <= EntryCount; i++)
    {
        CatalogEntry = WsTcEntryAllocate();
        if (!CatalogEntry)
        {
            ErrorCode = WSA_NOT_ENOUGH_MEMORY;
            goto Cleanup;
        }

        if (WsTcEntryInitializeFromRegistry(CatalogEntry, EntriesKey, i) != ERROR_SUCCESS ||
            IsEqualGUID(&CatalogEntry->ProtocolInfo.ProviderId, lpProviderId))
        {
            WsTcEntryDereference(CatalogEntry);
            goto Cleanup;
        }
        WsTcEntryDereference(CatalogEntry);
    }

    RegType = REG_DWORD;
    RegSize = sizeof(Serial);
    if (RegQueryValueEx(Catalog->CatalogKey,
                        "Serial_Access_Num",
                        0,
                        &RegType,
                        (LPBYTE)&Serial,
                        &RegSize) != ERROR_SUCCESS)
    {
        goto Cleanup;
    }

    sprintf(LockName, "%8.8lX", Serial);
    if (RegCreateKeyEx(Catalog->CatalogKey,
                       LockName,
                       0,
                       NULL,
                       REG_OPTION_VOLATILE,
                       KEY_ALL_ACCESS,
                       NULL,
                       &LockKey,
                       &Disposition) != ERROR_SUCCESS)
    {
        goto Cleanup;
    }
    RegCloseKey(LockKey);
    if (Disposition != REG_CREATED_NEW_KEY)
        goto Cleanup;
    Locked = TRUE;

    RegType = REG_DWORD;
    RegSize = sizeof(CurrentSerial);
    if (RegQueryValueEx(Catalog->CatalogKey,
                        "Serial_Access_Num",
                        0,
                        &RegType,
                        (LPBYTE)&CurrentSerial,
                        &RegSize) != ERROR_SUCCESS ||
        CurrentSerial != Serial)
    {
        goto Cleanup;
    }

    RegType = REG_DWORD;
    RegSize = sizeof(EntryCount);
    if (RegQueryValueEx(Catalog->CatalogKey,
                        "Num_Catalog_Entries",
                        0,
                        &RegType,
                        (LPBYTE)&EntryCount,
                        &RegSize) != ERROR_SUCCESS)
    {
        goto Cleanup;
    }

    RegType = REG_DWORD;
    RegSize = sizeof(NextId);
    if (RegQueryValueEx(Catalog->CatalogKey,
                        "Next_Catalog_Entry_ID",
                        0,
                        &RegType,
                        (LPBYTE)&NextId,
                        &RegSize) != ERROR_SUCCESS)
    {
        goto Cleanup;
    }

    for (i = 0; i < dwNumberOfEntries; i++)
    {
        PackedItem.ProtocolInfo = lpProtocolInfoList[i];
        PackedItem.ProtocolInfo.ProviderId = *lpProviderId;
        PackedItem.ProtocolInfo.dwCatalogEntryId = NextId + i;

        sprintf(EntryName, "%0""12""lu", EntryCount + i + 1);
        if (RegCreateKeyEx(EntriesKey,
                           EntryName,
                           0,
                           NULL,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           NULL,
                           &EntryKey,
                           &Disposition) != ERROR_SUCCESS)
        {
            goto Cleanup;
        }

        Status = RegSetValueEx(EntryKey,
                               "PackedCatalogItem",
                               0,
                               REG_BINARY,
                               (LPBYTE)&PackedItem,
                               sizeof(PackedItem));
        RegCloseKey(EntryKey);
        if (Status != ERROR_SUCCESS)
            goto Cleanup;
    }

    NextId += dwNumberOfEntries;
    if (RegSetValueEx(Catalog->CatalogKey,
                      "Next_Catalog_Entry_ID",
                      0,
                      REG_DWORD,
                      (LPBYTE)&NextId,
                      sizeof(NextId)) != ERROR_SUCCESS)
    {
        goto Cleanup;
    }

    EntryCount += dwNumberOfEntries;
    if (RegSetValueEx(Catalog->CatalogKey,
                      "Num_Catalog_Entries",
                      0,
                      REG_DWORD,
                      (LPBYTE)&EntryCount,
                      sizeof(EntryCount)) != ERROR_SUCCESS)
    {
        goto Cleanup;
    }

    Serial++;
    RegSetValueEx(Catalog->CatalogKey,
                  "Serial_Access_Num",
                  0,
                  REG_DWORD,
                  (LPBYTE)&Serial,
                  sizeof(Serial));
    ErrorCode = ERROR_SUCCESS;

Cleanup:
    if (EntriesKey)
        RegCloseKey(EntriesKey);
    if (Locked)
        RegDeleteKey(Catalog->CatalogKey, LockName);
    WsTcDelete(Catalog);
    return ErrorCode;
}

/*
 * @unimplemented
 */
INT
WSPAPI
WSCInstallProvider(IN LPGUID lpProviderId,
                   IN CONST WCHAR * lpszProviderDllPath,
                   IN CONST LPWSAPROTOCOL_INFOW lpProtocolInfoList,
                   IN DWORD dwNumberOfEntries,
                   OUT LPINT lpErrno)
{
    INT ErrorCode;

    ErrorCode = WsTcInstallProvider(lpProviderId,
                                    lpszProviderDllPath,
                                    lpProtocolInfoList,
                                    dwNumberOfEntries);
    if (ErrorCode != ERROR_SUCCESS)
    {
        *lpErrno = ErrorCode;
        return SOCKET_ERROR;
    }

    return ERROR_SUCCESS;
}

INT
WSPAPI
WSCInstallProvider64_32(IN LPGUID lpProviderId,
                        IN CONST WCHAR * lpszProviderDllPath,
                        IN CONST LPWSAPROTOCOL_INFOW lpProtocolInfoList,
                        IN DWORD dwNumberOfEntries,
                        OUT LPINT lpErrno)
{
    return WSCInstallProvider(lpProviderId,
                              lpszProviderDllPath,
                              lpProtocolInfoList,
                              dwNumberOfEntries,
                              lpErrno);
}

/*
 * Catalog writes that remove or move entries.
 *
 * The registry gives no transaction, so the writes below are ordered to keep
 * one rule true at every instant, also when the writer is stopped half way:
 * every provider that is to remain is present at least once among the
 * positions 1 to Num_Catalog_Entries. An interrupted write can leave an entry
 * listed twice, never missing. Every writer first drops such doubled entries.
 *
 * The lock and the change counter are those of WsTcInstallProvider: a
 * volatile key named after Serial_Access_Num, and the counter goes up by one
 * when the write is done. Readers only watch the counter.
 */

/* More positions than this is not a catalog but damage */
#define WS_CATALOG_MAX_ENTRIES 4096

typedef struct _WS_CATALOG_WRITER
{
    PTCATALOG Catalog;
    HKEY EntriesKey;
    CHAR LockName[32];
    BOOL Locked;
    DWORD Serial;
    /* Positions the registry counts now */
    DWORD EntryCount;
    /* The entries without the doubled ones, in their order */
    DWORD ItemCount;
    WS_PACKED_CATALOG_ITEM *Items;
} WS_CATALOG_WRITER;

static
INT
WsTcMapRegistryError(IN LONG Status)
{
    if (Status == ERROR_ACCESS_DENIED)
        return WSAEACCES;
    if (Status == ERROR_NOT_ENOUGH_MEMORY || Status == ERROR_OUTOFMEMORY)
        return WSA_NOT_ENOUGH_MEMORY;
    return WSANO_RECOVERY;
}

static
LONG
WsTcReadDword(IN HKEY Key,
              IN LPCSTR Name,
              OUT LPDWORD Value)
{
    DWORD RegType = REG_DWORD, RegSize = sizeof(*Value);
    LONG Status;

    Status = RegQueryValueEx(Key, Name, 0, &RegType, (LPBYTE)Value, &RegSize);
    if (Status == ERROR_SUCCESS && (RegType != REG_DWORD || RegSize != sizeof(*Value)))
        Status = ERROR_INVALID_DATA;
    return Status;
}

static
LONG
WsTcReadPackedItem(IN HKEY EntriesKey,
                   IN DWORD Position,
                   OUT WS_PACKED_CATALOG_ITEM *Item)
{
    CHAR EntryName[13];
    DWORD RegType = REG_BINARY, RegSize = sizeof(*Item);
    HKEY EntryKey;
    LONG Status;

    sprintf(EntryName, "%0""12""lu", Position);
    Status = RegOpenKeyEx(EntriesKey, EntryName, 0, KEY_READ, &EntryKey);
    if (Status != ERROR_SUCCESS)
        return Status;

    RtlZeroMemory(Item, sizeof(*Item));
    Status = RegQueryValueEx(EntryKey, "PackedCatalogItem", 0, &RegType, (LPBYTE)Item, &RegSize);
    RegCloseKey(EntryKey);
    if (Status == ERROR_SUCCESS && (RegType != REG_BINARY || RegSize != sizeof(*Item)))
        Status = ERROR_INVALID_DATA;
    return Status;
}

static
LONG
WsTcWritePackedItem(IN HKEY EntriesKey,
                    IN DWORD Position,
                    IN CONST WS_PACKED_CATALOG_ITEM *Item)
{
    CHAR EntryName[13];
    DWORD Disposition;
    HKEY EntryKey;
    LONG Status;

    sprintf(EntryName, "%0""12""lu", Position);
    Status = RegCreateKeyEx(EntriesKey,
                            EntryName,
                            0,
                            NULL,
                            REG_OPTION_NON_VOLATILE,
                            KEY_ALL_ACCESS,
                            NULL,
                            &EntryKey,
                            &Disposition);
    if (Status != ERROR_SUCCESS)
        return Status;

    Status = RegSetValueEx(EntryKey,
                           "PackedCatalogItem",
                           0,
                           REG_BINARY,
                           (CONST BYTE *)Item,
                           sizeof(*Item));
    RegCloseKey(EntryKey);
    return Status;
}

static
LONG
WsTcWriteEntryCount(IN PTCATALOG Catalog,
                    IN DWORD EntryCount)
{
    return RegSetValueEx(Catalog->CatalogKey,
                         "Num_Catalog_Entries",
                         0,
                         REG_DWORD,
                         (CONST BYTE *)&EntryCount,
                         sizeof(EntryCount));
}

/* Positions First to Last are no longer counted: remove their keys */
static
VOID
WsTcDeletePositions(IN HKEY EntriesKey,
                    IN DWORD First,
                    IN DWORD Last)
{
    CHAR EntryName[13];
    DWORD Position;

    for (Position = First; Position <= Last && Position >= First; Position++)
    {
        sprintf(EntryName, "%0""12""lu", Position);
        RegDeleteKey(EntriesKey, EntryName);
    }
}

static
VOID
WsTcEndWrite(IN OUT WS_CATALOG_WRITER *Writer,
             IN BOOL Changed)
{
    DWORD Serial;

    if (Changed && Writer->Locked)
    {
        /* This is what tells the readers to read the catalog again */
        Serial = Writer->Serial + 1;
        RegSetValueEx(Writer->Catalog->CatalogKey,
                      "Serial_Access_Num",
                      0,
                      REG_DWORD,
                      (CONST BYTE *)&Serial,
                      sizeof(Serial));
    }

    if (Writer->EntriesKey)
        RegCloseKey(Writer->EntriesKey);
    if (Writer->Locked)
        RegDeleteKey(Writer->Catalog->CatalogKey, Writer->LockName);
    if (Writer->Catalog)
        WsTcDelete(Writer->Catalog);
    if (Writer->Items)
        HeapFree(GetProcessHeap(), 0, Writer->Items);
    RtlZeroMemory(Writer, sizeof(*Writer));
}

/* Takes the writer lock and reads the catalog. On failure nothing is held */
static
INT
WsTcBeginWrite(OUT WS_CATALOG_WRITER *Writer)
{
    HKEY RootKey, LockKey;
    DWORD Disposition, CurrentSerial, i, j;
    LONG Status;
    INT ErrorCode;

    RtlZeroMemory(Writer, sizeof(*Writer));

    RootKey = WsOpenRegistryRoot();
    if (!RootKey)
        return WSANO_RECOVERY;

    Writer->Catalog = WsTcAllocate();
    if (!Writer->Catalog)
    {
        RegCloseKey(RootKey);
        return WSA_NOT_ENOUGH_MEMORY;
    }

    if (!WsTcOpen(Writer->Catalog, RootKey))
    {
        RegCloseKey(RootKey);
        ErrorCode = WSANO_RECOVERY;
        goto Fail;
    }
    RegCloseKey(RootKey);

    Status = RegOpenKeyEx(Writer->Catalog->CatalogKey,
                          "Catalog_Entries",
                          0,
                          KEY_ALL_ACCESS,
                          &Writer->EntriesKey);
    if (Status != ERROR_SUCCESS)
    {
        Writer->EntriesKey = NULL;
        ErrorCode = WsTcMapRegistryError(Status);
        goto Fail;
    }

    Status = WsTcReadDword(Writer->Catalog->CatalogKey, "Serial_Access_Num", &Writer->Serial);
    if (Status != ERROR_SUCCESS)
    {
        ErrorCode = WsTcMapRegistryError(Status);
        goto Fail;
    }

    sprintf(Writer->LockName, "%8.8lX", Writer->Serial);
    Status = RegCreateKeyEx(Writer->Catalog->CatalogKey,
                            Writer->LockName,
                            0,
                            NULL,
                            REG_OPTION_VOLATILE,
                            KEY_ALL_ACCESS,
                            NULL,
                            &LockKey,
                            &Disposition);
    if (Status != ERROR_SUCCESS)
    {
        ErrorCode = WsTcMapRegistryError(Status);
        goto Fail;
    }
    RegCloseKey(LockKey);
    if (Disposition != REG_CREATED_NEW_KEY)
    {
        /* Another writer is at work: its key is not ours to delete */
        ErrorCode = WSATRY_AGAIN;
        goto Fail;
    }
    Writer->Locked = TRUE;

    Status = WsTcReadDword(Writer->Catalog->CatalogKey, "Serial_Access_Num", &CurrentSerial);
    if (Status != ERROR_SUCCESS || CurrentSerial != Writer->Serial)
    {
        ErrorCode = (Status == ERROR_SUCCESS) ? WSATRY_AGAIN : WsTcMapRegistryError(Status);
        goto Fail;
    }

    Status = WsTcReadDword(Writer->Catalog->CatalogKey, "Num_Catalog_Entries", &Writer->EntryCount);
    if (Status != ERROR_SUCCESS || Writer->EntryCount > WS_CATALOG_MAX_ENTRIES)
    {
        ErrorCode = WSANO_RECOVERY;
        goto Fail;
    }

    Writer->Items = HeapAlloc(GetProcessHeap(),
                              0,
                              (Writer->EntryCount + 1) * sizeof(WS_PACKED_CATALOG_ITEM));
    if (!Writer->Items)
    {
        ErrorCode = WSA_NOT_ENOUGH_MEMORY;
        goto Fail;
    }

    for (i = 1; i <= Writer->EntryCount; i++)
    {
        WS_PACKED_CATALOG_ITEM *Item = &Writer->Items[Writer->ItemCount];

        Status = WsTcReadPackedItem(Writer->EntriesKey, i, Item);
        if (Status != ERROR_SUCCESS)
        {
            ErrorCode = WSANO_RECOVERY;
            goto Fail;
        }

        /* An entry an interrupted writer left twice counts once, at its first place */
        for (j = 0; j < Writer->ItemCount; j++)
        {
            if (Writer->Items[j].ProtocolInfo.dwCatalogEntryId == Item->ProtocolInfo.dwCatalogEntryId)
                break;
        }
        if (j == Writer->ItemCount)
            Writer->ItemCount++;
    }

    return ERROR_SUCCESS;

Fail:
    WsTcEndWrite(Writer, FALSE);
    return ErrorCode;
}

static
INT
WsTcDeinstallProvider(IN LPGUID lpProviderId)
{
    WS_CATALOG_WRITER Writer;
    DWORD i, Kept = 0;
    LONG Status = ERROR_SUCCESS;
    INT ErrorCode;

    if (!lpProviderId)
        return WSAEINVAL;

    ErrorCode = WsTcBeginWrite(&Writer);
    if (ErrorCode != ERROR_SUCCESS)
        return ErrorCode;

    /* Bring the entries that stay to the front, in their order */
    for (i = 0; i < Writer.ItemCount; i++)
    {
        if (IsEqualGUID(&Writer.Items[i].ProtocolInfo.ProviderId, lpProviderId))
            continue;
        if (Kept != i)
            Writer.Items[Kept] = Writer.Items[i];
        Kept++;
    }

    if (Kept == Writer.ItemCount)
    {
        /* Not in the catalog */
        WsTcEndWrite(&Writer, FALSE);
        return WSAEINVAL;
    }

    /* An entry only moves to a lower position, so the copy at its old position
       is still there until a later entry takes that place */
    for (i = 0; i < Kept && Status == ERROR_SUCCESS; i++)
        Status = WsTcWritePackedItem(Writer.EntriesKey, i + 1, &Writer.Items[i]);

    if (Status == ERROR_SUCCESS)
        Status = WsTcWriteEntryCount(Writer.Catalog, Kept);

    if (Status == ERROR_SUCCESS)
        WsTcDeletePositions(Writer.EntriesKey, Kept + 1, Writer.EntryCount);

    /* Next_Catalog_Entry_ID stays: an id is not given twice */
    ErrorCode = (Status == ERROR_SUCCESS) ? ERROR_SUCCESS : WsTcMapRegistryError(Status);

    /* A write that failed half way has changed what readers see, too */
    WsTcEndWrite(&Writer, TRUE);
    return ErrorCode;
}

static
INT
WsTcWriteProviderOrder(IN LPDWORD lpwdCatalogEntryId,
                       IN DWORD dwNumberOfEntries)
{
    WS_CATALOG_WRITER Writer;
    WS_PACKED_CATALOG_ITEM *Ordered = NULL;
    DWORD i, j, Count, Base, Placed = 0;
    BOOL Same = TRUE, Changed = FALSE;
    LONG Status = ERROR_SUCCESS;
    INT ErrorCode;

    if (!lpwdCatalogEntryId || !dwNumberOfEntries)
        return WSAEINVAL;

    ErrorCode = WsTcBeginWrite(&Writer);
    if (ErrorCode != ERROR_SUCCESS)
        return ErrorCode;

    Count = Writer.ItemCount;
    ErrorCode = WSAEINVAL;
    if (dwNumberOfEntries > Count)
        goto Done;

    Ordered = HeapAlloc(GetProcessHeap(), 0, (Count + 1) * sizeof(WS_PACKED_CATALOG_ITEM));
    if (!Ordered)
    {
        ErrorCode = WSA_NOT_ENOUGH_MEMORY;
        goto Done;
    }

    /* The entries named by the caller come first, in the caller's order */
    for (i = 0; i < dwNumberOfEntries; i++)
    {
        for (j = 0; j < i; j++)
        {
            if (lpwdCatalogEntryId[j] == lpwdCatalogEntryId[i])
                goto Done;
        }
        for (j = 0; j < Count; j++)
        {
            if (Writer.Items[j].ProtocolInfo.dwCatalogEntryId == lpwdCatalogEntryId[i])
                break;
        }
        if (j == Count)
            goto Done;
        Ordered[Placed++] = Writer.Items[j];
    }

    /* The entries the caller did not name follow, in the order they have.
       Windows documents the call for a full list; a shorter one is taken
       this way here */
    for (j = 0; j < Count; j++)
    {
        for (i = 0; i < dwNumberOfEntries; i++)
        {
            if (Writer.Items[j].ProtocolInfo.dwCatalogEntryId == lpwdCatalogEntryId[i])
                break;
        }
        if (i == dwNumberOfEntries)
            Ordered[Placed++] = Writer.Items[j];
    }

    for (i = 0; i < Count && Same; i++)
    {
        if (Ordered[i].ProtocolInfo.dwCatalogEntryId != Writer.Items[i].ProtocolInfo.dwCatalogEntryId)
            Same = FALSE;
    }

    ErrorCode = ERROR_SUCCESS;
    if (Same && Writer.EntryCount == Count)
        goto Done;

    /* The new order cannot go over the old one in place: the entry at the
       first position would be gone before it is written again. So the new
       order is written behind the counted positions, counted in, written to
       the front, and the copies behind are counted out again */
    Changed = TRUE;
    Base = Writer.EntryCount;
    for (i = 0; i < Count && Status == ERROR_SUCCESS; i++)
        Status = WsTcWritePackedItem(Writer.EntriesKey, Base + i + 1, &Ordered[i]);

    if (Status == ERROR_SUCCESS)
        Status = WsTcWriteEntryCount(Writer.Catalog, Base + Count);

    for (i = 0; i < Count && Status == ERROR_SUCCESS; i++)
        Status = WsTcWritePackedItem(Writer.EntriesKey, i + 1, &Ordered[i]);

    if (Status == ERROR_SUCCESS)
        Status = WsTcWriteEntryCount(Writer.Catalog, Count);

    if (Status == ERROR_SUCCESS)
        WsTcDeletePositions(Writer.EntriesKey, Count + 1, Base + Count);
    else
        ErrorCode = WsTcMapRegistryError(Status);

Done:
    if (Ordered)
        HeapFree(GetProcessHeap(), 0, Ordered);
    WsTcEndWrite(&Writer, Changed);
    return ErrorCode;
}

/*
 * @implemented
 */
INT
WSPAPI
WSCDeinstallProvider(IN LPGUID lpProviderId,
                     OUT LPINT lpErrno)
{
    INT ErrorCode;

    ErrorCode = WsTcDeinstallProvider(lpProviderId);
    if (ErrorCode != ERROR_SUCCESS)
    {
        if (lpErrno)
            *lpErrno = ErrorCode;
        return SOCKET_ERROR;
    }

    return ERROR_SUCCESS;
}

/*
 * @implemented
 *
 * Returns the error itself, not SOCKET_ERROR: that is how the call is
 * documented.
 */
INT
WSPAPI
WSCWriteProviderOrder(IN LPDWORD lpwdCatalogEntryId,
                      IN DWORD dwNumberOfEntries)
{
    return WsTcWriteProviderOrder(lpwdCatalogEntryId, dwNumberOfEntries);
}

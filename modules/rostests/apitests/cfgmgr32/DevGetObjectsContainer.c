/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for DevGetObjects / DevGetObjectProperties on device containers
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <winreg.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <devfiltertypes.h>
#include <devquery.h>

static const DEVPROPCOMPKEY ConnectedKey =
{
    {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 55},
    DEVPROP_STORE_SYSTEM,
    NULL
};

static BOOL
IsContainerId(const WCHAR *Id)
{
    return Id && wcslen(Id) == 38 && Id[0] == L'{' && Id[37] == L'}' && Id[9] == L'-' && Id[14] == L'-' &&
           Id[19] == L'-' && Id[24] == L'-';
}

START_TEST(DevGetObjectsContainer)
{
    const DEV_OBJECT *Objects = NULL, *Connected = NULL;
    ULONG Count = 0, ConnectedCount = 0, i, Len = 0;
    const DEVPROPERTY *Props = NULL;
    DEVPROP_FILTER_EXPRESSION Filter;
    DEVPROP_BOOLEAN True = DEVPROP_TRUE;
    HRESULT hr;

    hr = DevGetObjects(DevObjectTypeDeviceContainer, DevQueryFlagAllProperties, 0, NULL, 0, NULL, &Count, &Objects);
    ok(hr == S_OK, "DevGetObjects(DeviceContainer) returned 0x%08lx\n", hr);
    if (hr != S_OK)
        return;
    ok(Count > 0, "no device containers returned\n");

    for (i = 0; i < Count; i++)
    {
        const DEVPROPERTY *Prop;

        ok(Objects[i].ObjectType == DevObjectTypeDeviceContainer, "object %lu has type %d\n", i, Objects[i].ObjectType);
        ok(IsContainerId(Objects[i].pszObjectId), "object %lu has id %ls\n", i, Objects[i].pszObjectId);

        Prop = DevFindProperty(&ConnectedKey.Key, DEVPROP_STORE_SYSTEM, NULL, Objects[i].cPropertyCount, Objects[i].pProperties);
        ok(Prop != NULL, "container %ls has no IsConnected property\n", Objects[i].pszObjectId);
        if (Prop)
        {
            ok(Prop->Type == DEVPROP_TYPE_BOOLEAN, "IsConnected type is 0x%lx\n", Prop->Type);
            ok(Prop->BufferSize == sizeof(DEVPROP_BOOLEAN), "IsConnected size is %lu\n", Prop->BufferSize);
        }

        Prop = DevFindProperty(&DEVPKEY_DeviceContainer_FriendlyName, DEVPROP_STORE_SYSTEM, NULL,
                               Objects[i].cPropertyCount, Objects[i].pProperties);
        if (Prop)
        {
            const WCHAR *Name = Prop->Buffer;

            ok(Prop->Type == DEVPROP_TYPE_STRING, "FriendlyName type is 0x%lx\n", Prop->Type);
            ok(Prop->BufferSize >= sizeof(WCHAR) && Name[Prop->BufferSize / sizeof(WCHAR) - 1] == 0,
               "FriendlyName is not terminated\n");
        }

        Prop = DevFindProperty(&DEVPKEY_DeviceContainer_Category, DEVPROP_STORE_SYSTEM, NULL,
                               Objects[i].cPropertyCount, Objects[i].pProperties);
        if (Prop)
        {
            const WCHAR *List = Prop->Buffer;
            ULONG Chars = Prop->BufferSize / sizeof(WCHAR);

            ok(Prop->Type == DEVPROP_TYPE_STRING_LIST, "Category type is 0x%lx\n", Prop->Type);
            ok(Chars >= 2 && List[Chars - 1] == 0 && List[Chars - 2] == 0, "Category is not double terminated\n");
        }
    }

    /* A filter on IsConnected returns exactly the connected containers. */
    memset(&Filter, 0, sizeof(Filter));
    Filter.Operator = DEVPROP_OPERATOR_EQUALS;
    Filter.Property.CompKey = ConnectedKey;
    Filter.Property.Type = DEVPROP_TYPE_BOOLEAN;
    Filter.Property.Buffer = &True;
    Filter.Property.BufferSize = sizeof(True);
    hr = DevGetObjects(DevObjectTypeDeviceContainer, DevQueryFlagAllProperties, 0, NULL, 1, &Filter, &ConnectedCount, &Connected);
    ok(hr == S_OK, "DevGetObjects with IsConnected filter returned 0x%08lx\n", hr);
    if (hr == S_OK)
    {
        ULONG Expected = 0;

        for (i = 0; i < Count; i++)
        {
            const DEVPROPERTY *Prop = DevFindProperty(&ConnectedKey.Key, DEVPROP_STORE_SYSTEM, NULL,
                                                      Objects[i].cPropertyCount, Objects[i].pProperties);
            if (Prop && *(const DEVPROP_BOOLEAN *)Prop->Buffer == DEVPROP_TRUE)
                Expected++;
        }
        ok(ConnectedCount == Expected, "filter returned %lu containers, expected %lu\n", ConnectedCount, Expected);
        DevFreeObjects(ConnectedCount, Connected);
    }

    /* The same container is returned by id, and an unknown id is not found. */
    hr = DevGetObjectProperties(DevObjectTypeDeviceContainer, Objects[0].pszObjectId, DevQueryFlagAllProperties, 0, NULL, &Len, &Props);
    ok(hr == S_OK, "DevGetObjectProperties(%ls) returned 0x%08lx\n", Objects[0].pszObjectId, hr);
    if (hr == S_OK)
    {
        ok(Len == Objects[0].cPropertyCount, "got %lu properties, expected %lu\n", Len, Objects[0].cPropertyCount);
        DevFreeObjectProperties(Len, Props);
    }

    Len = 0;
    Props = NULL;
    hr = DevGetObjectProperties(DevObjectTypeDeviceContainer, L"{00000000-0000-0000-0000-000000000001}", DevQueryFlagAllProperties, 0, NULL, &Len, &Props);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "unknown id returned 0x%08lx\n", hr);

    DevFreeObjects(Count, Objects);
}

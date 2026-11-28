/*
 * PROJECT:     NT Object Namespace shell extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shell extension entry point and exports
 * COPYRIGHT:   Copyright 2015-2017 David Quintana <gigaherz@gmail.com>
 */

#include "precomp.h"

#include <atlwin.h>

BEGIN_OBJECT_MAP(ObjectMap)
    OBJECT_ENTRY(CLSID_NtObjectFolder, CNtObjectFolder)
    OBJECT_ENTRY(CLSID_RegistryFolder, CRegistryFolder)
END_OBJECT_MAP()

HINSTANCE  g_hInstance;
CComModule g_Module;

STDAPI_(BOOL)
DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID fImpLoad)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        g_hInstance = hinstDLL;
        DisableThreadLibraryCalls(g_hInstance);

        g_Module.Init(ObjectMap, g_hInstance, NULL);
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        g_hInstance = NULL;
        g_Module.Term();
    }
    return TRUE;
}

/* Earlier versions listed both folders under Computer; drop those entries when registering again */
static VOID
RemoveSystemLocations(VOID)
{
    static const GUID *const Folders[] = { &CLSID_NtObjectFolder, &CLSID_RegistryFolder };
    UINT i;

    for (i = 0; i < _countof(Folders); i++)
    {
        WCHAR Guid[40];
        WCHAR Path[200];

        if (!StringFromGUID2(*Folders[i], Guid, _countof(Guid)))
            continue;
        lstrcpyW(Path, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\NameSpace\\");
        lstrcatW(Path, Guid);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, Path);
    }
}

STDAPI
DllRegisterServer(void)
{
    HRESULT hr = g_Module.DllRegisterServer(FALSE);

    if (SUCCEEDED(hr))
        RemoveSystemLocations();
    return hr;
}

STDAPI
DllUnregisterServer(void)
{
    return g_Module.DllUnregisterServer(FALSE);
}

STDAPI
DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    return g_Module.DllGetClassObject(rclsid, riid, ppv);
}

STDAPI
DllCanUnloadNow(void)
{
    return g_Module.DllCanUnloadNow();
}

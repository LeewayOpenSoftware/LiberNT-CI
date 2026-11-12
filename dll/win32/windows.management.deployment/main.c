/*
 * PROJECT:     LiberNT WinRT Windows.Management.Deployment
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     PackageManager runtime class that reports no installed packages
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "initguid.h"
#include "activation.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_ApplicationModel
#include "windows.applicationmodel.h"
#define WIDL_using_Windows_Management_Deployment
#include "windows.management.deployment.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(deployment);

typedef struct _EMPTY_ITERATOR
{
    IIterator_Package IIterator_Package_iface;
    LONG Ref;
} EMPTY_ITERATOR;

typedef struct _EMPTY_ITERABLE
{
    IIterable_Package IIterable_Package_iface;
    LONG Ref;
} EMPTY_ITERABLE;

typedef struct _PACKAGE_MANAGER
{
    IPackageManager IPackageManager_iface;
    LONG Ref;
} PACKAGE_MANAGER;

static HRESULT
GetIidsNone(ULONG *Count, IID **Iids)
{
    *Count = 0;
    *Iids = NULL;
    return S_OK;
}

static HRESULT
GetTrustBase(TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static EMPTY_ITERATOR *
ImplFromIterator(IIterator_Package *Iface)
{
    return CONTAINING_RECORD(Iface, EMPTY_ITERATOR, IIterator_Package_iface);
}

static HRESULT WINAPI
Iterator_QueryInterface(IIterator_Package *Iface, REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IIterator_Package))
    {
        *Out = Iface;
        IIterator_Package_AddRef(Iface);
        return S_OK;
    }
    ERR("iterator: no interface %s\n", debugstr_guid(Iid));
    *Out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Iterator_AddRef(IIterator_Package *Iface)
{
    return InterlockedIncrement(&ImplFromIterator(Iface)->Ref);
}

static ULONG WINAPI
Iterator_Release(IIterator_Package *Iface)
{
    EMPTY_ITERATOR *Impl = ImplFromIterator(Iface);
    ULONG Ref = InterlockedDecrement(&Impl->Ref);

    if (!Ref)
        free(Impl);
    return Ref;
}

static HRESULT WINAPI
Iterator_GetIids(IIterator_Package *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
Iterator_GetRuntimeClassName(IIterator_Package *Iface, HSTRING *Name)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Iterator_GetTrustLevel(IIterator_Package *Iface, TrustLevel *Level)
{
    return GetTrustBase(Level);
}

static HRESULT WINAPI
Iterator_get_Current(IIterator_Package *Iface, IPackage **Value)
{
    *Value = NULL;
    return E_BOUNDS;
}

static HRESULT WINAPI
Iterator_get_HasCurrent(IIterator_Package *Iface, boolean *Value)
{
    *Value = FALSE;
    return S_OK;
}

static HRESULT WINAPI
Iterator_MoveNext(IIterator_Package *Iface, boolean *Value)
{
    *Value = FALSE;
    return S_OK;
}

static HRESULT WINAPI
Iterator_GetMany(IIterator_Package *Iface, UINT32 Size, IPackage **Items, UINT32 *Count)
{
    *Count = 0;
    return S_OK;
}

static const IIterator_PackageVtbl IteratorVtbl =
{
    Iterator_QueryInterface,
    Iterator_AddRef,
    Iterator_Release,
    Iterator_GetIids,
    Iterator_GetRuntimeClassName,
    Iterator_GetTrustLevel,
    Iterator_get_Current,
    Iterator_get_HasCurrent,
    Iterator_MoveNext,
    Iterator_GetMany,
};

static EMPTY_ITERABLE *
ImplFromIterable(IIterable_Package *Iface)
{
    return CONTAINING_RECORD(Iface, EMPTY_ITERABLE, IIterable_Package_iface);
}

static HRESULT WINAPI
Iterable_QueryInterface(IIterable_Package *Iface, REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IIterable_Package))
    {
        *Out = Iface;
        IIterable_Package_AddRef(Iface);
        return S_OK;
    }
    ERR("iterable: no interface %s\n", debugstr_guid(Iid));
    *Out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Iterable_AddRef(IIterable_Package *Iface)
{
    return InterlockedIncrement(&ImplFromIterable(Iface)->Ref);
}

static ULONG WINAPI
Iterable_Release(IIterable_Package *Iface)
{
    EMPTY_ITERABLE *Impl = ImplFromIterable(Iface);
    ULONG Ref = InterlockedDecrement(&Impl->Ref);

    if (!Ref)
        free(Impl);
    return Ref;
}

static HRESULT WINAPI
Iterable_GetIids(IIterable_Package *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
Iterable_GetRuntimeClassName(IIterable_Package *Iface, HSTRING *Name)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Iterable_GetTrustLevel(IIterable_Package *Iface, TrustLevel *Level)
{
    return GetTrustBase(Level);
}

static HRESULT WINAPI
Iterable_First(IIterable_Package *Iface, IIterator_Package **Value)
{
    EMPTY_ITERATOR *Impl;

    *Value = NULL;
    if (!(Impl = calloc(1, sizeof(*Impl))))
        return E_OUTOFMEMORY;
    Impl->IIterator_Package_iface.lpVtbl = &IteratorVtbl;
    Impl->Ref = 1;
    *Value = &Impl->IIterator_Package_iface;
    return S_OK;
}

static const IIterable_PackageVtbl IterableVtbl =
{
    Iterable_QueryInterface,
    Iterable_AddRef,
    Iterable_Release,
    Iterable_GetIids,
    Iterable_GetRuntimeClassName,
    Iterable_GetTrustLevel,
    Iterable_First,
};

static HRESULT
CreateEmptyIterable(IIterable_Package **Value)
{
    EMPTY_ITERABLE *Impl;

    *Value = NULL;
    if (!(Impl = calloc(1, sizeof(*Impl))))
        return E_OUTOFMEMORY;
    Impl->IIterable_Package_iface.lpVtbl = &IterableVtbl;
    Impl->Ref = 1;
    *Value = &Impl->IIterable_Package_iface;
    return S_OK;
}

static PACKAGE_MANAGER *
ImplFromManager(IPackageManager *Iface)
{
    return CONTAINING_RECORD(Iface, PACKAGE_MANAGER, IPackageManager_iface);
}

static HRESULT WINAPI
Manager_QueryInterface(IPackageManager *Iface, REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IPackageManager))
    {
        FIXME("PackageManager: interface %s\n", debugstr_guid(Iid));
        *Out = Iface;
        IPackageManager_AddRef(Iface);
        return S_OK;
    }
    ERR("PackageManager: no interface %s\n", debugstr_guid(Iid));
    *Out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Manager_AddRef(IPackageManager *Iface)
{
    return InterlockedIncrement(&ImplFromManager(Iface)->Ref);
}

static ULONG WINAPI
Manager_Release(IPackageManager *Iface)
{
    PACKAGE_MANAGER *Impl = ImplFromManager(Iface);
    ULONG Ref = InterlockedDecrement(&Impl->Ref);

    if (!Ref)
        free(Impl);
    return Ref;
}

static HRESULT WINAPI
Manager_GetIids(IPackageManager *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
Manager_GetRuntimeClassName(IPackageManager *Iface, HSTRING *Name)
{
    return WindowsCreateString(RuntimeClass_Windows_Management_Deployment_PackageManager,
                               ARRAY_SIZE(RuntimeClass_Windows_Management_Deployment_PackageManager) - 1, Name);
}

static HRESULT WINAPI
Manager_GetTrustLevel(IPackageManager *Iface, TrustLevel *Level)
{
    return GetTrustBase(Level);
}

static HRESULT WINAPI
Manager_AddPackageAsync(IPackageManager *Iface, IUriRuntimeClass *Uri, IIterable_Uri *Dependencies,
                        DeploymentOptions Options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **Operation)
{
    FIXME("PackageManager: AddPackageAsync\n");
    *Operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI
Manager_UpdatePackageAsync(IPackageManager *Iface, IUriRuntimeClass *Uri, IIterable_Uri *Dependencies,
                           DeploymentOptions Options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **Operation)
{
    FIXME("PackageManager: UpdatePackageAsync\n");
    *Operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI
Manager_RemovePackageAsync(IPackageManager *Iface, HSTRING Name,
                           IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **Operation)
{
    FIXME("PackageManager: RemovePackageAsync %s\n", debugstr_hstring(Name));
    *Operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI
Manager_StagePackageAsync(IPackageManager *Iface, IUriRuntimeClass *Uri, IIterable_Uri *Dependencies,
                          IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **Operation)
{
    FIXME("PackageManager: StagePackageAsync\n");
    *Operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI
Manager_RegisterPackageAsync(IPackageManager *Iface, IUriRuntimeClass *Uri, IIterable_Uri *Dependencies,
                             DeploymentOptions Options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **Operation)
{
    FIXME("PackageManager: RegisterPackageAsync\n");
    *Operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI
Manager_FindPackages(IPackageManager *Iface, IIterable_Package **Packages)
{
    FIXME("PackageManager: FindPackages\n");
    return CreateEmptyIterable(Packages);
}

static HRESULT WINAPI
Manager_FindPackagesByUserSecurityId(IPackageManager *Iface, HSTRING Sid, IIterable_Package **Packages)
{
    FIXME("PackageManager: FindPackagesByUserSecurityId %s\n", debugstr_hstring(Sid));
    return CreateEmptyIterable(Packages);
}

static HRESULT WINAPI
Manager_FindPackagesByNamePublisher(IPackageManager *Iface, HSTRING Name, HSTRING Publisher, IIterable_Package **Packages)
{
    FIXME("PackageManager: FindPackagesByNamePublisher %s %s\n", debugstr_hstring(Name), debugstr_hstring(Publisher));
    return CreateEmptyIterable(Packages);
}

static HRESULT WINAPI
Manager_FindPackagesByUserSecurityIdNamePublisher(IPackageManager *Iface, HSTRING Sid, HSTRING Name, HSTRING Publisher,
                                                  IIterable_Package **Packages)
{
    FIXME("PackageManager: FindPackagesByUserSecurityIdNamePublisher %s %s %s\n",
          debugstr_hstring(Sid), debugstr_hstring(Name), debugstr_hstring(Publisher));
    return CreateEmptyIterable(Packages);
}

static HRESULT WINAPI
Manager_FindUsers(IPackageManager *Iface, HSTRING Name, IIterable_PackageUserInformation **Users)
{
    FIXME("PackageManager: FindUsers %s\n", debugstr_hstring(Name));
    *Users = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI
Manager_SetPackageState(IPackageManager *Iface, HSTRING Name, PackageState State)
{
    FIXME("PackageManager: SetPackageState %s\n", debugstr_hstring(Name));
    return E_NOTIMPL;
}

static HRESULT WINAPI
Manager_FindPackageByPackageFullName(IPackageManager *Iface, HSTRING Name, IPackage **Package)
{
    FIXME("PackageManager: FindPackageByPackageFullName %s\n", debugstr_hstring(Name));
    *Package = NULL;
    return S_OK;
}

static HRESULT WINAPI
Manager_CleanupPackageForUserAsync(IPackageManager *Iface, HSTRING Name, HSTRING Sid,
                                   IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **Operation)
{
    FIXME("PackageManager: CleanupPackageForUserAsync %s\n", debugstr_hstring(Name));
    *Operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI
Manager_FindPackagesByPackageFamilyName(IPackageManager *Iface, HSTRING FamilyName, IIterable_Package **Packages)
{
    FIXME("PackageManager: FindPackagesByPackageFamilyName %s\n", debugstr_hstring(FamilyName));
    return CreateEmptyIterable(Packages);
}

static HRESULT WINAPI
Manager_FindPackagesByUserSecurityIdPackageFamilyName(IPackageManager *Iface, HSTRING Sid, HSTRING FamilyName,
                                                      IIterable_Package **Packages)
{
    FIXME("PackageManager: FindPackagesByUserSecurityIdPackageFamilyName %s %s\n",
          debugstr_hstring(Sid), debugstr_hstring(FamilyName));
    return CreateEmptyIterable(Packages);
}

static HRESULT WINAPI
Manager_FindPackageByUserSecurityIdPackageFullName(IPackageManager *Iface, HSTRING Sid, HSTRING Name, IPackage **Package)
{
    FIXME("PackageManager: FindPackageByUserSecurityIdPackageFullName %s %s\n",
          debugstr_hstring(Sid), debugstr_hstring(Name));
    *Package = NULL;
    return S_OK;
}

static const IPackageManagerVtbl ManagerVtbl =
{
    Manager_QueryInterface,
    Manager_AddRef,
    Manager_Release,
    Manager_GetIids,
    Manager_GetRuntimeClassName,
    Manager_GetTrustLevel,
    Manager_AddPackageAsync,
    Manager_UpdatePackageAsync,
    Manager_RemovePackageAsync,
    Manager_StagePackageAsync,
    Manager_RegisterPackageAsync,
    Manager_FindPackages,
    Manager_FindPackagesByUserSecurityId,
    Manager_FindPackagesByNamePublisher,
    Manager_FindPackagesByUserSecurityIdNamePublisher,
    Manager_FindUsers,
    Manager_SetPackageState,
    Manager_FindPackageByPackageFullName,
    Manager_CleanupPackageForUserAsync,
    Manager_FindPackagesByPackageFamilyName,
    Manager_FindPackagesByUserSecurityIdPackageFamilyName,
    Manager_FindPackageByUserSecurityIdPackageFullName,
};

static HRESULT WINAPI
Factory_QueryInterface(IActivationFactory *Iface, REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IActivationFactory))
    {
        *Out = Iface;
        return S_OK;
    }
    ERR("PackageManager factory: no interface %s\n", debugstr_guid(Iid));
    *Out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Factory_AddRef(IActivationFactory *Iface)
{
    return 2;
}

static ULONG WINAPI
Factory_Release(IActivationFactory *Iface)
{
    return 1;
}

static HRESULT WINAPI
Factory_GetIids(IActivationFactory *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
Factory_GetRuntimeClassName(IActivationFactory *Iface, HSTRING *Name)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Factory_GetTrustLevel(IActivationFactory *Iface, TrustLevel *Level)
{
    return GetTrustBase(Level);
}

static HRESULT WINAPI
Factory_ActivateInstance(IActivationFactory *Iface, IInspectable **Instance)
{
    PACKAGE_MANAGER *Impl;

    FIXME("PackageManager: ActivateInstance\n");
    *Instance = NULL;
    if (!(Impl = calloc(1, sizeof(*Impl))))
        return E_OUTOFMEMORY;
    Impl->IPackageManager_iface.lpVtbl = &ManagerVtbl;
    Impl->Ref = 1;
    *Instance = (IInspectable *)&Impl->IPackageManager_iface;
    return S_OK;
}

static const IActivationFactoryVtbl FactoryVtbl =
{
    Factory_QueryInterface,
    Factory_AddRef,
    Factory_Release,
    Factory_GetIids,
    Factory_GetRuntimeClassName,
    Factory_GetTrustLevel,
    Factory_ActivateInstance,
};

static IActivationFactory ManagerFactory = { &FactoryVtbl };

HRESULT WINAPI
DllGetClassObject(REFCLSID Clsid, REFIID Iid, void **Out)
{
    *Out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI
DllGetActivationFactory(HSTRING ClassId, IActivationFactory **Factory)
{
    const WCHAR *Name = WindowsGetStringRawBuffer(ClassId, NULL);

    *Factory = NULL;
    if (wcscmp(Name, RuntimeClass_Windows_Management_Deployment_PackageManager))
    {
        ERR("no class %s\n", debugstr_hstring(ClassId));
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    *Factory = &ManagerFactory;
    return S_OK;
}

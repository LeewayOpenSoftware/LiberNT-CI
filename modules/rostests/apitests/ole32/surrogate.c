/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests activation of a DLL server outside the caller's process
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS

#include <wine/test.h>
#include <windef.h>
#include <winbase.h>
#include <objbase.h>

static const CLSID CLSID_TestServer = { 0xee09b103, 0x97e0, 0x11cf, { 0x97, 0x8f, 0x00, 0xa0, 0x24, 0x63, 0xe0, 0x6f } };

static void TestWithoutSurrogate(void)
{
    IClassFactory *ClassFactory = (IClassFactory *)(ULONG_PTR)0xdeadbeef;
    IUnknown *Unknown = (IUnknown *)(ULONG_PTR)0xdeadbeef;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_TestServer, NULL, CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)&Unknown);
    ok(hr == REGDB_E_CLASSNOTREG, "CoCreateInstance without a surrogate returned %#lx\n", hr);
    ok(Unknown == NULL, "Unknown is %p\n", Unknown);

    hr = CoGetClassObject(&CLSID_TestServer, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&ClassFactory);
    ok(hr == REGDB_E_CLASSNOTREG, "CoGetClassObject without a surrogate returned %#lx\n", hr);
    ok(ClassFactory == NULL, "ClassFactory is %p\n", ClassFactory);
}

START_TEST(surrogate)
{
    IUnknown *Unknown;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(hr == S_OK, "CoInitializeEx failed, hr %#lx\n", hr);

    hr = CoCreateInstance(&CLSID_TestServer, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&Unknown);
    if (hr != S_OK)
    {
        skip("The test class cannot be created in process, hr %#lx\n", hr);
        CoUninitialize();
        return;
    }
    IUnknown_Release(Unknown);

    TestWithoutSurrogate();

    CoUninitialize();
}

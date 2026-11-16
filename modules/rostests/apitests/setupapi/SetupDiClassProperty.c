/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SetupDiGetClassPropertyW / SetupDiSetClassPropertyW store a device
 *              setup-class property in the class registry key and round-trip it.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <windows.h>
#include <setupapi.h>
#include <devpropdef.h>

/* A private test property key (random fmtid) so the test never collides with a
   real class property. */
static const DEVPROPKEY TestProp =
{
    { 0x6d9b1f27, 0x4a3c, 0x4e21, { 0x9b, 0x7e, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } },
    7
};

/* GUID_DEVCLASS_SYSTEM: a setup class that is always registered. */
static const GUID SystemClass =
{
    0x4d36e97d, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

START_TEST(SetupDiClassProperty)
{
    DEVPROPTYPE Type = 0;
    DWORD Value = 0xCAFEF00D, Read = 0, Required = 0;
    BOOL Ret;
    HKEY Check;

    /* NULL class GUID -> ERROR_INVALID_PARAMETER */
    SetLastError(0xdeadbeef);
    Ret = SetupDiGetClassPropertyW(NULL, &TestProp, &Type, (PBYTE)&Read, sizeof(Read), &Required, DICLASSPROP_INSTALLER);
    ok(!Ret, "expected failure for a NULL class GUID\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got %lu, expected %u\n", GetLastError(), ERROR_INVALID_PARAMETER);

    /* Bad flags (neither installer nor interface) -> ERROR_INVALID_FLAGS */
    SetLastError(0xdeadbeef);
    Ret = SetupDiGetClassPropertyW(&SystemClass, &TestProp, &Type, (PBYTE)&Read, sizeof(Read), &Required, 0);
    ok(!Ret, "expected failure for bad flags\n");
    ok(GetLastError() == ERROR_INVALID_FLAGS, "got %lu, expected %u\n", GetLastError(), ERROR_INVALID_FLAGS);

    /* The system setup class must be registered for the round-trip. */
    Check = SetupDiOpenClassRegKeyExW(&SystemClass, KEY_QUERY_VALUE, DIOCR_INSTALLER, NULL, NULL);
    if (Check == INVALID_HANDLE_VALUE)
    {
        skip("the system setup class key is not present (%lu)\n", GetLastError());
        return;
    }
    RegCloseKey(Check);

    /* Writing a class property needs administrator rights; skip cleanly otherwise. */
    Ret = SetupDiSetClassPropertyW(&SystemClass, &TestProp, DEVPROP_TYPE_UINT32,
                                   (const BYTE *)&Value, sizeof(Value), DICLASSPROP_INSTALLER);
    if (!Ret && GetLastError() == ERROR_ACCESS_DENIED)
    {
        skip("not elevated; cannot write a class property\n");
        return;
    }
    ok(Ret, "SetupDiSetClassPropertyW failed %lu\n", GetLastError());

    /* Read it back: type, value and required size. */
    Type = 0;
    Read = 0;
    Required = 0;
    Ret = SetupDiGetClassPropertyW(&SystemClass, &TestProp, &Type, (PBYTE)&Read, sizeof(Read), &Required, DICLASSPROP_INSTALLER);
    ok(Ret, "SetupDiGetClassPropertyW failed %lu\n", GetLastError());
    ok(Type == DEVPROP_TYPE_UINT32, "got type %lu, expected %u\n", Type, DEVPROP_TYPE_UINT32);
    ok(Read == Value, "got value 0x%lX, expected 0x%lX\n", Read, Value);
    ok(Required == sizeof(Value), "got required %lu, expected %u\n", Required, (UINT)sizeof(Value));

    /* A NULL buffer reports the required size and ERROR_INSUFFICIENT_BUFFER. */
    SetLastError(0xdeadbeef);
    Required = 0;
    Ret = SetupDiGetClassPropertyW(&SystemClass, &TestProp, &Type, NULL, 0, &Required, DICLASSPROP_INSTALLER);
    ok(!Ret, "expected ERROR_INSUFFICIENT_BUFFER with a NULL buffer\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got %lu, expected %u\n", GetLastError(), ERROR_INSUFFICIENT_BUFFER);
    ok(Required == sizeof(Value), "got required %lu, expected %u\n", Required, (UINT)sizeof(Value));

    /* DEVPROP_TYPE_EMPTY deletes the property. */
    Ret = SetupDiSetClassPropertyW(&SystemClass, &TestProp, DEVPROP_TYPE_EMPTY, NULL, 0, DICLASSPROP_INSTALLER);
    ok(Ret, "SetupDiSetClassPropertyW (delete) failed %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    Read = 0;
    Ret = SetupDiGetClassPropertyW(&SystemClass, &TestProp, &Type, (PBYTE)&Read, sizeof(Read), &Required, DICLASSPROP_INSTALLER);
    ok(!Ret, "expected failure after the property was deleted\n");
    ok(GetLastError() == ERROR_NOT_FOUND, "got %lu, expected %u\n", GetLastError(), ERROR_NOT_FOUND);
}

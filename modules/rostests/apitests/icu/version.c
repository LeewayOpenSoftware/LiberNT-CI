/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The unversioned ICU C API of the full icu.dll, as System.Globalization.Native looks it up
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <apitest.h>
#include <stdint.h>

typedef void (__cdecl *PFN_U_GETVERSION)(uint8_t *);

/* A sample of the names .NET resolves with GetProcAddress before it accepts icu.dll */
static const char *const NetNames[] =
{
    "u_charsToUChars", "u_getVersion", "u_strlen", "ubrk_open", "ucal_open", "ucol_open",
    "ucol_strcoll", "udat_open", "uidna_openUTS46", "uldn_open", "uloc_getDefault",
    "uloc_getName", "unorm2_getNFCInstance", "unum_open", "usearch_openFromCollator",
};

START_TEST(version)
{
    HMODULE Icu;
    PFN_U_GETVERSION pu_getVersion;
    uint8_t Version[4] = { 0 };
    char Name[32];
    unsigned i;

    Icu = LoadLibraryA("icu.dll");
    ok(Icu != NULL, "LoadLibrary(icu.dll) failed: %lu\n", GetLastError());
    if (!Icu)
        return;

    pu_getVersion = (PFN_U_GETVERSION)GetProcAddress(Icu, "u_getVersion");
    if (!pu_getVersion)
    {
        skip("icu.dll is the stub (no u_getVersion): the full ICU is installed by the second stage\n");
        FreeLibrary(Icu);
        return;
    }

    pu_getVersion(Version);
    ok(Version[0] >= 72, "u_getVersion major = %u, expected 72 or later\n", Version[0]);

    for (i = 0; i < ARRAYSIZE(NetNames); i++)
        ok(GetProcAddress(Icu, NetNames[i]) != NULL, "icu.dll lacks %s\n", NetNames[i]);

    /* Windows exports the unversioned names only; a renamed build would carry u_getVersion_<major> */
    sprintf(Name, "u_getVersion_%u", Version[0]);
    ok(GetProcAddress(Icu, Name) == NULL, "icu.dll exports the versioned %s\n", Name);

    FreeLibrary(Icu);
}

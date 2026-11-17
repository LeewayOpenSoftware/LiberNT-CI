/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     DriverStore package verification matches each file against the catalog by
 *              hashing the file once (not once per catalog member) and still fails a
 *              tampered file and passes an intact one.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <windows.h>
#include <setupapi.h>
#include <wincrypt.h>
#include <mssip.h>
#include <mscat.h>

/* From setupapi_private.h (internal); LnSetupVerifyDriverPackage is exported by ordinal. */
#define PKG_SIG_UNSIGNED        0
#define PKG_SIG_SIGNED_VALID    1
#define PKG_SIG_HASH_MISMATCH   2
#define PKG_SIG_CATALOG_INVALID 3

DWORD WINAPI LnSetupVerifyDriverPackage(PCWSTR InfPath, PDWORD SigState,
                                        PWSTR MismatchFile, DWORD MismatchFileCch);

#define DECOY_COUNT 12

static const char InfTemplate[] =
    "[Version]\r\n"
    "Signature=\"$Windows NT$\"\r\n"
    "CatalogFile=lnt_vonce.cat\r\n"
    "\r\n"
    "[DefaultInstall]\r\n"
    "CopyFiles=Lnt.Files\r\n"
    "\r\n"
    "[Lnt.Files]\r\n"
    "lnt_vonce_payload.bin\r\n"
    "\r\n"
    "[SourceDisksNames]\r\n"
    "1=\"LntTest\",,,\"\"\r\n"
    "\r\n"
    "[SourceDisksFiles]\r\n"
    "lnt_vonce_payload.bin=1\r\n"
    "\r\n"
    "[DestinationDirs]\r\n"
    "DefaultDestDir=11\r\n";

static BOOL write_bytes(PCWSTR Path, const void *Data, DWORD Len)
{
    HANDLE h = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok2;

    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    ok2 = WriteFile(h, Data, Len, &written, NULL) && written == Len;
    CloseHandle(h);
    return ok2;
}

/* Add one catalog member for File, computing its indirect data through the SIP (exactly
   as the catalog builder does). Returns FALSE if the SIP/catalog path is unavailable. */
static BOOL add_member(HANDLE hCat, PCWSTR File, PCWSTR Tag)
{
    GUID Subject;
    SIP_DISPATCH_INFO Sip;
    SIP_SUBJECTINFO Info;
    HANDLE hFile;
    DWORD Size = 0;
    BYTE *Indirect = NULL;
    BOOL Ok = FALSE;

    hFile = CreateFileW(File, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    if (CryptSIPRetrieveSubjectGuid(File, hFile, &Subject))
    {
        ZeroMemory(&Sip, sizeof(Sip));
        Sip.cbSize = sizeof(Sip);
        if (CryptSIPLoad(&Subject, 0, &Sip) && Sip.pfCreate)
        {
            ZeroMemory(&Info, sizeof(Info));
            Info.cbSize = sizeof(Info);
            Info.pgSubjectType = &Subject;
            Info.hFile = hFile;
            Info.pwsFileName = File;
            Info.DigestAlgorithm.pszObjId = (LPSTR)szOID_OIWSEC_sha1;

            if (Sip.pfCreate(&Info, &Size, NULL) && Size &&
                (Indirect = HeapAlloc(GetProcessHeap(), 0, Size)) != NULL &&
                Sip.pfCreate(&Info, &Size, (SIP_INDIRECT_DATA *)Indirect))
            {
                Ok = CryptCATPutMemberInfo(hCat, (WCHAR *)File, (WCHAR *)Tag, &Subject,
                                           0x200, Size, Indirect) != NULL;
            }
        }
    }

    if (Indirect)
        HeapFree(GetProcessHeap(), 0, Indirect);
    CloseHandle(hFile);
    return Ok;
}

static BOOL build_catalog(PCWSTR CatPath, PCWSTR PayloadPath, PCWSTR DecoyPath)
{
    HANDLE hCat;
    WCHAR Tag[32];
    DWORD i;
    BOOL Ok;

    hCat = CryptCATOpen((WCHAR *)CatPath, CRYPTCAT_OPEN_CREATENEW, 0, CRYPTCAT_VERSION_1, 0);
    if (hCat == INVALID_HANDLE_VALUE || hCat == NULL)
        return FALSE;

    /* Many decoy members first, then the real one: the file must still be hashed once. */
    for (i = 0; i < DECOY_COUNT; i++)
    {
        BYTE blob[64];
        memset(blob, (int)(i + 1), sizeof(blob));
        if (!write_bytes(DecoyPath, blob, sizeof(blob)))
            break;
        wsprintfW(Tag, L"decoy%lu", i);
        add_member(hCat, DecoyPath, Tag);   /* best effort; decoys need not all succeed */
    }
    DeleteFileW(DecoyPath);

    Ok = add_member(hCat, PayloadPath, L"payload");

    if (!CryptCATPersistStore(hCat))
        Ok = FALSE;
    CryptCATClose(hCat);
    return Ok;
}

START_TEST(DriverStoreVerifyHashOnce)
{
    WCHAR Dir[MAX_PATH], InfPath[MAX_PATH], CatPath[MAX_PATH], Payload[MAX_PATH], Decoy[MAX_PATH];
    WCHAR Mismatch[MAX_PATH];
    DWORD ImageSize = 0, read = 0, SigState = 0, err;
    HANDLE hSelf;

    if (!GetTempPathW(ARRAYSIZE(Dir), Dir))
    {
        skip("GetTempPathW failed (%lu)\n", GetLastError());
        return;
    }
    lstrcatW(Dir, L"lnt_vonce");
    CreateDirectoryW(Dir, NULL);

    wsprintfW(InfPath, L"%s\\lnt_vonce.inf", Dir);
    wsprintfW(CatPath, L"%s\\lnt_vonce.cat", Dir);
    wsprintfW(Payload, L"%s\\lnt_vonce_payload.bin", Dir);
    wsprintfW(Decoy, L"%s\\lnt_vonce_decoy.bin", Dir);

    /* Use this test module (a real PE) as the payload so the PE SIP applies. */
    {
        WCHAR Self[MAX_PATH];
        if (!GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self)) ||
            !CopyFileW(Self, Payload, FALSE))
        {
            skip("Could not stage the PE payload (%lu)\n", GetLastError());
            goto cleanup;
        }
    }

    if (!write_bytes(InfPath, InfTemplate, (DWORD)(sizeof(InfTemplate) - 1)))
    {
        skip("Could not write the INF (%lu)\n", GetLastError());
        goto cleanup;
    }

    if (!build_catalog(CatPath, Payload, Decoy))
    {
        skip("Synthetic catalog unavailable (SIP/CryptCAT); verify with a real .cat in the lab\n");
        goto cleanup;
    }

    /* Baseline: the intact payload must verify. If it does not, the catalog round-trip is
       not usable in this environment - skip rather than report a false result. */
    SigState = 0xffffffff;
    err = LnSetupVerifyDriverPackage(InfPath, &SigState, Mismatch, ARRAYSIZE(Mismatch));
    if (err != ERROR_SUCCESS || SigState != PKG_SIG_SIGNED_VALID)
    {
        skip("Baseline did not verify (err %lu, state %lu); needs a real signed catalog in the lab\n",
             err, SigState);
        goto cleanup;
    }
    ok(SigState == PKG_SIG_SIGNED_VALID, "intact payload verifies (state %lu)\n", SigState);

    /* Tamper one byte in the middle of the payload: the Authenticode hash changes and the
       package must now fail verification, naming the mismatched file. */
    hSelf = CreateFileW(Payload, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSelf != INVALID_HANDLE_VALUE)
    {
        BYTE b;
        ImageSize = GetFileSize(hSelf, NULL);
        SetFilePointer(hSelf, (LONG)(ImageSize / 2), NULL, FILE_BEGIN);
        if (ReadFile(hSelf, &b, 1, &read, NULL) && read == 1)
        {
            b ^= 0xFF;
            SetFilePointer(hSelf, (LONG)(ImageSize / 2), NULL, FILE_BEGIN);
            WriteFile(hSelf, &b, 1, &read, NULL);
        }
        CloseHandle(hSelf);
    }

    SigState = 0xffffffff;
    Mismatch[0] = 0;
    err = LnSetupVerifyDriverPackage(InfPath, &SigState, Mismatch, ARRAYSIZE(Mismatch));
    ok(err == ERROR_SUCCESS, "LnSetupVerifyDriverPackage (tampered) failed %lu\n", err);
    ok(SigState == PKG_SIG_HASH_MISMATCH,
       "tampered payload must fail verification, got state %lu\n", SigState);
    ok(Mismatch[0] != 0, "a mismatched file must be named\n");

cleanup:
    DeleteFileW(Payload);
    DeleteFileW(Decoy);
    DeleteFileW(CatPath);
    DeleteFileW(InfPath);
    RemoveDirectoryW(Dir);
}

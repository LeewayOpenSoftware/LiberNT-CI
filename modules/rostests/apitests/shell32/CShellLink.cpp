/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Test for CShellLink
 * PROGRAMMER:      Andreas Maier
 */

#include "shelltest.h"

#define NDEBUG
#include <debug.h>
#include <stdio.h>
#include <shellutils.h>

/* Test IShellLink::SetPath with environment-variables, existing, non-existing, ...*/
typedef struct
{
    PCWSTR pathIn;
    HRESULT hrSetPath;

    /* Test 1 - hrGetPathX = IShellLink::GetPath(pathOutX, ... , flagsX); */
    PCWSTR pathOut1;
    DWORD flags1;
    HRESULT hrGetPath1;
    BOOL expandPathOut1;

    /* Test 2 */
    PCWSTR pathOut2;
    DWORD flags2;
    HRESULT hrGetPath2;
    BOOL expandPathOut2;
} TEST_SHELL_LINK_DEF;

static TEST_SHELL_LINK_DEF linkTestList_WS03[] =
{
    {
        L"%comspec%",                                                 S_OK,
        L"%comspec%",                                 SLGP_SHORTPATH, S_OK, TRUE,
        L"%comspec%",                                 SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"%anyvar%",                                                  E_INVALIDARG,
        L"",                                          SLGP_SHORTPATH, S_FALSE, FALSE,
        L"",                                          SLGP_RAWPATH,   S_FALSE, FALSE
    },
    {
        L"%anyvar%%comspec%",                                         S_OK,
        L"c:\\%anyvar%%comspec%",                     SLGP_SHORTPATH, S_OK, TRUE,
        L"%anyvar%%comspec%",                         SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"%temp%",                                                    S_OK,
        L"%temp%",                                    SLGP_SHORTPATH, S_OK, TRUE,
        L"%temp%",                                    SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"%shell%",                                                   S_OK,
        L"%systemroot%\\system32\\%shell%",           SLGP_SHORTPATH, S_OK, TRUE,
        L"%shell%",                                   SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"u:\\anypath\\%anyvar%",                                     S_OK,
        L"u:\\anypath\\%anyvar%",                     SLGP_SHORTPATH, S_OK, TRUE,
        L"u:\\anypath\\%anyvar%",                     SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"c:\\temp",                                                  S_OK,
        L"c:\\temp",                                  SLGP_SHORTPATH, S_OK, FALSE,
        L"c:\\temp",                                  SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"cmd.exe",                                                   S_OK,
        L"%comspec%",                                 SLGP_SHORTPATH, S_OK, TRUE,
        L"%comspec%",                                 SLGP_RAWPATH,   S_OK, TRUE
    },
    {
        L"%systemroot%\\non-existent-file",                           S_OK,
        L"%systemroot%\\non-existent-file",           SLGP_SHORTPATH, S_OK, TRUE,
        L"%systemroot%\\non-existent-file",           SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"c:\\non-existent-path\\non-existent-file",                  S_OK,
        L"c:\\non-existent-path\\non-existent-file",  SLGP_SHORTPATH, S_OK, FALSE,
        L"c:\\non-existent-path\\non-existent-file",  SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"non-existent-file",                                         E_INVALIDARG,
        L"",                                          SLGP_SHORTPATH, S_FALSE, FALSE,
        L"",                                          SLGP_RAWPATH,   S_FALSE, FALSE
    },
};

static TEST_SHELL_LINK_DEF linkTestList_Vista[] =
{
    {
        L"%comspec%",                                                 S_OK,
        L"%comspec%",                                 SLGP_SHORTPATH, S_OK, TRUE,
        L"%comspec%",                                 SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"%anyvar%",                                                  S_OK,
        L"%USERPROFILE%\\Desktop\\%anyvar%",          SLGP_SHORTPATH, S_OK, TRUE,
        L"%USERPROFILE%\\Desktop\\%anyvar%",          SLGP_RAWPATH,   S_OK, TRUE
    },
    {
        L"%anyvar%%comspec%",                                         E_INVALIDARG,
        L"",                                          SLGP_SHORTPATH, S_FALSE, TRUE,
        L"%anyvar%%comspec%",                         SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"%temp%",                                                    S_OK,
        L"%temp%",                                    SLGP_SHORTPATH, S_OK, TRUE,
        L"%temp%",                                    SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"%shell%",                                                   S_OK,
        L"%systemroot%\\system32\\%shell%",           SLGP_SHORTPATH, S_OK, TRUE,
        L"%shell%",                                   SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"u:\\anypath\\%anyvar%",                                     S_OK,
        L"u:\\anypath\\%anyvar%",                     SLGP_SHORTPATH, S_OK, TRUE,
        L"u:\\anypath\\%anyvar%",                     SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"c:\\temp",                                                  S_OK,
        L"c:\\temp",                                  SLGP_SHORTPATH, S_OK, FALSE,
        L"c:\\temp",                                  SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"cmd.exe",                                                   S_OK,
        L"%comspec%",                                 SLGP_SHORTPATH, S_OK,  TRUE,
        L"%comspec%",                                 SLGP_RAWPATH,   S_OK,  TRUE
    },
    {
        L"%systemroot%\\non-existent-file",                           S_OK,
        L"%systemroot%\\non-existent-file",           SLGP_SHORTPATH, S_OK, TRUE,
        L"%systemroot%\\non-existent-file",           SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"c:\\non-existent-path\\non-existent-file",                  S_OK,
        L"c:\\non-existent-path\\non-existent-file",  SLGP_SHORTPATH, S_OK, FALSE,
        L"c:\\non-existent-path\\non-existent-file",  SLGP_RAWPATH,   S_OK, FALSE
    },
    {
        L"non-existent-file",                                         S_OK,
        L"%USERPROFILE%\\Desktop\\non-existent-file", SLGP_SHORTPATH, S_OK, TRUE,
        L"%USERPROFILE%\\Desktop\\non-existent-file", SLGP_RAWPATH,   S_OK, TRUE
    },
};

static
VOID
test_checklinkpath(UINT i, TEST_SHELL_LINK_DEF* testDef)
{
static WCHAR evVar[MAX_PATH];

    HRESULT hr, expectedHr;
    WCHAR wPathOut[MAX_PATH];
    BOOL expandPathOut;
    PCWSTR expectedPathOut;
    CComPtr<IShellLinkW> psl;
    UINT i1;
    DWORD flags;

    hr = CoCreateInstance(CLSID_ShellLink,
                          NULL,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IShellLinkW, &psl));
    ok(hr == S_OK, "CoCreateInstance, hr = 0x%lx\n", hr);
    if (FAILED(hr))
    {
        skip("Could not instantiate CShellLink\n");
        return;
    }

    hr = psl->SetPath(testDef->pathIn);
    ok(hr == testDef->hrSetPath, "IShellLink::SetPath(%d), got hr = 0x%lx, expected 0x%lx\n", i, hr, testDef->hrSetPath);

    expectedPathOut = NULL;
    for (i1 = 0; i1 <= 1; i1++)
    {
        if (i1 == 0) /* Usually SLGP_SHORTPATH */
        {
            flags = testDef->flags1;
            expandPathOut = testDef->expandPathOut1;
            expectedPathOut = testDef->pathOut1;
            expectedHr = testDef->hrGetPath1;
        }
        else // if (i1 == 1) /* Usually SLGP_RAWPATH */
        {
            flags = testDef->flags2;
            expandPathOut = testDef->expandPathOut2;
            expectedPathOut = testDef->pathOut2;
            expectedHr = testDef->hrGetPath2;
        }

        /* Patch some variables */
        if (expandPathOut)
        {
            ExpandEnvironmentStringsW(expectedPathOut, evVar, _countof(evVar));
            DPRINT("** %S **\n",evVar);
            expectedPathOut = evVar;
        }

        hr = psl->GetPath(wPathOut, _countof(wPathOut), NULL, flags);
        ok(hr == expectedHr,
           "IShellLink::GetPath(%d), flags 0x%lx, got hr = 0x%lx, expected 0x%lx\n",
            i, flags, hr, expectedHr);
        ok(_wcsicmp(wPathOut, expectedPathOut) == 0,
           "IShellLink::GetPath(%d), flags 0x%lx, in %S, got %S, expected %S\n",
           i, flags, testDef->pathIn, wPathOut, expectedPathOut);
    }
}

static
VOID
TestShellLink(void)
{
    UINT i, TestListCnt;
    TEST_SHELL_LINK_DEF* Test;

    /* Needed for test */
    SetEnvironmentVariableW(L"shell", L"cmd.exe");

    if (GetNTVersion() <= _WIN32_WINNT_WS03)
    {
        Test = linkTestList_WS03;
        TestListCnt = _countof(linkTestList_WS03);
    }
    else
    {
        Test = linkTestList_Vista;
        TestListCnt = _countof(linkTestList_Vista);
    }

    for (i = 0; i < TestListCnt; ++i)
    {
        DPRINT("IShellLink-Test(%d): %S\n", i, Test[i].pathIn);
        test_checklinkpath(i, &Test[i]);
    }

    SetEnvironmentVariableW(L"shell",NULL);
}

static
VOID
TestDescription(void)
{
    HRESULT hr;
    CComPtr<IShellLinkW> psl;
    WCHAR buffer[64];
    PCWSTR testDescription = L"This is a test description";

    /* Test SetDescription */
    hr = CoCreateInstance(CLSID_ShellLink,
                          NULL,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IShellLinkW, &psl));
    ok(hr == S_OK, "CoCreateInstance, hr = 0x%lx\n", hr);
    if (FAILED(hr))
    {
        skip("Could not instantiate CShellLink\n");
        return;
    }

    memset(buffer, 0x55, sizeof(buffer));
    hr = psl->GetDescription(buffer, RTL_NUMBER_OF(buffer));
    ok(hr == S_OK, "IShellLink::GetDescription returned hr = 0x%lx\n", hr);
    ok(buffer[0] == 0, "buffer[0] = %x\n", buffer[0]);
    ok(buffer[1] == 0x5555, "buffer[1] = %x\n", buffer[1]);

    hr = psl->SetDescription(testDescription);
    ok(hr == S_OK, "IShellLink::SetDescription returned hr = 0x%lx\n", hr);

    memset(buffer, 0x55, sizeof(buffer));
    hr = psl->GetDescription(buffer, RTL_NUMBER_OF(buffer));
    ok(hr == S_OK, "IShellLink::GetDescription returned hr = 0x%lx\n", hr);
    ok(buffer[wcslen(testDescription)] == 0, "buffer[n] = %x\n", buffer[wcslen(testDescription)]);
    ok(buffer[wcslen(testDescription) + 1] == 0x5555, "buffer[n+1] = %x\n", buffer[wcslen(testDescription) + 1]);
    ok(!wcscmp(buffer, testDescription), "buffer = '%ls'\n", buffer);

    hr = psl->SetDescription(NULL);
    ok(hr == S_OK, "IShellLink::SetDescription returned hr = 0x%lx\n", hr);

    memset(buffer, 0x55, sizeof(buffer));
    hr = psl->GetDescription(buffer, RTL_NUMBER_OF(buffer));
    ok(hr == S_OK, "IShellLink::GetDescription returned hr = 0x%lx\n", hr);
    ok(buffer[0] == 0, "buffer[0] = %x\n", buffer[0]);
    ok(buffer[1] == 0x5555, "buffer[1] = %x\n", buffer[1]);
}


/* Test IShellLink::Get/SetIconLocation and IExtractIcon::GetIconLocation */
typedef struct
{
    PCWSTR FilePath;

    /* Expected results */
    HRESULT hrDefIcon;  // Return value for GIL_DEFAULTICON
    HRESULT hrForShrt;  // Return value for GIL_FORSHORTCUT
    /* Return values for GIL_FORSHELL */
    HRESULT hrForShell;
    PCWSTR  IconPath;
    UINT    Flags;
} TEST_SHELL_ICON;

static TEST_SHELL_ICON ShIconTests[] =
{
    /* Executable with icons */
    {L"%SystemRoot%\\system32\\cmd.exe", S_FALSE, E_INVALIDARG,
     S_OK, L"%SystemRoot%\\system32\\cmd.exe", GIL_NOTFILENAME | GIL_PERINSTANCE},

    /* Executable without icon */
    {L"%SystemRoot%\\system32\\autochk.exe", S_FALSE, E_INVALIDARG,
     S_OK, L"%SystemRoot%\\system32\\autochk.exe", GIL_NOTFILENAME | GIL_PERINSTANCE},

    /* Existing file */
    {L"%SystemRoot%\\system32\\shell32.dll", S_FALSE, E_INVALIDARG,
     S_OK, L"*", GIL_NOTFILENAME | GIL_PERCLASS},

    /* Non-existing files */
    {L"%SystemRoot%\\non-existent-file.sdf", S_FALSE, E_INVALIDARG,
     S_OK, L"*", GIL_NOTFILENAME | GIL_PERCLASS},
    {L"c:\\non-existent-path\\non-existent-file.sdf", S_FALSE, E_INVALIDARG,
     S_OK, L"*", GIL_NOTFILENAME | GIL_PERCLASS},
};

static
VOID
test_iconlocation(UINT i, TEST_SHELL_ICON* testDef)
{
    HRESULT hr;
    CComPtr<IShellLinkW> psl;
    CComPtr<IExtractIconW> pei;
    INT iIcon;
    UINT wFlags;
    PCWSTR pszExplorer = L"%SystemRoot%\\explorer.exe";
    WCHAR szPath[MAX_PATH];
    WCHAR szPath2[MAX_PATH];

    hr = CoCreateInstance(CLSID_ShellLink,
                          NULL,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IShellLinkW, &psl));
    ok(hr == S_OK, "CoCreateInstance, hr = 0x%lx\n", hr);
    if (FAILED(hr))
    {
        skip("Could not instantiate CShellLink\n");
        return;
    }

    /* Set the path to a file */
    ExpandEnvironmentStringsW(testDef->FilePath, szPath, _countof(szPath));
    hr = psl->SetPath(szPath);
    ok(hr == S_OK, "IShellLink::SetPath failed, hr = 0x%lx\n", hr);

    /*
     * This test shows that this does not imply that the icon is automatically
     * set and be retrieved naively by a call to IShellLink::GetIconLocation.
     */
    iIcon = 0xdeadbeef;
    wcscpy(szPath, L"garbage");
    hr = psl->GetIconLocation(szPath, _countof(szPath), &iIcon);
    ok(hr == S_OK, "IShellLink::GetIconLocation(%d) failed, hr = 0x%lx\n", i, hr);
    ok(*szPath == L'\0', "IShellLink::GetIconLocation(%d) returned '%S'\n", i, szPath);
    ok(iIcon == 0, "IShellLink::GetIconLocation(%d) returned %d, expected %d\n", i, iIcon, 0);

    /* Try to grab the IExtractIconW interface */
    hr = psl->QueryInterface(IID_PPV_ARG(IExtractIconW, &pei));
    ok(hr == S_OK, "IShellLink::QueryInterface(IExtractIconW)(%d) failed, hr = 0x%lx\n", i, hr);
    if (!pei)
    {
        win_skip("No IExtractIconW interface\n");
        return;
    }

    iIcon = wFlags = 0xdeadbeef;
    wcscpy(szPath, L"garbage");
    hr = pei->GetIconLocation(GIL_DEFAULTICON, szPath, _countof(szPath), &iIcon, &wFlags);
    ok(hr == testDef->hrDefIcon, "IExtractIcon::GetIconLocation(%d) returned hr = 0x%lx, expected 0x%lx\n", i, hr, testDef->hrDefIcon);
    ok(*szPath == L'\0', "IExtractIcon::GetIconLocation(%d) returned '%S'\n", i, szPath);
    // ok(iIcon == 0, "IExtractIcon::GetIconLocation(%d) returned %d\n", i, iIcon);

    iIcon = wFlags = 0xdeadbeef;
    wcscpy(szPath, L"garbage");
    hr = pei->GetIconLocation(GIL_FORSHORTCUT, szPath, _countof(szPath), &iIcon, &wFlags);
    ok(hr == testDef->hrForShrt, "IExtractIcon::GetIconLocation(%d) returned hr = 0x%lx, expected 0x%lx\n", i, hr, testDef->hrForShrt);
    // Here, both szPath and iIcon are untouched...

    iIcon = wFlags = 0xdeadbeef;
    wcscpy(szPath, L"garbage");
    hr = pei->GetIconLocation(GIL_FORSHELL, szPath, _countof(szPath), &iIcon, &wFlags);
    ok(hr == testDef->hrForShell, "IExtractIcon::GetIconLocation(%d) returned hr = 0x%lx, expected 0x%lx\n", i, hr, testDef->hrForShell);
    ok(wFlags == testDef->Flags, "IExtractIcon::GetIconLocation(%d) returned wFlags = 0x%x, expected 0x%x\n", i, wFlags, testDef->Flags);
    /*
     * Actually, even if wFlags specifies GIL_NOTFILENAME, a correct file name is returned
     * for executables only (at least...), otherwise we can get an asterix '*'.
     */
    ExpandEnvironmentStringsW(testDef->IconPath, szPath2, _countof(szPath2));
    ok(_wcsicmp(szPath2, szPath) == 0, "IExtractIcon::GetIconLocation(%d) returned '%S', expected '%S'\n", i, szPath, szPath2);

    // ok(*szPath == L'\0', "IExtractIcon::GetIconLocation returned '%S'\n", szPath);
    // ok(iIcon == 0, "IExtractIcon::GetIconLocation returned %d\n", iIcon);
    // ok(FALSE, "hr = 0x%lx, szPath = '%S', iIcon = %d, wFlags = %d\n", hr, szPath, iIcon, wFlags);


    /*
     * Now we test what happens when we explicitly set an icon to the shortcut.
     * Note that actually, SetIconLocation() does not verify whether the file
     * really exists.
     */
    hr = psl->SetIconLocation(pszExplorer, 1);
    ok(hr == S_OK, "IShellLink::SetIconLocation(%d) failed, hr = 0x%lx\n", i, hr);

    /*
     * First, we call IShellLink::GetIconLocation. We retrieve
     * exactly what we specified with SetIconLocation.
     */
    iIcon = 0xdeadbeef;
    wcscpy(szPath, L"garbage");
    hr = psl->GetIconLocation(szPath, _countof(szPath), &iIcon);
    ok(hr == S_OK, "IShellLink::GetIconLocation(%d) failed, hr = 0x%lx\n", i, hr);
    ok(wcscmp(szPath, pszExplorer) == 0, "IShellLink::GetIconLocation(%d) returned '%S', expected '%S'\n", i, szPath, pszExplorer);
    ok(iIcon == 1, "IShellLink::GetIconLocation(%d) returned %d, expected %d\n", i, iIcon, 1);

    /*
     * Now we test what happens with IExtractIcon::GetIconLocation.
     * We see that it retrieves the icon of the shortcut's underlying file.
     */
    iIcon = wFlags = 0xdeadbeef;
    wcscpy(szPath, L"garbage");
    hr = pei->GetIconLocation(GIL_DEFAULTICON, szPath, _countof(szPath), &iIcon, &wFlags);
    ok(hr == testDef->hrDefIcon, "IExtractIcon::GetIconLocation(%d) returned hr = 0x%lx, expected 0x%lx\n", i, hr, testDef->hrDefIcon);
    ok(*szPath == L'\0', "IExtractIcon::GetIconLocation(%d) returned '%S'\n", i, szPath);
    // ok(iIcon == 0, "IExtractIcon::GetIconLocation(%d) returned %d\n", i, iIcon);

    iIcon = wFlags = 0xdeadbeef;
    wcscpy(szPath, L"garbage");
    hr = pei->GetIconLocation(GIL_FORSHORTCUT, szPath, _countof(szPath), &iIcon, &wFlags);
    ok(hr == testDef->hrForShrt, "IExtractIcon::GetIconLocation(%d) returned hr = 0x%lx, expected 0x%lx\n", i, hr, testDef->hrForShrt);
    // Here, both szPath and iIcon are untouched...

    iIcon = wFlags = 0xdeadbeef;
    wcscpy(szPath, L"garbage");
    hr = pei->GetIconLocation(GIL_FORSHELL, szPath, _countof(szPath), &iIcon, &wFlags);
    ok(hr == testDef->hrForShell, "IExtractIcon::GetIconLocation(%d) returned hr = 0x%lx, expected 0x%lx\n", i, hr, testDef->hrForShell);
    ok(wFlags == testDef->Flags, "IExtractIcon::GetIconLocation(%d) returned wFlags = 0x%x, expected 0x%x\n", i, wFlags, testDef->Flags);
    /*
     * Actually, even if wFlags specifies GIL_NOTFILENAME, a correct file name is returned
     * for executables only (at least...), otherwise we can get an asterix '*'.
     */
    ExpandEnvironmentStringsW(testDef->IconPath, szPath2, _countof(szPath2));
    ok(_wcsicmp(szPath2, szPath) == 0, "IExtractIcon::GetIconLocation(%d) returned '%S', expected '%S'\n", i, szPath, szPath2);

    // ok(*szPath == L'\0', "IExtractIcon::GetIconLocation returned '%S'\n", szPath);
    // ok(iIcon == 0, "IExtractIcon::GetIconLocation returned %d\n", iIcon);
    // ok(FALSE, "hr = 0x%lx, szPath = '%S', iIcon = %d, wFlags = %d\n", hr, szPath, iIcon, wFlags);
}

static
VOID
TestIconLocation(void)
{
    UINT i;

    for (i = 0; i < _countof(ShIconTests); ++i)
    {
        test_iconlocation(i, &ShIconTests[i]);
    }
}


/* A .lnk with LinkInfo but no IDList: the target is only known by its local path. */
static BOOL WriteLinkInfoOnlyLink(PCWSTR pszLink, PCWSTR pszTarget)
{
    CHAR szTargetA[MAX_PATH];
    BYTE buffer[0x4C + 0x1C + 0x11 + MAX_PATH + 1 + 4];
    BYTE *p = buffer;
    DWORD cbPath, cbInfo, written;
    HANDLE hFile;
    static const GUID CLSID_Link = { 0x00021401, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

    cbPath = WideCharToMultiByte(CP_ACP, 0, pszTarget, -1, szTargetA, sizeof(szTargetA), NULL, NULL);
    if (!cbPath)
        return FALSE;

    /* SHELL_LINK_HEADER: size, CLSID, flags = HasLinkInfo | IsUnicode, attributes, 3 times, size, icon index, SW_SHOWNORMAL, hotkey, reserved */
    ZeroMemory(buffer, sizeof(buffer));
    *(DWORD *)p = 0x4C;
    memcpy(p + 4, &CLSID_Link, sizeof(GUID));
    *(DWORD *)(p + 0x14) = 0x00000002 | 0x00000080;
    *(DWORD *)(p + 0x18) = FILE_ATTRIBUTE_ARCHIVE;
    *(DWORD *)(p + 0x3C) = 1;
    p += 0x4C;

    /* LINK_INFO: total, header 0x1C, flags = VolumeIDAndLocalBasePath, volume at 0x1C, local path after the volume, no network volume, final path = "" */
    cbInfo = 0x1C + 0x11 + cbPath + 1;
    *(DWORD *)(p + 0x00) = cbInfo;
    *(DWORD *)(p + 0x04) = 0x1C;
    *(DWORD *)(p + 0x08) = 1;
    *(DWORD *)(p + 0x0C) = 0x1C;
    *(DWORD *)(p + 0x10) = 0x1C + 0x11;
    *(DWORD *)(p + 0x14) = 0;
    *(DWORD *)(p + 0x18) = 0x1C + 0x11 + cbPath;
    /* VOLUME_ID: size 0x11, DRIVE_FIXED, serial, label at 0x10 = "" */
    *(DWORD *)(p + 0x1C) = 0x11;
    *(DWORD *)(p + 0x20) = DRIVE_FIXED;
    *(DWORD *)(p + 0x24) = 0x12345678;
    *(DWORD *)(p + 0x28) = 0x10;
    p[0x2C] = 0;
    memcpy(p + 0x2D, szTargetA, cbPath);
    p[0x2D + cbPath] = 0;
    p += cbInfo;

    /* EXTRA_DATA: the terminal block (size below 4) */
    *(DWORD *)p = 0;
    p += 4;

    hFile = CreateFileW(pszLink, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    WriteFile(hFile, buffer, (DWORD)(p - buffer), &written, NULL);
    CloseHandle(hFile);
    return written == (DWORD)(p - buffer);
}

static void TestLinkInfoOnlyIcon(void)
{
    WCHAR szTarget[MAX_PATH], szLink[MAX_PATH], szPath[MAX_PATH];
    CComPtr<IShellLinkW> psl;
    CComPtr<IPersistFile> ppf;
    CComPtr<IExtractIconW> pei;
    SHFILEINFOW sfi;
    INT iIcon = -1;
    UINT wFlags = 0;
    HRESULT hr;

    ExpandEnvironmentStringsW(L"%SystemRoot%\\system32\\cmd.exe", szTarget, _countof(szTarget));
    GetTempPathW(_countof(szLink), szLink);
    wcscat(szLink, L"linkinfo-only-cmd.lnk");
    if (!WriteLinkInfoOnlyLink(szLink, szTarget))
    {
        skip("Cannot write %S\n", szLink);
        return;
    }

    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &psl));
    ok(hr == S_OK, "CoCreateInstance, hr = 0x%lx\n", hr);
    if (SUCCEEDED(hr))
        hr = psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf));
    if (SUCCEEDED(hr))
        hr = ppf->Load(szLink, STGM_READ);
    ok(hr == S_OK, "IPersistFile::Load(LinkInfo-only) hr = 0x%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = psl->GetPath(szPath, _countof(szPath), NULL, SLGP_RAWPATH);
        ok(hr == S_OK && _wcsicmp(szPath, szTarget) == 0, "GetPath hr = 0x%lx, '%S'\n", hr, szPath);

        hr = psl->QueryInterface(IID_PPV_ARG(IExtractIconW, &pei));
        ok(hr == S_OK, "QueryInterface(IExtractIconW) hr = 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            szPath[0] = 0;
            hr = pei->GetIconLocation(GIL_FORSHELL, szPath, _countof(szPath), &iIcon, &wFlags);
            ok(hr == S_OK, "IExtractIcon::GetIconLocation(LinkInfo-only) hr = 0x%lx\n", hr);
            ok(_wcsicmp(szPath, szTarget) == 0, "IExtractIcon::GetIconLocation returned '%S', expected '%S'\n", szPath, szTarget);
        }
    }
    psl.Release();
    ppf.Release();
    pei.Release();

    /* What the shell view asks for */
    ZeroMemory(&sfi, sizeof(sfi));
    ok(SHGetFileInfoW(szLink, 0, &sfi, sizeof(sfi), SHGFI_ICONLOCATION) != 0, "SHGetFileInfo(SHGFI_ICONLOCATION) failed\n");
    ok(_wcsicmp(sfi.szDisplayName, szTarget) == 0, "SHGFI_ICONLOCATION gave '%S', expected '%S'\n", sfi.szDisplayName, szTarget);

    DeleteFileW(szLink);

    /* The same link with a network target: the LinkInfo path is not followed for the icon
     * (no network access for a dropped .lnk), so there is no target icon and no long wait.
     * Windows may behave differently here. */
    {
        PCWSTR pszUnc = L"\\\\nonexistent-host-a1b2c3d4\\share\\cmd.exe";
        DWORD dwStart;

        if (!WriteLinkInfoOnlyLink(szLink, pszUnc))
        {
            skip("Cannot write %S\n", szLink);
            return;
        }
        hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &psl));
        if (SUCCEEDED(hr))
            hr = psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf));
        if (SUCCEEDED(hr))
            hr = ppf->Load(szLink, STGM_READ);
        ok(hr == S_OK, "IPersistFile::Load(UNC LinkInfo-only) hr = 0x%lx\n", hr);
        if (SUCCEEDED(hr) && SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IExtractIconW, &pei))))
        {
            szPath[0] = 0;
            dwStart = GetTickCount();
            hr = pei->GetIconLocation(GIL_FORSHELL, szPath, _countof(szPath), &iIcon, &wFlags);
            ok(FAILED(hr) || _wcsicmp(szPath, pszUnc) != 0, "UNC LinkInfo-only link gave the target icon '%S'\n", szPath);
            ok(GetTickCount() - dwStart < 2000, "UNC LinkInfo-only GetIconLocation took %lu ms\n", GetTickCount() - dwStart);
        }
        psl.Release();
        ppf.Release();
        pei.Release();
        DeleteFileW(szLink);
    }
}

START_TEST(CShellLink)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    TestShellLink();
    TestDescription();
    TestIconLocation();
    TestLinkInfoOnlyIcon();

    CoUninitialize();
}

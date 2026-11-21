/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test Fonts folder (fontext) stacking, columns, and display names
 */

#include "shelltest.h"
#include <debug.h>
#include <wchar.h>
#include <shlobj.h>
#include <shellutils.h>

static void TestFontsFolder(void)
{
    CComPtr<IShellFolder> psfDsk;
    HRESULT hr = SHGetDesktopFolder(&psfDsk);
    ok(hr == S_OK, "Get desktop folder: 0x%lx\n", hr);
    if (hr != S_OK) return;

    LPITEMIDLIST pidlFonts;
    hr = SHGetSpecialFolderLocation(NULL, CSIDL_FONTS, &pidlFonts);
    ok(hr == S_OK, "Get Fonts pidl: 0x%lx\n", hr);
    if (hr != S_OK) return;

    CComPtr<IShellFolder> psf;
    hr = psfDsk->BindToObject(pidlFonts, NULL, IID_PPV_ARG(IShellFolder, &psf));
    ILFree(pidlFonts);
    ok(hr == S_OK, "Bind Fonts folder: 0x%lx\n", hr);
    if (hr != S_OK) return;

    CComPtr<IEnumIDList> penum;
    hr = psf->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &penum);
    ok(hr == S_OK, "EnumObjects: 0x%lx\n", hr);

    LPITEMIDLIST pidl;
    ULONG pceltFetched;
    int itemCount = 0, familyCount = 0;

    while (penum && penum->Next(1, &pidl, &pceltFetched) == S_OK && pceltFetched)
    {
        itemCount++;
        SFGAOF sfgao = SFGAO_FOLDER;
        hr = psf->GetAttributesOf(1, (LPCITEMIDLIST*)&pidl, &sfgao);
        ok(hr == S_OK, "GetAttributesOf: 0x%lx\n", hr);

        if (sfgao & SFGAO_FOLDER)
        {
            familyCount++;
            CComPtr<IShellFolder> psub;
            hr = psf->BindToObject(pidl, NULL, IID_PPV_ARG(IShellFolder, &psub));
            ok(hr == S_OK, "BindToObject family: 0x%lx\n", hr);
            if (hr == S_OK)
            {
                CComPtr<IEnumIDList> psubenum;
                LPITEMIDLIST pidlStyle;
                int styleCount = 0;
                if (SUCCEEDED(psub->EnumObjects(NULL, SHCONTF_NONFOLDERS, &psubenum)))
                {
                    while (psubenum->Next(1, &pidlStyle, &pceltFetched) == S_OK && pceltFetched)
                    {
                        styleCount++;
                        ILFree(pidlStyle);
                    }
                }
                ok(styleCount >= 2, "Family should have at least 2 styles, got %d\n", styleCount);
            }
        }

        STRRET sr;
        hr = psf->GetDisplayNameOf(pidl, SHGDN_INFOLDER, &sr);
        ok(hr == S_OK, "GetDisplayNameOf: 0x%lx\n", hr);
        if (hr == S_OK && sr.uType == STRRET_WSTR)
        {
            ok(!wcsstr(sr.pOleStr, L" (TrueType)") && !wcsstr(sr.pOleStr, L" (OpenType)"),
               "Display name should not have file type suffix: %S\n", sr.pOleStr);
            CoTaskMemFree(sr.pOleStr);
        }

        ILFree(pidl);
    }

    ok(itemCount > 0, "Should enumerate items, got %d\n", itemCount);
    ok(familyCount > 0, "Should have at least one family\n");

    CComPtr<IShellFolder2> psf2;
    hr = psf->QueryInterface(IID_PPV_ARG(IShellFolder2, &psf2));
    if (hr == S_OK)
    {
        SHELLDETAILS sd;
        for (int col = 0; col < 7; col++)
        {
            ZeroMemory(&sd, sizeof(sd));
            hr = psf2->GetDetailsOf(NULL, col, &sd);
            ok(hr == S_OK, "GetDetailsOf col %d: 0x%lx\n", col, hr);
        }
    }
}

START_TEST(FontsFolderExtension)
{
    CoInitialize(NULL); // the Fonts folder handler is a COM class
    TestFontsFolder();
    CoUninitialize();
}

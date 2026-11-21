/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Test for SHSimpleIDListFromPath
 * COPYRIGHT:   Copyright 2024 Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 *              Copyright 2024 Whindmar Saksit <whindsaks@proton.me>
 */

#include "shelltest.h"
#include <shellutils.h>

enum { 
    DIRBIT = 1, FILEBIT = 2,
    PT_COMPUTER_REGITEM = 0x2E,
    PT_INTERNET_URL     = 0x61,
    PT_CONTROLS_OLDREGITEM = 0x70,
    PT_CONTROLS_NEWREGITEM = 0x71,
};

static BYTE GetPIDLType(LPCITEMIDLIST pidl)
{
    // Return the type without the 0x80 flag
    return pidl && pidl->mkid.cb >= 3 ? (pidl->mkid.abID[0] & 0x7F) : 0;
}

struct FS95 // FileSystem item header
{
    WORD cb;
    BYTE type;
    BYTE unknown;
    UINT size;
    WORD date, time;
    WORD att;
    CHAR name[ANYSIZE_ARRAY];

    static BOOL IsFS(LPCITEMIDLIST p)
    {
        return (p && p->mkid.cb > 2) ? (p->mkid.abID[0] & 0x70) == 0x30 : FALSE;
    }
    static FS95* Validate(LPCITEMIDLIST p)
    {
        C_ASSERT(FIELD_OFFSET(FS95, name) == 14);
        return p && p->mkid.cb > FIELD_OFFSET(FS95, name) && IsFS(p) ? (FS95*)p : NULL;
    }
};

static int FileStruct_Att(LPCITEMIDLIST pidl)
{
    C_ASSERT(FIELD_OFFSET(FS95, att) == 12);
    FS95 *p = FS95::Validate(pidl);
    return p ? p->att : (UINT(1) << 31);
}

static HRESULT ParseDisplayName(PWSTR pszPath, LPITEMIDLIST *ppidl, IBindCtx *pBC = NULL)
{
    CComPtr<IShellFolder> pSF;
    HRESULT hr = SHGetDesktopFolder(&pSF);
    return FAILED(hr) ? hr : pSF->ParseDisplayName(NULL, pBC, pszPath, NULL, ppidl, NULL);
}

static HRESULT GetDisplayNameOf(IShellFolder *pSF, LPCITEMIDLIST pidl, UINT Flags, PWSTR Buf, UINT Cap)
{
    STRRET sr;
    HRESULT hr = pSF->GetDisplayNameOf(pidl, Flags, &sr);
    if (SUCCEEDED(hr))
        hr = StrRetToBufW(&sr, pidl, Buf, Cap);
    return hr;
}

static HRESULT GetDisplayNameOf(PCIDLIST_ABSOLUTE pidl, UINT Flags, PWSTR Buf, UINT Cap)
{
    CComPtr<IShellFolder> pSF;
    PCUITEMID_CHILD pidlChild;
    HRESULT hr = SHBindToParent(pidl, IID_PPV_ARG(IShellFolder, &pSF), &pidlChild);
    return FAILED(hr) ? hr : GetDisplayNameOf(pSF, pidlChild, Flags, Buf, Cap);
}

#define TEST_CLSID(pidl, type, offset, clsid) \
    do { \
        ok_long(GetPIDLType(pidl), (type)); \
        ok_int(*(CLSID*)((&pidl->mkid.abID[(offset) - sizeof(WORD)])) == clsid, TRUE); \
    } while (0)

START_TEST(SHSimpleIDListFromPath)
{
    HRESULT hr;
    WCHAR szPath[MAX_PATH];
    GetWindowsDirectoryW(szPath, _countof(szPath));

    // We compare pidl1 and pidl2
    CComHeapPtr<ITEMIDLIST> pidl1(SHSimpleIDListFromPath(szPath));
    CComHeapPtr<ITEMIDLIST> pidl2(ILCreateFromPathW(szPath));

    // Yes, they are equal logically
    LPITEMIDLIST pidl1Last = ILFindLastID(pidl1), pidl2Last = ILFindLastID(pidl2);
    ok_int(ILIsEqual(pidl1, pidl2), TRUE);
    ok_int(ILIsEqual(pidl1Last, pidl2Last), TRUE);

    // Bind to parent
    CComPtr<IShellFolder> psf1, psf2;
    hr = SHBindToParent(pidl1, IID_PPV_ARG(IShellFolder, &psf1), NULL);
    ok_long(hr, S_OK);
    hr = SHBindToParent(pidl2, IID_PPV_ARG(IShellFolder, &psf2), NULL);
    ok_long(hr, S_OK);

    // Get attributes
    DWORD attrs1 = SFGAO_FOLDER, attrs2 = SFGAO_FOLDER;
    hr = (psf1 ? psf1->GetAttributesOf(1, &pidl1Last, &attrs1) : E_UNEXPECTED);
    ok_long(hr, S_OK);
    hr = (psf2 ? psf2->GetAttributesOf(1, &pidl2Last, &attrs2) : E_UNEXPECTED);
    ok_long(hr, S_OK);

    // There is the difference in attributes because SHSimpleIDListFromPath
    // cannot create PIDLs to folders, only files and drives:
    ok_long((attrs1 & SFGAO_FOLDER), 0);
    ok_long((attrs2 & SFGAO_FOLDER), SFGAO_FOLDER);


    // Make sure the internal details match Windows NT5+
    LPITEMIDLIST item;
    GetSystemDirectoryW(szPath, _countof(szPath));
    CComHeapPtr<ITEMIDLIST> pidlSys32(SHSimpleIDListFromPath(szPath));
    if (szPath[1] != ':' || PathFindFileNameW(szPath) <= szPath)
    {
        skip("Not a local directory %ls\n", szPath);
    }
    else if (!(LPITEMIDLIST)pidlSys32)
    {
        skip("?\n");
    }
    else
    {
        item = ILFindLastID(pidlSys32);
        ok_long(item->mkid.abID[0] & 0x73, 0x30 | FILEBIT); // This is actually a file PIDL
        ok_long(FileStruct_Att(item), 0); // Simple PIDL without attributes
        ok_int(*(UINT*)(&item->mkid.abID[2]), 0); // No size

        ILRemoveLastID(pidlSys32); // Now we should have "c:\Windows"
        item = ILFindLastID(pidlSys32);
        ok_long(item->mkid.abID[0] & 0x73, 0x30 | DIRBIT);
        ok_int(*(UINT*)(&item->mkid.abID[2]), 0); // No size
    }

    WCHAR drive[4] = { szPath[0], szPath[1], L'\\', L'\0' };
    CComHeapPtr<ITEMIDLIST> pidlDrive(SHSimpleIDListFromPath(drive));
    if (drive[1] != ':')
    {
        skip("Not a local drive %ls\n", drive);
    }
    else if (!(LPITEMIDLIST)pidlDrive)
    {
        skip("?\n");
    }
    else
    {
        item = ILFindLastID(pidlDrive);
        ok_long(item->mkid.abID[0] & 0x70, 0x20); // Something in My Computer
        ok_char(item->mkid.abID[1] | 32, drive[0] | 32);
    }

    CComHeapPtr<ITEMIDLIST> pidlVirt(SHSimpleIDListFromPath(L"x:\\IDontExist"));
    if (!(LPITEMIDLIST)pidlVirt)
    {
        skip("?\n");
    }
    else
    {
        item = ILFindLastID(pidlVirt);
        ok_long(item->mkid.abID[0] & 0x73, 0x30 | FILEBIT); // Yes, a file
        ok_long(FileStruct_Att(item), 0); // Simple PIDL, no attributes
        ok_int(*(UINT*)(&item->mkid.abID[2]), 0); // No size

        ILRemoveLastID(pidlVirt); // "x:\"
        item = ILFindLastID(pidlVirt);
        ok_long(item->mkid.abID[0] & 0x70, 0x20); // Something in My Computer
        ok_char(item->mkid.abID[1] | 32, 'x' | 32); // x:
    }

    LPITEMIDLIST pidl;
    ok_int((pidl = SHSimpleIDListFromPath(L"c:")) != NULL, LOBYTE(GetVersion()) >= 6);
    ILFree(pidl);
    ok_int((pidl = SHSimpleIDListFromPath(L"c:\\")) != NULL, TRUE);
    ILFree(pidl);
}

START_TEST(ILCreateFromPath)
{
    WCHAR szPath[MAX_PATH];
    LPITEMIDLIST item;

    ok_ptr(ILCreateFromPathW(L"c:\\IDontExist"), NULL);

    GetSystemDirectoryW(szPath, _countof(szPath));
    CComHeapPtr<ITEMIDLIST> pidlDir(ILCreateFromPathW(szPath));
    if (szPath[1] != ':' || PathFindFileNameW(szPath) <= szPath)
    {
        skip("Not a local directory %ls\n", szPath);
    }
    else if (!(LPITEMIDLIST)pidlDir)
    {
        skip("?\n");
    }
    else
    {
        item = ILFindLastID(pidlDir);
        ok_long(item->mkid.abID[0] & 0x73, 0x30 | DIRBIT);
        ok_int(*(UINT*)(&item->mkid.abID[2]), 0); // No size
    }
    PathAppendW(szPath, L"kernel32.dll");
    CComHeapPtr<ITEMIDLIST> pidlFile(ILCreateFromPathW(szPath));
    if (!(LPITEMIDLIST)pidlFile)
    {
        skip("?\n");
    }
    else
    {
        item = ILFindLastID(pidlFile);
        ok_long(item->mkid.abID[0] & 0x73, 0x30 | FILEBIT);
        ok_int(*(UINT*)(&item->mkid.abID[2]) > 1024 * 42, TRUE); // At least this large
    }
}

START_TEST(PIDL)
{
    CCoInit ComInit;
    LPITEMIDLIST pidl;

    pidl = SHCloneSpecialIDList(NULL, CSIDL_DRIVES, FALSE);
    if (pidl)
        TEST_CLSID(ILFindLastID(pidl), 0x1f, 4, CLSID_MyComputer);
    else
        skip("?\n");
    ILFree(pidl);

    pidl = SHCloneSpecialIDList(NULL, CSIDL_PRINTERS, FALSE);
    if (pidl)
    {
        // Accept both the old and new format from the special folder API (NT5 vs NT6)
        LPITEMIDLIST pidlLeaf = ILFindLastID(pidl);
        if (LOBYTE(GetVersion()) < 6)
            TEST_CLSID(pidlLeaf, PT_CONTROLS_OLDREGITEM, 4, CLSID_Printers);
        else
            TEST_CLSID(pidlLeaf, PT_CONTROLS_NEWREGITEM, 14, CLSID_Printers);

        // The Control Panel should always return the new format when parsing
        LPITEMIDLIST pidl2;
        WCHAR szParse[MAX_PATH];
        if (SUCCEEDED(GetDisplayNameOf(pidl, SHGDN_FORPARSING, szParse, _countof(szParse))) &&
            SUCCEEDED(ParseDisplayName(szParse, &pidl2)))
        {
            TEST_CLSID(ILFindLastID(pidl2), PT_CONTROLS_NEWREGITEM, 14, CLSID_Printers);
            ILFree(pidl2);
        }
        else
        {
            skip("Failed to parse in Control Panel\n");
        }
    }
    else
    {
        skip("?\n");
    }
    ILFree(pidl);


    CComPtr<IShellFolder> pInternet;
    HRESULT hr = SHCoCreateInstance(NULL, &CLSID_Internet, NULL, IID_PPV_ARG(IShellFolder, &pInternet));
    if (SUCCEEDED(hr))
    {
        PCWSTR pszUrl = L"http://example.com/page?query&foo=bar";
        PIDLIST_RELATIVE pidlUrl;
        hr = pInternet->ParseDisplayName(NULL, NULL, const_cast<PWSTR>(pszUrl), NULL, &pidlUrl, NULL);
        ok_long(hr, S_OK);
        if (hr == S_OK)
        {
            ok_int(pidlUrl->mkid.abID[0], PT_INTERNET_URL);
            WCHAR buf[MAX_PATH];
            hr = GetDisplayNameOf(pInternet, pidlUrl, SHGDN_FORPARSING, buf, _countof(buf));
            ok_long(hr, S_OK);
            if (SUCCEEDED(hr))
            {
                ok(!lstrcmpiW(buf, pszUrl), "FORPARSING must match URL\n");
            }
            ILFree(pidlUrl);
        }
    }
}

START_TEST(ILIsEqual)
{
    LPITEMIDLIST p1, p2, pidl;

    p1 = p2 = NULL;
    ok_int(ILIsEqual(p1, p2), TRUE);

    ITEMIDLIST emptyitem = {}, emptyitem2 = {};
    ok_int(ILIsEqual(&emptyitem, &emptyitem2), TRUE);

    ok_int(ILIsEqual(NULL, &emptyitem), FALSE); // These two are not equal for some reason

    p1 = SHCloneSpecialIDList(NULL, CSIDL_DRIVES, FALSE);
    p2 = SHCloneSpecialIDList(NULL, CSIDL_DRIVES, FALSE);
    if (p1 && p2)
    {
        ok_int(ILIsEqual(p1, p2), TRUE);
        p1->mkid.abID[0] = PT_COMPUTER_REGITEM; // RegItem in wrong parent
        ok_int(ILIsEqual(p1, p2), FALSE);
    }
    else
    {
        skip("Unable to initialize test\n");
    }
    ILFree(p1);
    ILFree(p2);

    // ILIsParent must compare like ILIsEqual
    p1 = SHSimpleIDListFromPath(L"c:\\");
    p2 = SHSimpleIDListFromPath(L"c:\\dir\\file");
    if (p1 && p2)
    {
        ok_int(ILIsParent(NULL, p1, FALSE), FALSE); // NULL is always false
        ok_int(ILIsParent(p1, NULL, FALSE), FALSE); // NULL is always false
        ok_int(ILIsParent(NULL, NULL, FALSE), FALSE); // NULL is always false
        ok_int(ILIsParent(p1, p1, FALSE), TRUE); // I'm my own parent
        ok_int(ILIsParent(p1, p1, TRUE), FALSE); // Self is not immediate
        ok_int(ILIsParent(p1, p2, FALSE), TRUE); // Grandchild
        ok_int(ILIsParent(p1, p2, TRUE), FALSE); // Grandchild is not immediate
        ok_ptr(ILFindChild(p1, p2), ILGetNext(ILGetNext(p2))); // Child is "dir\\file", skip MyComputer and C:
        ok_int(ILIsEmpty(pidl = ILFindChild(p1, p1)) && pidl, TRUE); // Self
        ILRemoveLastID(p2);
        ok_int(ILIsParent(p1, p2, TRUE), TRUE); // Immediate child

        p1->mkid.abID[0] = PT_COMPUTER_REGITEM; // RegItem in wrong parent
        ok_int(ILIsParent(p1, p2, FALSE), FALSE);
    }
    else
    {
        skip("Unable to initialize test\n");
    }
    ILFree(p1);
    ILFree(p2);
}

/* The ID list Windows 10 wrote into a shortcut to C:\Windows\System32\cmd.exe: each file
 * entry ends with a version 9 BEEF0004 block whose long name follows a file reference. */
static const BYTE g_WindowsCmdIDList[] =
{
    0x14, 0x00, 0x1f, 0x50, 0xe0, 0x4f, 0xd0, 0x20, 0xea, 0x3a, 0x69, 0x10, 0xa2, 0xd8, 0x08, 0x00,
    0x2b, 0x30, 0x30, 0x9d, 0x19, 0x00, 0x2f, 0x43, 0x3a, 0x5c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x00, 0x31,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x74, 0x5d, 0xf4, 0xa2, 0x10, 0x00, 0x57, 0x69, 0x6e, 0x64, 0x6f,
    0x77, 0x73, 0x00, 0x40, 0x00, 0x09, 0x00, 0x04, 0x00, 0xef, 0xbe, 0x6d, 0x5c, 0x75, 0x21, 0x74,
    0x5d, 0x09, 0xbb, 0x2e, 0x00, 0x00, 0x00, 0x0b, 0x25, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x7b, 0xfb,
    0x00, 0x57, 0x00, 0x69, 0x00, 0x6e, 0x00, 0x64, 0x00, 0x6f, 0x00, 0x77, 0x00, 0x73, 0x00, 0x00,
    0x00, 0x16, 0x00, 0x5a, 0x00, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x5d, 0x4e, 0x5c, 0x10,
    0x00, 0x53, 0x79, 0x73, 0x74, 0x65, 0x6d, 0x33, 0x32, 0x00, 0x00, 0x42, 0x00, 0x09, 0x00, 0x04,
    0x00, 0xef, 0xbe, 0x6d, 0x5c, 0x75, 0x21, 0x74, 0x5d, 0xa1, 0xb4, 0x2e, 0x00, 0x00, 0x00, 0x11,
    0x25, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xf4, 0xab, 0x00, 0x53, 0x00, 0x79, 0x00, 0x73, 0x00, 0x74,
    0x00, 0x65, 0x00, 0x6d, 0x00, 0x33, 0x00, 0x32, 0x00, 0x00, 0x00, 0x18, 0x00, 0x56, 0x00, 0x32,
    0x00, 0x00, 0x30, 0x05, 0x00, 0x6e, 0x5d, 0xf9, 0xbe, 0x20, 0x00, 0x63, 0x6d, 0x64, 0x2e, 0x65,
    0x78, 0x65, 0x00, 0x40, 0x00, 0x09, 0x00, 0x04, 0x00, 0xef, 0xbe, 0x6e, 0x5d, 0xf9, 0xbe, 0x74,
    0x5d, 0xe9, 0xb9, 0x2e, 0x00, 0x00, 0x00, 0xdd, 0x2d, 0x48, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe3, 0x9d, 0x10,
    0x01, 0x63, 0x00, 0x6d, 0x00, 0x64, 0x00, 0x2e, 0x00, 0x65, 0x00, 0x78, 0x00, 0x65, 0x00, 0x00,
    0x00, 0x16, 0x00, 0x00, 0x00
};

START_TEST(WindowsFileIDList)
{
    PCIDLIST_ABSOLUTE pidl = (PCIDLIST_ABSOLUTE)g_WindowsCmdIDList;
    WCHAR szPath[MAX_PATH], szName[MAX_PATH];
    HRESULT hr;

    szPath[0] = UNICODE_NULL;
    ok_int(SHGetPathFromIDListW(pidl, szPath), TRUE);
    ok_wstr(szPath, L"C:\\Windows\\System32\\cmd.exe");

    hr = GetDisplayNameOf(pidl, SHGDN_INFOLDER | SHGDN_FORPARSING, szName, _countof(szName));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
        ok_wstr(szName, L"cmd.exe");

    /* A damaged long name in the last entry must not be read: the primary name "cmd.exe"
     * is used instead, so a long name read past its block would show up in the path. */
    for (UINT Case = 0; Case < 3; ++Case)
    {
        BYTE Buf[sizeof(g_WindowsCmdIDList)];
        const UINT cbLast = 86, cbItemHead = 22, cbBlockName = 46;
        const UINT LastItem = sizeof(Buf) - sizeof(WORD) - cbLast;
        BYTE *pBlock = Buf + LastItem + cbItemHead;
        UINT cbTotal = sizeof(Buf);

        memcpy(Buf, g_WindowsCmdIDList, sizeof(Buf));
        if (Case == 0) /* a version we do not know */
        {
            pBlock[2] = 10;
        }
        else if (Case == 1) /* the long name loses its terminator: "cmd.exeX" fills the block */
        {
            pBlock[cbBlockName + 14] = 'X';
        }
        else /* the block ends two characters into the long name */
        {
            const WORD cbBlock = cbBlockName + 2 * sizeof(WCHAR) + sizeof(WORD);
            const WORD cbItem = cbItemHead + cbBlock;

            *(WORD *)pBlock = cbBlock;
            *(WORD *)(pBlock + cbBlock - sizeof(WORD)) = cbItemHead;
            *(WORD *)(Buf + LastItem) = cbItem;
            *(WORD *)(Buf + LastItem + cbItem) = 0;
            cbTotal = LastItem + cbItem + sizeof(WORD);
        }
        ok(ILGetSize((PCIDLIST_ABSOLUTE)Buf) == cbTotal, "case %u: size %u\n", Case, ILGetSize((PCIDLIST_ABSOLUTE)Buf));
        szPath[0] = UNICODE_NULL;
        ok(SHGetPathFromIDListW((PCIDLIST_ABSOLUTE)Buf, szPath), "case %u: no path\n", Case);
        ok(!wcscmp(szPath, L"C:\\Windows\\System32\\cmd.exe"), "case %u: %ls\n", Case, szPath);
    }
}

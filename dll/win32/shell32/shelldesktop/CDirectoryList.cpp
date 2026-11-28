/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Shell change notification
 * COPYRIGHT:   Copyright 2020 Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 */
#include "shelldesktop.h"
#include "CDirectoryList.h"
#include <assert.h>      // for assert

WINE_DEFAULT_DEBUG_CHANNEL(shcn);

BOOL CDirectoryList::ContainsPath(LPCWSTR pszPath) const
{
    assert(!PathIsRelativeW(pszPath));

    for (INT i = 0; i < m_items.GetSize(); ++i)
    {
        if (m_items[i].IsEmpty())
            continue;

        if (m_items[i].EqualPath(pszPath))
            return TRUE; // matched
    }
    return FALSE;
}

BOOL CDirectoryList::AddPath(LPCWSTR pszPath)
{
    assert(!PathIsRelativeW(pszPath));
    if (ContainsPath(pszPath))
        return FALSE;
    for (INT i = 0; i < m_items.GetSize(); ++i)
    {
        if (m_items[i].IsEmpty())
        {
            m_items[i].SetPath(pszPath);
            return TRUE;
        }
    }
    return m_items.Add(pszPath);
}

BOOL CDirectoryList::RenamePath(LPCWSTR pszPath1, LPCWSTR pszPath2)
{
    assert(!PathIsRelativeW(pszPath1));
    assert(!PathIsRelativeW(pszPath2));

    for (INT i = 0; i < m_items.GetSize(); ++i)
    {
        if (m_items[i].EqualPath(pszPath1))
        {
            // matched
            m_items[i].SetPath(pszPath2);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL CDirectoryList::DeletePath(LPCWSTR pszPath)
{
    assert(!PathIsRelativeW(pszPath));

    for (INT i = 0; i < m_items.GetSize(); ++i)
    {
        if (m_items[i].EqualPath(pszPath))
        {
            // matched
            m_items[i].SetPath(NULL);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL CDirectoryList::AddPathsFromDirectory(LPCWSTR pszDirectoryPath)
{
    // Scanning into an empty list (the constructor) finds every path once, so the
    // duplicate search of AddPath, which made a large scan quadratic, is skipped
    return ScanDirectory(pszDirectoryPath, m_items.GetSize() == 0);
}

BOOL CDirectoryList::ScanDirectory(LPCWSTR pszDirectoryPath, BOOL fFresh)
{
    // get the full path
    WCHAR szDir[MAX_PATH], szPath[MAX_PATH];
    lstrcpynW(szDir, pszDirectoryPath, _countof(szDir));
    assert(!PathIsRelativeW(szDir));

    // is it a directory?
    if (!PathIsDirectoryW(szDir))
        return FALSE;

    // add the path
    if (fFresh ? !m_items.Add(szDir) : !AddPath(szDir))
        return FALSE;

    // enumerate the file items to remember
    lstrcpynW(szPath, szDir, _countof(szPath));
    if (!PathAppendW(szPath, L"*"))
        return FALSE;
    WIN32_FIND_DATAW find;
    HANDLE hFind = FindFirstFileW(szPath, &find);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        ERR("FindFirstFileW failed\n");
        return FALSE;
    }

    LPWSTR pch;
    do
    {
        // ignore "." and ".."
        pch = find.cFileName;
        if (pch[0] == L'.' && (pch[1] == 0 || (pch[1] == L'.' && pch[2] == 0)))
            continue;

        // build a path (always from the directory, so a skipped name cannot
        // leave szPath one level too high for the next one)
        if (lstrlenW(szDir) + lstrlenW(find.cFileName) + 2 > MAX_PATH)
        {
            ERR("szPath is too long\n");
            continue;
        }
        lstrcpynW(szPath, szDir, _countof(szPath));
        PathAppendW(szPath, find.cFileName);

        // add the path and do recurse; junctions and symbolic links are not
        // followed, since they can lead back up the tree
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (m_fRecursive && !(find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                ScanDirectory(szPath, fFresh);
            else if (fFresh)
                m_items.Add(szPath);
            else
                AddPath(szPath);
        }
    } while (FindNextFileW(hFind, &find));

    FindClose(hFind);

    return TRUE;
}

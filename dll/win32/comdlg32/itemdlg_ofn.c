/*
 * PROJECT:     LiberNT
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Run GetOpenFileName/GetSaveFileName through the item dialog
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <shobjidl.h>

WINE_DEFAULT_DEBUG_CHANNEL(commdlg);

static const WCHAR PolicyKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Comdlg32";

static BOOL classic_forced(HKEY root)
{
    DWORD value = 0, size = sizeof(value);

    return RegGetValueW(root, PolicyKey, L"UseClassicFileDialog", RRF_RT_REG_DWORD,
                        NULL, &value, &size) == ERROR_SUCCESS && value;
}

/* Hooks, templates and the Windows 3.1 look keep the classic dialog, as on Windows. */
BOOL COMDLG32_UseItemDialog(const OPENFILENAMEW *ofn)
{
    if (ofn->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE))
        return FALSE;
    if ((ofn->Flags & OFN_ALLOWMULTISELECT) && !(ofn->Flags & OFN_EXPLORER))
        return FALSE;
    if (!ofn->lpstrFile || !ofn->nMaxFile)
        return FALSE;
    return !classic_forced(HKEY_CURRENT_USER) && !classic_forced(HKEY_LOCAL_MACHINE);
}

static LPWSTR heap_strdup(LPCWSTR str)
{
    SIZE_T size = (lstrlenW(str) + 1) * sizeof(WCHAR);
    LPWSTR copy = HeapAlloc(GetProcessHeap(), 0, size);

    if (copy)
        memcpy(copy, str, size);
    return copy;
}

static void free_filter_specs(COMDLG_FILTERSPEC *specs, UINT count)
{
    UINT i;

    for (i = 0; i < count; i++)
    {
        HeapFree(GetProcessHeap(), 0, (LPWSTR)specs[i].pszName);
        HeapFree(GetProcessHeap(), 0, (LPWSTR)specs[i].pszSpec);
    }
    HeapFree(GetProcessHeap(), 0, specs);
}

static COMDLG_FILTERSPEC *parse_filter(LPCWSTR filter, UINT *count)
{
    COMDLG_FILTERSPEC *specs;
    LPCWSTR p, spec;
    UINT n = 0, i;

    *count = 0;
    if (!filter)
        return NULL;
    for (p = filter; *p; p = spec + lstrlenW(spec) + 1)
    {
        spec = p + lstrlenW(p) + 1;
        if (!*spec)
            break;
        n++;
    }
    if (!n)
        return NULL;

    specs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(*specs));
    if (!specs)
        return NULL;
    for (p = filter, i = 0; i < n; i++)
    {
        spec = p + lstrlenW(p) + 1;
        specs[i].pszName = heap_strdup(p);
        specs[i].pszSpec = heap_strdup(spec);
        if (!specs[i].pszName || !specs[i].pszSpec)
        {
            free_filter_specs(specs, i + 1);
            return NULL;
        }
        p = spec + lstrlenW(spec) + 1;
    }
    *count = n;
    return specs;
}

static BOOL set_folder_path(IFileDialog *dialog, LPCWSTR path)
{
    IShellItem *item;

    if (!path || !*path || !PathIsDirectoryW(path))
        return FALSE;
    if (FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&item)))
        return FALSE;
    IFileDialog_SetFolder(dialog, item);
    IShellItem_Release(item);
    return TRUE;
}

/* The current directory is used only when it holds files of the selected type. */
static BOOL cwd_has_matches(const COMDLG_FILTERSPEC *spec)
{
    WCHAR pattern[MAX_PATH];
    LPCWSTR p, end;
    WIN32_FIND_DATAW data;
    HANDLE find;

    if (!spec)
        return FALSE;
    for (p = spec->pszSpec; *p; p = *end ? end + 1 : end)
    {
        end = p;
        while (*end && *end != L';')
            end++;
        while (p < end && *p == L' ')
            p++;
        if (p == end || (SIZE_T)(end - p) >= ARRAY_SIZE(pattern))
            continue;
        memcpy(pattern, p, (end - p) * sizeof(WCHAR));
        pattern[end - p] = 0;
        find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, 0);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                FindClose(find);
                return TRUE;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return FALSE;
}

/* Windows order: the path in lpstrFile, lpstrInitialDir, a matching current directory, Documents. */
static void set_initial_location(IFileDialog *dialog, const OPENFILENAMEW *ofn,
                                 const COMDLG_FILTERSPEC *spec)
{
    WCHAR dir[MAX_PATH];
    LPCWSTR name = ofn->lpstrFile, slash;

    if (*name)
    {
        slash = wcsrchr(name, L'\\');
        if (!slash)
            slash = wcsrchr(name, L'/');
        if (slash && (SIZE_T)(slash - name) < ARRAY_SIZE(dir) - 1)
        {
            memcpy(dir, name, (slash - name) * sizeof(WCHAR));
            dir[slash - name] = 0;
            if (slash == name + 2 && name[1] == L':')
                lstrcatW(dir, L"\\");
            if (set_folder_path(dialog, dir))
            {
                IFileDialog_SetFileName(dialog, slash + 1);
                return;
            }
        }
        IFileDialog_SetFileName(dialog, name);
    }

    if (set_folder_path(dialog, ofn->lpstrInitialDir))
        return;
    if (!ofn->lpstrInitialDir && cwd_has_matches(spec) &&
        GetCurrentDirectoryW(ARRAY_SIZE(dir), dir) && set_folder_path(dialog, dir))
    {
        return;
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, dir)))
        set_folder_path(dialog, dir);
}

static FILEOPENDIALOGOPTIONS map_options(DWORD flags, BOOL save)
{
    FILEOPENDIALOGOPTIONS fos = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;

    if (save && (flags & OFN_OVERWRITEPROMPT)) fos |= FOS_OVERWRITEPROMPT;
    if (!save && (flags & OFN_ALLOWMULTISELECT)) fos |= FOS_ALLOWMULTISELECT | FOS_NODEREFERENCELINKS;
    if (flags & OFN_PATHMUSTEXIST) fos |= FOS_PATHMUSTEXIST;
    if (flags & OFN_FILEMUSTEXIST) fos |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    if (flags & OFN_CREATEPROMPT) fos |= FOS_CREATEPROMPT;
    if (flags & OFN_NOVALIDATE) fos |= FOS_NOVALIDATE;
    if (flags & OFN_SHAREAWARE) fos |= FOS_SHAREAWARE;
    if (flags & OFN_NOREADONLYRETURN) fos |= FOS_NOREADONLYRETURN;
    if (flags & OFN_NOTESTFILECREATE) fos |= FOS_NOTESTFILECREATE;
    if (flags & OFN_NODEREFERENCELINKS) fos |= FOS_NODEREFERENCELINKS;
    if (flags & OFN_DONTADDTORECENT) fos |= FOS_DONTADDTORECENT;
    if (flags & OFN_FORCESHOWHIDDEN) fos |= FOS_FORCESHOWHIDDEN;
    return fos;
}

static LPWSTR get_item_path(IShellItem *item)
{
    LPWSTR path = NULL, copy;

    if (FAILED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)))
        return NULL;
    copy = heap_strdup(path);
    CoTaskMemFree(path);
    return copy;
}

/* As the classic dialog does: the needed size goes into the first WORD of the buffer. */
static BOOL buffer_too_small(OPENFILENAMEW *ofn, SIZE_T needed)
{
    if (ofn->nMaxFile >= 2)
        *(WORD *)ofn->lpstrFile = (WORD)min(needed, 0xffff);
    COMDLG32_SetCommDlgExtendedError(FNERR_BUFFERTOOSMALL);
    return FALSE;
}

static void set_file_title(OPENFILENAMEW *ofn, LPCWSTR name)
{
    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle)
        lstrcpynW(ofn->lpstrFileTitle, name, ofn->nMaxFileTitle);
}

static BOOL store_single(OPENFILENAMEW *ofn, LPCWSTR path)
{
    SIZE_T len = lstrlenW(path);
    LPCWSTR name, dot;

    if (len + 1 > ofn->nMaxFile)
        return buffer_too_small(ofn, len + 1);
    memcpy(ofn->lpstrFile, path, (len + 1) * sizeof(WCHAR));
    if ((ofn->Flags & OFN_ALLOWMULTISELECT) && len + 2 <= ofn->nMaxFile)
        ofn->lpstrFile[len + 1] = 0;

    name = PathFindFileNameW(ofn->lpstrFile);
    ofn->nFileOffset = (WORD)(name - ofn->lpstrFile);
    dot = wcsrchr(name, L'.');
    ofn->nFileExtension = dot ? (WORD)(dot + 1 - ofn->lpstrFile) : 0;
    /* Like the classic dialog, no extension at all also differs from lpstrDefExt. */
    if (ofn->lpstrDefExt && lstrcmpiW(dot ? dot + 1 : L"", ofn->lpstrDefExt))
        ofn->Flags |= OFN_EXTENSIONDIFFERENT;
    set_file_title(ofn, name);
    return TRUE;
}

/* Several files come back as "dir\0name1\0name2\0\0", like the Explorer-style dialog. */
static BOOL store_multiple(OPENFILENAMEW *ofn, IShellItemArray *items, DWORD count)
{
    LPWSTR *paths;
    SIZE_T dir_len, needed, pos;
    DWORD i;
    BOOL ret = FALSE;

    paths = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(*paths));
    if (!paths)
        return FALSE;
    for (i = 0; i < count; i++)
    {
        IShellItem *item;

        if (FAILED(IShellItemArray_GetItemAt(items, i, &item)))
            goto done;
        paths[i] = get_item_path(item);
        IShellItem_Release(item);
        if (!paths[i])
            goto done;
    }

    /* The layout names one folder; refuse rather than return names relative to the wrong one. */
    dir_len = PathFindFileNameW(paths[0]) - paths[0];
    for (i = 1; i < count; i++)
    {
        if ((SIZE_T)(PathFindFileNameW(paths[i]) - paths[i]) != dir_len || _wcsnicmp(paths[i], paths[0], dir_len))
        {
            COMDLG32_SetCommDlgExtendedError(FNERR_INVALIDFILENAME);
            goto done;
        }
    }
    if (dir_len > 1 && paths[0][dir_len - 1] == L'\\' && !(dir_len == 3 && paths[0][1] == L':'))
        dir_len--;
    needed = dir_len + 2;
    for (i = 0; i < count; i++)
        needed += lstrlenW(PathFindFileNameW(paths[i])) + 1;
    if (needed > ofn->nMaxFile)
    {
        ret = buffer_too_small(ofn, needed);
        goto done;
    }

    memcpy(ofn->lpstrFile, paths[0], dir_len * sizeof(WCHAR));
    ofn->lpstrFile[dir_len] = 0;
    pos = dir_len + 1;
    for (i = 0; i < count; i++)
    {
        LPCWSTR name = PathFindFileNameW(paths[i]);
        SIZE_T len = lstrlenW(name) + 1;

        memcpy(ofn->lpstrFile + pos, name, len * sizeof(WCHAR));
        pos += len;
    }
    ofn->lpstrFile[pos] = 0;
    ofn->nFileOffset = (WORD)(dir_len + 1);
    ofn->nFileExtension = 0;
    set_file_title(ofn, L"");
    ret = TRUE;

done:
    for (i = 0; i < count; i++)
        HeapFree(GetProcessHeap(), 0, paths[i]);
    HeapFree(GetProcessHeap(), 0, paths);
    return ret;
}

/* Multiselect asks for unresolved links; a single pick still follows its link, like the classic dialog. */
static LPWSTR resolve_link(LPWSTR path)
{
    WCHAR target[MAX_PATH];
    IShellLinkW *link;
    IPersistFile *file;
    LPWSTR resolved = NULL;

    if (lstrcmpiW(PathFindExtensionW(path), L".lnk") ||
        FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)))
    {
        return path;
    }
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)))
    {
        if (SUCCEEDED(IPersistFile_Load(file, path, STGM_READ)) &&
            IShellLinkW_GetPath(link, target, ARRAY_SIZE(target), NULL, 0) == S_OK && target[0])
        {
            resolved = heap_strdup(target);
        }
        IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
    if (!resolved)
        return path;
    HeapFree(GetProcessHeap(), 0, path);
    return resolved;
}

static BOOL store_item(OPENFILENAMEW *ofn, IShellItem *item)
{
    LPWSTR path = get_item_path(item);
    BOOL ret;

    if (!path)
    {
        COMDLG32_SetCommDlgExtendedError(FNERR_INVALIDFILENAME);
        return FALSE;
    }
    if ((ofn->Flags & OFN_ALLOWMULTISELECT) && !(ofn->Flags & OFN_NODEREFERENCELINKS))
        path = resolve_link(path);
    ret = store_single(ofn, path);
    HeapFree(GetProcessHeap(), 0, path);
    return ret;
}

static BOOL store_results(OPENFILENAMEW *ofn, IFileDialog *dialog, BOOL save)
{
    IFileOpenDialog *open;
    IShellItemArray *items;
    IShellItem *item;
    DWORD count = 0;
    BOOL ret = FALSE;

    if (!save && SUCCEEDED(IFileDialog_QueryInterface(dialog, &IID_IFileOpenDialog, (void **)&open)))
    {
        if (SUCCEEDED(IFileOpenDialog_GetResults(open, &items)))
        {
            if (SUCCEEDED(IShellItemArray_GetCount(items, &count)) && count > 1)
                ret = store_multiple(ofn, items, count);
            else if (count == 1 && SUCCEEDED(IShellItemArray_GetItemAt(items, 0, &item)))
            {
                ret = store_item(ofn, item);
                IShellItem_Release(item);
            }
            IShellItemArray_Release(items);
        }
        IFileOpenDialog_Release(open);
        return ret;
    }

    if (SUCCEEDED(IFileDialog_GetResult(dialog, &item)))
    {
        ret = store_item(ofn, item);
        IShellItem_Release(item);
    }
    return ret;
}

/* Returns TRUE when the item dialog ran; *result then holds the API result. */
BOOL COMDLG32_GetFileNameItemDialog(OPENFILENAMEW *ofn, BOOL save, BOOL *result)
{
    COMDLG_FILTERSPEC *specs;
    IFileDialog *dialog = NULL;
    UINT count = 0, index = 0;
    HRESULT hr, init;
    BOOL shown = FALSE;

    *result = FALSE;
    init = OleInitialize(NULL);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        return FALSE;

    hr = save ? FileSaveDialog_Constructor(NULL, &IID_IFileDialog, (void **)&dialog)
              : FileOpenDialog_Constructor(NULL, &IID_IFileDialog, (void **)&dialog);
    if (FAILED(hr))
        goto done;

    specs = parse_filter(ofn->lpstrFilter, &count);
    if (specs)
    {
        index = ofn->nFilterIndex ? min(ofn->nFilterIndex, count) : 1;
        IFileDialog_SetFileTypes(dialog, count, specs);
        IFileDialog_SetFileTypeIndex(dialog, index);
    }
    IFileDialog_SetOptions(dialog, map_options(ofn->Flags, save));
    if (!save && (ofn->Flags & OFN_ALLOWMULTISELECT) && !(ofn->Flags & OFN_NODEREFERENCELINKS))
        COMDLG32_ItemDialogFollowFolderLinks(dialog);
    if (ofn->lpstrTitle)
        IFileDialog_SetTitle(dialog, ofn->lpstrTitle);
    if (ofn->lpstrDefExt)
        IFileDialog_SetDefaultExtension(dialog, ofn->lpstrDefExt);
    set_initial_location(dialog, ofn, specs ? &specs[index - 1] : NULL);
    if (specs)
        free_filter_specs(specs, count);

    hr = IFileDialog_Show(dialog, ofn->hwndOwner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        COMDLG32_SetCommDlgExtendedError(0);
        shown = TRUE;
    }
    else if (SUCCEEDED(hr))
    {
        shown = TRUE;
        ofn->Flags &= ~(OFN_READONLY | OFN_EXTENSIONDIFFERENT);
        COMDLG32_SetCommDlgExtendedError(0);
        *result = store_results(ofn, dialog, save);
        if (*result && count && SUCCEEDED(IFileDialog_GetFileTypeIndex(dialog, &index)))
            ofn->nFilterIndex = index;
        /* The classic API moves the current directory to the chosen folder. */
        if (*result && !(ofn->Flags & OFN_NOCHANGEDIR) && ofn->nFileOffset > 1 && ofn->nFileOffset <= MAX_PATH)
        {
            WCHAR dir[MAX_PATH];

            lstrcpynW(dir, ofn->lpstrFile, ofn->nFileOffset);
            if (dir[1] == L':' && !dir[2])
                lstrcatW(dir, L"\\");
            SetCurrentDirectoryW(dir);
        }
    }
    else
    {
        WARN("Item dialog failed (0x%08lx), using the classic dialog\n", hr);
    }

done:
    if (dialog)
        IFileDialog_Release(dialog);
    if (SUCCEEDED(init))
        OleUninitialize();
    return shown;
}

static LPWSTR strdup_AtoW(LPCSTR str, int len)
{
    LPWSTR ret;
    int count;

    if (!str)
        return NULL;
    count = MultiByteToWideChar(CP_ACP, 0, str, len, NULL, 0);
    ret = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(WCHAR));
    if (ret)
    {
        MultiByteToWideChar(CP_ACP, 0, str, len, ret, count);
        ret[count] = 0;
    }
    return ret;
}

static int filter_lengthA(LPCSTR filter)
{
    LPCSTR s = filter;

    while (*s)
        s += strlen(s) + 1;
    return (int)(s - filter) + 1;
}

/* Characters the result occupies, terminators included. */
static SIZE_T result_lengthW(const OPENFILENAMEW *ofn)
{
    LPCWSTR p = ofn->lpstrFile;

    if (ofn->nFileOffset && !p[ofn->nFileOffset - 1])
    {
        p += ofn->nFileOffset;
        while (*p)
            p += lstrlenW(p) + 1;
        return p - ofn->lpstrFile + 1;
    }
    return lstrlenW(p) + 1;
}

static WORD offset_WtoA(LPCWSTR str, WORD offset)
{
    return offset ? (WORD)WideCharToMultiByte(CP_ACP, 0, str, offset, NULL, 0, NULL, NULL) : 0;
}

/* The ANSI APIs run the same path on a converted copy of the structure. */
BOOL COMDLG32_GetFileNameItemDialogA(OPENFILENAMEA *ofn, BOOL save, BOOL *result)
{
    OPENFILENAMEW ofnW;
    BOOL shown = FALSE;
    int len;

    *result = FALSE;
    if (!ofn->lpstrFile || !ofn->nMaxFile)
        return FALSE;
    ZeroMemory(&ofnW, sizeof(ofnW));
    memcpy(&ofnW, ofn, min(ofn->lStructSize, sizeof(ofnW)));
    ofnW.lStructSize = sizeof(ofnW);
    ofnW.lpstrCustomFilter = NULL;
    ofnW.nMaxCustFilter = 0;
    ofnW.lpstrFilter = ofn->lpstrFilter ? strdup_AtoW(ofn->lpstrFilter, filter_lengthA(ofn->lpstrFilter)) : NULL;
    ofnW.lpstrInitialDir = strdup_AtoW(ofn->lpstrInitialDir, -1);
    ofnW.lpstrTitle = strdup_AtoW(ofn->lpstrTitle, -1);
    ofnW.lpstrDefExt = strdup_AtoW(ofn->lpstrDefExt, -1);
    ofnW.lpstrFile = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (ofn->nMaxFile + 1) * sizeof(WCHAR));
    ofnW.lpstrFileTitle = ofn->lpstrFileTitle && ofn->nMaxFileTitle ?
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ofn->nMaxFileTitle * sizeof(WCHAR)) : NULL;
    if (!ofnW.lpstrFile || !COMDLG32_UseItemDialog(&ofnW))
        goto done;
    MultiByteToWideChar(CP_ACP, 0, ofn->lpstrFile, -1, ofnW.lpstrFile, ofn->nMaxFile);
    ofnW.lpstrFile[ofn->nMaxFile] = 0;

    shown = COMDLG32_GetFileNameItemDialog(&ofnW, save, result);
    if (!shown)
        goto done;
    ofn->Flags = ofnW.Flags;
    if (!*result)
    {
        if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL && ofn->nMaxFile >= 2)
        {
            CPINFO info;
            UINT needed = *(WORD *)ofnW.lpstrFile;

            /* Characters to bytes: assume the widest character of the ANSI code page. */
            if (GetCPInfo(CP_ACP, &info) && info.MaxCharSize > 1)
                needed *= info.MaxCharSize;
            *(WORD *)ofn->lpstrFile = (WORD)min(needed, 0xffff);
        }
        goto done;
    }

    len = WideCharToMultiByte(CP_ACP, 0, ofnW.lpstrFile, (int)result_lengthW(&ofnW), NULL, 0, NULL, NULL);
    if ((DWORD)len > ofn->nMaxFile)
    {
        if (ofn->nMaxFile >= 2)
            *(WORD *)ofn->lpstrFile = (WORD)min(len, 0xffff);
        COMDLG32_SetCommDlgExtendedError(FNERR_BUFFERTOOSMALL);
        *result = FALSE;
        goto done;
    }
    WideCharToMultiByte(CP_ACP, 0, ofnW.lpstrFile, (int)result_lengthW(&ofnW), ofn->lpstrFile, len, NULL, NULL);
    if ((ofn->Flags & OFN_ALLOWMULTISELECT) && (DWORD)len < ofn->nMaxFile)
        ofn->lpstrFile[len] = 0;
    ofn->nFileOffset = offset_WtoA(ofnW.lpstrFile, ofnW.nFileOffset);
    ofn->nFileExtension = offset_WtoA(ofnW.lpstrFile, ofnW.nFileExtension);
    ofn->nFilterIndex = ofnW.nFilterIndex;
    if (ofnW.lpstrFileTitle &&
        !WideCharToMultiByte(CP_ACP, 0, ofnW.lpstrFileTitle, -1, ofn->lpstrFileTitle, ofn->nMaxFileTitle, NULL, NULL))
    {
        ofn->lpstrFileTitle[ofn->nMaxFileTitle - 1] = 0;
    }

done:
    HeapFree(GetProcessHeap(), 0, (LPWSTR)ofnW.lpstrFilter);
    HeapFree(GetProcessHeap(), 0, (LPWSTR)ofnW.lpstrInitialDir);
    HeapFree(GetProcessHeap(), 0, (LPWSTR)ofnW.lpstrTitle);
    HeapFree(GetProcessHeap(), 0, (LPWSTR)ofnW.lpstrDefExt);
    HeapFree(GetProcessHeap(), 0, ofnW.lpstrFile);
    HeapFree(GetProcessHeap(), 0, ofnW.lpstrFileTitle);
    return shown;
}

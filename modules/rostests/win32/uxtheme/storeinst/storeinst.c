/*
 * PROJECT:     LiberNT tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * SPDX-License-Identifier: GPL-3.0-or-later
 * PURPOSE:     storeinst: install a visual style into the protected theme store the way the ThemeLoader4 trust gate
 *              expects it, so an ordinary user can load a version-4 style. It copies <style>.msstyles (and its MUI)
 *              into %SystemRoot%\Resources\Themes\<name>\<name>.msstyles, stamps the store security descriptor on the
 *              files and their directories, optionally writes a <name>.theme descriptor for the shell, and optionally makes the style the active system theme
 *              (--set-active) using the style's own default colour and size. Run it elevated, inside the test VM only;
 *              it never overwrites an existing store entry. --set-active must be run by the ordinary user who will see
 *              the theme (it writes that user's HKCU and applies in that user's session).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Usage: storeinst <style.msstyles> <name> [--mui <file.msstyles.mui> <locale>] [--theme] [--set-active]
 *   <name>        1..32 of [A-Za-z0-9_-], starting with a letter or digit (matches the gate's UXT_NAME_MAX)
 *   --mui         install a language module as <name>\<locale>\<name>.msstyles.mui (<locale> like en-US)
 *   --theme       write %SystemRoot%\Resources\Themes\<name>.theme pointing at the installed style (for the shell)
 *   --set-active  make the style the active system theme for the CURRENT (ordinary) user, using the colour and size
 *                 the style declares as its default; a non-zero exit means the apply did not take
 */

#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>

/* The store security descriptor the gate accepts (THEME-TRUST-POLICY v6 T-STORE):
 * owner Administrators, group SYSTEM, a protected DACL with SYSTEM and Administrators full control and Users
 * read+execute (0x1200a9). On the files the OI/CI flags have no effect. */
static const WCHAR STORE_SDDL[] =
    L"O:BAG:SYD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

static const WCHAR THEMEMGR[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager";

/* GetThemeDefaults and SetSystemVisualStyle are exported by ordinal only (7 and 65), here and on Windows. */
typedef HRESULT (WINAPI *PFN_GETTHEMEDEFAULTS)(LPCWSTR, LPWSTR, DWORD, LPWSTR, DWORD);
typedef HRESULT (WINAPI *PFN_SETSYSTEMVISUALSTYLE)(LPCWSTR, LPCWSTR, LPCWSTR, UINT);

static int fail(const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stderr, fmt, ap);
    va_end(ap);
    fputwc(L'\n', stderr);
    return 1;
}

static BOOL valid_name(const WCHAR *s)
{
    size_t i, n = wcslen(s);
    if (n < 1 || n > 32) return FALSE;   /* must match the gate's UXT_NAME_MAX (32) or every load is refused */
    if (!iswalnum(s[0])) return FALSE;
    for (i = 0; i < n; i++)
        if (!iswalnum(s[i]) && s[i] != L'_' && s[i] != L'-') return FALSE;
    return TRUE;
}

static BOOL valid_locale(const WCHAR *s)
{
    size_t i, n = wcslen(s);
    if (n < 2 || n > 16) return FALSE;
    for (i = 0; i < n; i++)
        if (!iswalnum(s[i]) && s[i] != L'-') return FALSE;
    return TRUE;
}

/* Apply STORE_SDDL to one path (file or directory). */
static BOOL stamp(const WCHAR *path)
{
    PSECURITY_DESCRIPTOR psd = NULL;
    PSID owner = NULL, group = NULL;
    PACL dacl = NULL;
    BOOL ownerDef = FALSE, groupDef = FALSE, daclPresent = FALSE, daclDef = FALSE;
    DWORD err;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(STORE_SDDL, SDDL_REVISION_1, &psd, NULL))
    {
        fail(L"storeinst: cannot parse the store descriptor (error %lu)", GetLastError());
        return FALSE;
    }
    GetSecurityDescriptorOwner(psd, &owner, &ownerDef);
    GetSecurityDescriptorGroup(psd, &group, &groupDef);
    GetSecurityDescriptorDacl(psd, &daclPresent, &dacl, &daclDef);

    err = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                owner, group, dacl, NULL);
    LocalFree(psd);
    if (err != ERROR_SUCCESS)
    {
        fail(L"storeinst: cannot set the descriptor of %ls (error %lu)", path, err);
        return FALSE;
    }
    return TRUE;
}

/* Print the descriptor of every directory from path up to AND INCLUDING the volume root, then the volume flags:
 * exactly what the gate checks (file, its directory, the store root and every directory above it, to the volume). */
static void report_chain(const WCHAR *leaf)
{
    WCHAR dir[MAX_PATH];
    BOOL atRoot = FALSE;
    lstrcpynW(dir, leaf, MAX_PATH);

    wprintf(L"Descriptors the gate checks (file, its directory, the store root and every directory above it):\n");
    for (;;)
    {
        LPWSTR sddl = NULL;
        PSECURITY_DESCRIPTOR psd = NULL;
        DWORD need = 0;
        WCHAR *slash;

        GetFileSecurityW(dir, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                         NULL, 0, &need);
        if (need && (psd = LocalAlloc(LPTR, need)) != NULL &&
            GetFileSecurityW(dir, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                             psd, need, &need) &&
            ConvertSecurityDescriptorToStringSecurityDescriptorW(
                psd, SDDL_REVISION_1,
                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sddl, NULL))
        {
            wprintf(L"  %ls  %ls\n", dir, sddl);
            LocalFree(sddl);
        }
        else
            wprintf(L"  %ls  (no descriptor read, error %lu)\n", dir, GetLastError());
        if (psd) LocalFree(psd);

        if (atRoot) break;                 /* the volume root was just printed */
        slash = wcsrchr(dir, L'\\');
        if (!slash) break;
        if (slash > dir && *(slash - 1) == L':') { slash[1] = 0; atRoot = TRUE; }  /* X:\ -> print once more */
        else *slash = 0;
    }

    {
        WCHAR root[MAX_PATH];
        DWORD flags = 0, maxc = 0;
        lstrcpynW(root, leaf, 4);          /* "X:\" */
        if (GetVolumeInformationW(root, NULL, 0, NULL, &maxc, &flags, NULL, 0))
            wprintf(L"Volume %ls flags 0x%08lx FILE_PERSISTENT_ACLS=%ls\n", root, flags,
                    (flags & FILE_PERSISTENT_ACLS) ? L"yes" : L"NO (ACLs will not stick)");
    }
}

/* Ensure a directory exists (no error if it already does). */
static BOOL ensure_dir(const WCHAR *path)
{
    if (CreateDirectoryW(path, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

int wmain(int argc, WCHAR **argv)
{
    const WCHAR *styleArg = NULL, *name = NULL, *muiArg = NULL, *locale = NULL;
    BOOL writeTheme = FALSE, setActive = FALSE, haveDefaults = FALSE;
    int i, rc = 0;
    WCHAR win[MAX_PATH], root[MAX_PATH], nameDir[MAX_PATH], styleDst[MAX_PATH];
    WCHAR color[64] = L"", size[64] = L"";
    HMODULE ux = NULL;

    for (i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--mui") && i + 2 < argc) { muiArg = argv[++i]; locale = argv[++i]; }
        else if (!wcscmp(argv[i], L"--theme"))      writeTheme = TRUE;
        else if (!wcscmp(argv[i], L"--set-active")) setActive = TRUE;
        else if (argv[i][0] == L'-')                return fail(L"storeinst: unknown argument %ls", argv[i]);
        else if (!styleArg)                         styleArg = argv[i];
        else if (!name)                             name = argv[i];
        else                                        return fail(L"storeinst: unexpected argument %ls", argv[i]);
    }

    if (!styleArg || !name)
        return fail(L"usage: storeinst <style.msstyles> <name> [--mui <file.msstyles.mui> <locale>]"
                    L" [--theme] [--set-active]");
    if (!valid_name(name))
        return fail(L"storeinst: the name must be 1-32 of [A-Za-z0-9_-] starting with a letter or digit");
    if (muiArg && !valid_locale(locale))
        return fail(L"storeinst: the locale must look like en-US");

    if (!GetWindowsDirectoryW(win, MAX_PATH))
        return fail(L"storeinst: cannot get the Windows directory (error %lu)", GetLastError());
    StringCchPrintfW(root, MAX_PATH, L"%ls\\Resources\\Themes", win);
    StringCchPrintfW(nameDir, MAX_PATH, L"%ls\\%ls", root, name);
    StringCchPrintfW(styleDst, MAX_PATH, L"%ls\\%ls.msstyles", nameDir, name);

    /* Install, unless the store entry is already there (then just proceed to the theme/active steps). */
    if (GetFileAttributesW(styleDst) == INVALID_FILE_ATTRIBUTES)
    {
        if (!ensure_dir(root) || !ensure_dir(nameDir))
            return fail(L"storeinst: cannot create %ls (error %lu); run elevated", nameDir, GetLastError());
        if (!CopyFileW(styleArg, styleDst, TRUE))
            return fail(L"storeinst: cannot copy %ls to %ls (error %lu)", styleArg, styleDst, GetLastError());

        if (muiArg)
        {
            WCHAR locDir[MAX_PATH], muiDst[MAX_PATH];
            StringCchPrintfW(locDir, MAX_PATH, L"%ls\\%ls", nameDir, locale);
            StringCchPrintfW(muiDst, MAX_PATH, L"%ls\\%ls.msstyles.mui", locDir, name);
            if (!ensure_dir(locDir) || !CopyFileW(muiArg, muiDst, TRUE))
                return fail(L"storeinst: the MUI install failed (error %lu)", GetLastError());
            if (!stamp(locDir) || !stamp(muiDst)) return 1;
        }
        if (!stamp(nameDir) || !stamp(styleDst)) return 1;
        wprintf(L"Installed %ls\n", styleDst);
    }
    else
        wprintf(L"%ls already present; keeping it\n", styleDst);

    /* Resolve the style's own default colour and size (GetThemeDefaults is UXTHEME ordinal 7, no name). */
    if (writeTheme || setActive)
    {
        ux = LoadLibraryW(L"uxtheme.dll");
        if (ux)
        {
            PFN_GETTHEMEDEFAULTS gd = (PFN_GETTHEMEDEFAULTS)GetProcAddress(ux, (LPCSTR)(ULONG_PTR)7);
            if (gd && SUCCEEDED(gd(styleDst, color, ARRAYSIZE(color), size, ARRAYSIZE(size))) && color[0] && size[0])
                haveDefaults = TRUE;
        }
    }

    if (writeTheme)
    {
        /* For the .theme descriptor the generic NormalColor/NormalSize is an acceptable default when the style's
         * own names could not be read; the loader resolves the real variant from the .msstyles anyway. */
        const WCHAR *tc = haveDefaults ? color : L"NormalColor";
        const WCHAR *tsz = haveDefaults ? size : L"NormalSize";
        WCHAR themePath[MAX_PATH], body[512];
        HANDLE hf;
        StringCchPrintfW(themePath, MAX_PATH, L"%ls\\%ls.theme", root, name);
        hf = CreateFileW(themePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            DWORD wr;
            WORD bom = 0xFEFF;
            StringCchPrintfW(body, ARRAYSIZE(body),
                L"[Theme]\r\nDisplayName=%ls\r\n\r\n[VisualStyles]\r\n"
                L"Path=%%SystemRoot%%\\Resources\\Themes\\%ls\\%ls.msstyles\r\n"
                L"ColorStyle=%ls\r\nSize=%ls\r\n", name, name, name, tc, tsz);
            WriteFile(hf, &bom, sizeof(bom), &wr, NULL);
            WriteFile(hf, body, (DWORD)(wcslen(body) * sizeof(WCHAR)), &wr, NULL);
            CloseHandle(hf);
            stamp(themePath);   /* best effort: a .theme in the store root is not loaded through the gate */
            wprintf(L"Wrote %ls\n", themePath);
        }
        else
        {
            wprintf(L"storeinst: could not write %ls (error %lu)\n", themePath, GetLastError());
            rc = 1;
        }
    }

    if (setActive)
    {
        if (!haveDefaults)
        {
            /* Do not guess the colour/size: a wrong variant name makes the ordinary-user load fail outright. */
            fail(L"storeinst: could not read the style default colour/size; run --set-active as the ordinary user "
                 L"who will see the theme (not elevated), so uxtheme can read the version-4 style");
            rc = 1;
        }
        else
        {
            HKEY hk;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, THEMEMGR, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS)
            {
                const WCHAR one[] = L"1";
                RegSetValueExW(hk, L"DllName",    0, REG_SZ, (const BYTE *)styleDst, (lstrlenW(styleDst) + 1) * sizeof(WCHAR));
                RegSetValueExW(hk, L"ColorName",  0, REG_SZ, (const BYTE *)color,    (lstrlenW(color)    + 1) * sizeof(WCHAR));
                RegSetValueExW(hk, L"SizeName",   0, REG_SZ, (const BYTE *)size,     (lstrlenW(size)     + 1) * sizeof(WCHAR));
                RegSetValueExW(hk, L"ThemeActive",0, REG_SZ, (const BYTE *)one,      sizeof(one));
                RegCloseKey(hk);
            }
            else
            {
                if (ux) FreeLibrary(ux);
                return fail(L"storeinst: cannot write HKCU ThemeManager (error %lu)", GetLastError());
            }

            /* Apply it live. SetSystemVisualStyle (uxtheme ordinal 65) needs the theme hooks active in the
             * calling process, which a console tool does not have (it returns E_FAIL), so broadcast
             * WM_THEMECHANGED instead: the themed processes reload the HKCU DllName/ColorName/SizeName just
             * written and switch to the style now. */
            SendNotifyMessageW(HWND_BROADCAST, WM_THEMECHANGED, 0, 0);
            wprintf(L"Active theme set: %ls (%ls/%ls); broadcast WM_THEMECHANGED\n", styleDst, color, size);
        }
    }

    if (ux) FreeLibrary(ux);
    report_chain(styleDst);
    return rc;
}

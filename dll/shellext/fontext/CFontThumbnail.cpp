/*
 * PROJECT:     LiberNT Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Thumbnail provider for font files
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winternl.h>

/* The NDK I/O headers clash with the ATL status macros, so the little of
 * FileFsDeviceInformation that is needed here is declared locally. */
typedef enum _FONTEXT_FS_INFORMATION_CLASS
{
    FontextFileFsDeviceInformation = 4
} FONTEXT_FS_INFORMATION_CLASS;

typedef struct _FONTEXT_FS_DEVICE_INFORMATION
{
    ULONG DeviceType;
    ULONG Characteristics;
} FONTEXT_FS_DEVICE_INFORMATION;

extern "C" NTSTATUS NTAPI
NtQueryVolumeInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation,
                             ULONG Length, FONTEXT_FS_INFORMATION_CLASS FsInformationClass);

#ifndef FILE_DEVICE_DISK
#define FILE_DEVICE_DISK 0x00000007
#endif
#ifndef FILE_REMOVABLE_MEDIA
#define FILE_REMOVABLE_MEDIA 0x00000001
#endif
#ifndef FILE_FLOPPY_DISKETTE
#define FILE_FLOPPY_DISKETTE 0x00000004
#endif
#ifndef FILE_REMOTE_DEVICE
#define FILE_REMOTE_DEVICE 0x00000010
#endif

WINE_DEFAULT_DEBUG_CHANNEL(fontext);

const GUID CLSID_CFontThumbnail = { 0xe2537d60, 0x39c7, 0x4e40, { 0x95, 0x60, 0xb3, 0x38, 0x6d, 0x4b, 0xb2, 0x07 } };

STDMETHODIMP CFontThumbnail::Initialize(LPCWSTR pszFilePath, DWORD grfMode)
{
    if (!pszFilePath || !*pszFilePath)
        return E_INVALIDARG;
    m_Path = pszFilePath;
    return S_OK;
}

#ifndef FILE_PORTABLE_DEVICE
#define FILE_PORTABLE_DEVICE 0x00004000
#endif

/*
 * A thumbnail parses the font in the kernel font engine without any click, so
 * only files from trusted locations get one: the installed fonts under
 * SystemRoot\Fonts, and otherwise files on a local fixed disk (judged on the
 * opened file's own volume, so a junction, a mount point or a SUBST drive onto
 * removable or network storage does not pass) that carry no Zone.Identifier
 * stream. Anything that cannot be verified keeps its icon.
 */
static BOOL IsTrustedFontLocation(LPCWSTR pszGivenPath)
{
    WCHAR szPath[MAX_PATH];
    WCHAR szFonts[MAX_PATH];
    LPCWSTR pszPath = szPath;
    UINT cch;
    HANDLE hFile, hStream;
    IO_STATUS_BLOCK Iosb;
    FONTEXT_FS_DEVICE_INFORMATION Device = { 0, 0 };
    NTSTATUS Status;
    DWORD dwError;
    CStringW strStream;

    if (!pszGivenPath || !*pszGivenPath)
        return FALSE;
    /* An in-process caller may hand in a relative path or one with ..\: compare the real one */
    cch = GetFullPathNameW(pszGivenPath, _countof(szPath), szPath, NULL);
    if (!cch || cch >= _countof(szPath) || PathIsUNCW(pszPath))
        return FALSE;

    cch = GetWindowsDirectoryW(szFonts, _countof(szFonts));
    if (cch && cch < _countof(szFonts) && PathAppendW(szFonts, L"Fonts"))
    {
        cch = lstrlenW(szFonts);
        if (StrCmpNIW(pszPath, szFonts, cch) == 0 && pszPath[cch] == L'\\')
            return TRUE;
    }

    hFile = CreateFileW(pszPath, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Status = NtQueryVolumeInformationFile(hFile, &Iosb, &Device, sizeof(Device), FontextFileFsDeviceInformation);
    CloseHandle(hFile);
    if (!NT_SUCCESS(Status) || Device.DeviceType != FILE_DEVICE_DISK ||
        (Device.Characteristics & (FILE_REMOVABLE_MEDIA | FILE_REMOTE_DEVICE | FILE_FLOPPY_DISKETTE | FILE_PORTABLE_DEVICE)))
    {
        TRACE("'%S' is not on a local fixed disk (status 0x%08lx type %lu char 0x%lx): no thumbnail\n",
              pszPath, Status, Device.DeviceType, Device.Characteristics);
        return FALSE;
    }

    strStream = pszPath;
    strStream += L":Zone.Identifier";
    hStream = CreateFileW(strStream, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (hStream != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hStream);
        TRACE("'%S' carries a Zone.Identifier stream: no thumbnail\n", pszPath);
        return FALSE;
    }
    dwError = GetLastError();
    if (dwError != ERROR_FILE_NOT_FOUND && dwError != ERROR_PATH_NOT_FOUND)
    {
        /* Cannot tell whether the file was downloaded: do not trust it */
        TRACE("'%S': Zone.Identifier check failed with %lu: no thumbnail\n", pszPath, dwError);
        return FALSE;
    }
    return TRUE;
}

static ULONG BE32(const BYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static USHORT BE16(const BYTE *p)
{
    return (USHORT)((p[0] << 8) | p[1]);
}

static BOOL ReadAt(HANDLE hFile, ULONGLONG Offset, void *pBuffer, DWORD cb)
{
    LARGE_INTEGER li;
    DWORD cbRead;

    li.QuadPart = Offset;
    return SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) && ReadFile(hFile, pBuffer, cb, &cbRead, NULL) && cbRead == cb;
}

/*
 * Family and style of the first face of a TrueType/OpenType file or
 * collection, from its own 'name' table: typographic family (16) before the
 * legacy family (1), as FreeType names the face, and the subfamily (2) for the
 * weight and slant. GetFontResourceInfoW cannot be used here because
 * win32k describes only globally installed fonts, not a private resource.
 */
static BOOL GetSfntNames(LPCWSTR pszPath, LPWSTR pszFamily, UINT cchFamily, LPWSTR pszStyle, UINT cchStyle)
{
    HANDLE hFile;
    BYTE Header[12];
    BYTE *pDir = NULL, *pName = NULL;
    ULONGLONG Base = 0;
    ULONG Tag, NameOffset = 0, NameLength = 0;
    USHORT NumTables, Count, StringOffset, i;
    int BestFamily = 0, BestStyle = 0;
    BOOL bRet = FALSE;

    pszFamily[0] = pszStyle[0] = UNICODE_NULL;
    hFile = CreateFileW(pszPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!ReadAt(hFile, 0, Header, sizeof(Header)))
        goto done;
    Tag = BE32(Header);
    if (Tag == 0x74746366) /* 'ttcf': first font of the collection */
    {
        Base = BE32(Header + 8);
        if (!ReadAt(hFile, Base, Header, sizeof(Header)))
            goto done;
        Tag = BE32(Header);
    }
    if (Tag != 0x00010000 && Tag != 0x4F54544F /* 'OTTO' */ && Tag != 0x74727565 /* 'true' */)
        goto done;
    NumTables = BE16(Header + 4);
    if (!NumTables || NumTables > 512)
        goto done;
    pDir = (BYTE *)HeapAlloc(GetProcessHeap(), 0, NumTables * 16);
    if (!pDir || !ReadAt(hFile, Base + 12, pDir, NumTables * 16))
        goto done;
    for (i = 0; i < NumTables; i++)
    {
        if (BE32(pDir + i * 16) == 0x6E616D65) /* 'name' */
        {
            NameOffset = BE32(pDir + i * 16 + 8);
            NameLength = BE32(pDir + i * 16 + 12);
            break;
        }
    }
    if (NameLength < 6 || NameLength > (1 << 20))
        goto done;
    pName = (BYTE *)HeapAlloc(GetProcessHeap(), 0, NameLength);
    if (!pName || !ReadAt(hFile, NameOffset, pName, NameLength))
        goto done;
    Count = BE16(pName + 2);
    StringOffset = BE16(pName + 4);
    for (i = 0; i < Count && 6 + (ULONG)(i + 1) * 12 <= NameLength; i++)
    {
        const BYTE *pRec = pName + 6 + i * 12;
        USHORT Platform = BE16(pRec), Encoding = BE16(pRec + 2), Language = BE16(pRec + 4);
        USHORT Id = BE16(pRec + 6), Length = BE16(pRec + 8), Offset = BE16(pRec + 10);
        const BYTE *pStr = pName + StringOffset + Offset;
        BOOL bWide;
        int Score;
        LPWSTR pszOut;
        UINT cchOut, n, cch;

        if (Id != 16 && Id != 1 && Id != 2)
            continue;
        if ((ULONG)StringOffset + Offset + Length > NameLength || !Length)
            continue;
        if (Platform == 3 && (Encoding == 1 || Encoding == 10))
        {
            bWide = TRUE;
            Score = (Language == 0x0409) ? 6 : 4;
        }
        else if (Platform == 0)
        {
            bWide = TRUE;
            Score = 3;
        }
        else if (Platform == 1 && Encoding == 0)
        {
            bWide = FALSE;
            Score = 1;
        }
        else
        {
            continue;
        }
        if (Id == 16)
            Score += 10; /* the typographic family wins over the legacy one */
        if (Id == 2)
        {
            if (Score <= BestStyle)
                continue;
            BestStyle = Score;
            pszOut = pszStyle;
            cchOut = cchStyle;
        }
        else
        {
            if (Score <= BestFamily)
                continue;
            BestFamily = Score;
            pszOut = pszFamily;
            cchOut = cchFamily;
        }
        cch = bWide ? Length / 2 : Length;
        if (cch > cchOut - 1)
            cch = cchOut - 1;
        for (n = 0; n < cch; n++)
            pszOut[n] = bWide ? (WCHAR)BE16(pStr + n * 2) : (WCHAR)pStr[n];
        pszOut[cch] = UNICODE_NULL;
    }
    bRet = pszFamily[0] != UNICODE_NULL;

done:
    if (pName)
        HeapFree(GetProcessHeap(), 0, pName);
    if (pDir)
        HeapFree(GetProcessHeap(), 0, pDir);
    CloseHandle(hFile);
    return bRet;
}

/*
 * The sample text in the face itself on a white tile, as the Fonts folder
 * shows it. The file is loaded as a private, non-enumerable resource for the
 * duration of the call, so it never shows up as an installed font; the font
 * mapper searches the process-private list first, so the file's own face is
 * the one drawn even when an installed face has the same name. Raster fonts
 * (.fon/.fnt) carry no 'name' table and get no thumbnail for now.
 */
STDMETHODIMP CFontThumbnail::GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha)
{
    WCHAR szFamily[LF_FACESIZE], szStyle[64];
    BITMAPINFO bmi;
    RGBQUAD *pBits = NULL;
    HDC hdc;
    HBITMAP hbm;
    HGDIOBJ hbmOld, hFontOld;
    HFONT hFont;
    LOGFONTW lf;
    RECT rc;
    BOOL bPrivate = FALSE;
    HRESULT hr = E_FAIL;

    if (pdwAlpha)
        *pdwAlpha = WTSAT_UNKNOWN;
    if (!phbmp)
        return E_POINTER;
    *phbmp = NULL;
    if (m_Path.IsEmpty() || !cx || cx > 4096)
        return E_FAIL;
    if (!IsTrustedFontLocation(m_Path))
        return E_FAIL;
    if (!GetSfntNames(m_Path, szFamily, _countof(szFamily), szStyle, _countof(szStyle)))
    {
        TRACE("'%S' has no usable name table: no thumbnail\n", (LPCWSTR)m_Path);
        return E_FAIL;
    }

    bPrivate = AddFontResourceExW(m_Path, FR_PRIVATE | FR_NOT_ENUM, NULL) != 0;
    if (!bPrivate)
    {
        ERR("Cannot load '%S' as a private font resource (%lu)\n", (LPCWSTR)m_Path, GetLastError());
        goto done;
    }

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -(LONG)cx;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc)
        goto done;
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&pBits, NULL, 0);
    if (!hbm || !pBits)
    {
        if (hbm)
            DeleteObject(hbm);
        DeleteDC(hdc);
        hr = E_OUTOFMEMORY;
        goto done;
    }
    FillMemory(pBits, cx * cx * sizeof(RGBQUAD), 0xFF); /* white, opaque */

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -(LONG)(cx * 9 / 20);
    lf.lfWeight = StrStrIW(szStyle, L"Bold") ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (StrStrIW(szStyle, L"Italic") || StrStrIW(szStyle, L"Oblique")) ? TRUE : FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH;
    StringCchCopyW(lf.lfFaceName, _countof(lf.lfFaceName), szFamily);
    hFont = CreateFontIndirectW(&lf);

    hbmOld = SelectObject(hdc, hbm);
    hFontOld = SelectObject(hdc, hFont ? hFont : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    SetRect(&rc, 0, 0, cx, cx);
    DrawTextW(hdc, L"Abc", 3, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    SelectObject(hdc, hFontOld);
    SelectObject(hdc, hbmOld);
    GdiFlush();
    if (hFont)
        DeleteObject(hFont);
    DeleteDC(hdc);

    /* Text output leaves the alpha bytes of a 32bpp DIB at zero */
    for (UINT i = 0; i < cx * cx; i++)
        pBits[i].rgbReserved = 0xFF;

    if (pdwAlpha)
        *pdwAlpha = WTSAT_RGB;
    *phbmp = hbm;
    hr = S_OK;

done:
    if (bPrivate)
        RemoveFontResourceExW(m_Path, FR_PRIVATE | FR_NOT_ENUM, NULL);
    return hr;
}

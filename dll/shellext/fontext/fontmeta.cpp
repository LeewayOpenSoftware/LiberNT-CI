/*
 * PROJECT:     ReactOS Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Font file metadata (sfnt name/OS2 tables) for the Fonts folder columns
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "precomp.h"

static inline WORD Be16(const BYTE* p) { return (WORD)((p[0] << 8) | p[1]); }
static inline DWORD Be32(const BYTE* p) { return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | p[3]; }

static bool ReadAt(HANDLE hFile, ULONGLONG offset, void* buffer, DWORD cb)
{
    LARGE_INTEGER li;
    DWORD cbRead;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
        return false;
    return ReadFile(hFile, buffer, cb, &cbRead, NULL) && cbRead == cb;
}

static CStringW GetSfntName(const BYTE* nt, DWORD len, WORD id)
{
    CStringW result;
    if (len < 6)
        return result;

    DWORD count = Be16(nt + 2), strOff = Be16(nt + 4);
    int bestScore = 0;
    for (DWORD i = 0; i < count; ++i)
    {
        DWORD rec = 6 + i * 12;
        if (rec + 12 > len)
            break;
        const BYTE* r = nt + rec;
        WORD platform = Be16(r), enc = Be16(r + 2), lang = Be16(r + 4), nameId = Be16(r + 6);
        DWORD cb = Be16(r + 8), off = Be16(r + 10);
        if (nameId != id || strOff + off + cb > len || !cb)
            continue;

        int score = 0;
        if (platform == 3 && (enc == 1 || enc == 10))
            score = (lang == 0x409) ? 4 : 3;
        else if (platform == 0)
            score = 2;
        else if (platform == 1 && enc == 0 && lang == 0)
            score = 1;
        if (score <= bestScore)
            continue;

        const BYTE* s = nt + strOff + off;
        CStringW text;
        if (platform == 1)
        {
            for (DWORD k = 0; k < cb; ++k)
                text += (WCHAR)s[k];
        }
        else
        {
            for (DWORD k = 0; k + 1 < cb; k += 2)
                text += (WCHAR)Be16(s + k);
        }
        if (!text.IsEmpty())
        {
            result = text;
            bestScore = score;
        }
    }
    return result;
}

bool ReadFontMeta(PCWSTR path, CFontMeta& meta)
{
    meta = CFontMeta();

    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    bool ok = false;
    BYTE hdr[12];
    ULONGLONG base = 0;
    if (ReadAt(hFile, 0, hdr, sizeof(hdr)))
    {
        if (Be32(hdr) == 0x74746366 /* 'ttcf' */)
        {
            BYTE first[4];
            if (ReadAt(hFile, 12, first, sizeof(first)))
                base = Be32(first);
            else
                base = (ULONGLONG)-1;
        }
        if (base != (ULONGLONG)-1 && ReadAt(hFile, base, hdr, sizeof(hdr)))
        {
            DWORD tag = Be32(hdr);
            if (tag == 0x00010000 || tag == 0x4F54544F /* 'OTTO' */ || tag == 0x74727565 /* 'true' */)
            {
                DWORD numTables = Be16(hdr + 4);
                BYTE dir[64 * 16];
                if (numTables > 64)
                    numTables = 64;
                if (numTables && ReadAt(hFile, base + 12, dir, numTables * 16))
                {
                    DWORD nameOff = 0, nameLen = 0, os2Off = 0, os2Len = 0;
                    for (DWORD i = 0; i < numTables; ++i)
                    {
                        const BYTE* e = dir + i * 16;
                        DWORD t = Be32(e);
                        if (t == 0x6E616D65 /* 'name' */)
                        {
                            nameOff = Be32(e + 8);
                            nameLen = Be32(e + 12);
                        }
                        else if (t == 0x4F532F32 /* 'OS/2' */)
                        {
                            os2Off = Be32(e + 8);
                            os2Len = Be32(e + 12);
                        }
                    }

                    if (nameOff && nameLen >= 6 && nameLen <= (1 << 20))
                    {
                        CComHeapPtr<BYTE> nt;
                        if (nt.AllocateBytes(nameLen) && ReadAt(hFile, nameOff, nt, nameLen))
                        {
                            meta.Family = GetSfntName(nt, nameLen, 16);
                            if (meta.Family.IsEmpty())
                                meta.Family = GetSfntName(nt, nameLen, 1);
                            meta.Style = GetSfntName(nt, nameLen, 17);
                            if (meta.Style.IsEmpty())
                                meta.Style = GetSfntName(nt, nameLen, 2);
                            meta.Designer = GetSfntName(nt, nameLen, 9);
                            meta.Foundry = GetSfntName(nt, nameLen, 8);
                            ok = !meta.Family.IsEmpty();
                        }
                    }

                    if (os2Off && os2Len >= 78)
                    {
                        BYTE os2[96] = {};
                        DWORD cb = os2Len < sizeof(os2) ? os2Len : (DWORD)sizeof(os2);
                        if (ReadAt(hFile, os2Off, os2, cb))
                        {
                            meta.hasOS2 = true;
                            meta.weight = Be16(os2 + 4);
                            meta.fsType = Be16(os2 + 8);
                            meta.panoseFamily = os2[32];
                            for (int i = 0; i < 4; ++i)
                                meta.uni[i] = Be32(os2 + 42 + i * 4);
                            meta.italic = (Be16(os2 + 62) & 1) != 0;
                            if (cb >= 86 && Be16(os2) >= 1)
                            {
                                meta.cp[0] = Be32(os2 + 78);
                                meta.cp[1] = Be32(os2 + 82);
                            }
                        }
                    }
                }
            }
        }
    }

    CloseHandle(hFile);
    meta.valid = ok;
    return ok;
}

CStringW FontMeta_DesignedFor(const CFontMeta& m)
{
    CStringW out;
    auto add = [&out](PCWSTR s)
    {
        if (!out.IsEmpty())
            out += L", ";
        out += s;
    };

    if (m.Cp(0) || m.Uni(1))
        add(L"Latin");
    if (m.Uni(7))
        add(L"Greek");
    if (m.Uni(9))
        add(L"Cyrillic");
    if (m.Uni(10))
        add(L"Armenian");
    if (m.Uni(11))
        add(L"Hebrew");
    if (m.Uni(13))
        add(L"Arabic");
    if (m.Uni(15))
        add(L"Devanagari");
    if (m.Uni(24))
        add(L"Thai");
    if (m.Cp(19) || m.Cp(21) || m.Uni(56))
        add(L"Korean");
    if (m.Cp(17) || (m.Uni(49) && m.Uni(50)))
        add(L"Japanese");
    if (m.Cp(18))
        add(L"Chinese (Simplified)");
    if (m.Cp(20))
        add(L"Chinese (Traditional)");
    return out;
}

int FontMeta_Category(const CFontMeta& m)
{
    if (m.panoseFamily == 5 || m.Cp(31))
        return 2;
    if (m.panoseFamily == 3 || m.panoseFamily == 4)
        return 1;
    return 0;
}

int FontMeta_Embeddability(const CFontMeta& m)
{
    if (m.fsType & 0x0002)
        return 3;
    if (m.fsType & 0x0004)
        return 2;
    if (m.fsType & 0x0008)
        return 1;
    return 0;
}

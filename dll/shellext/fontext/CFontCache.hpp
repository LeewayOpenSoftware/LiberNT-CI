/*
 * PROJECT:     ReactOS Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     font list cache handling
 * COPYRIGHT:   Copyright 2019-2021 Mark Jansen <mark.jansen@reactos.org>
 */

#pragma once

class CFontInfo
{
private:
    CStringW m_Name;
    CStringW m_File;
    BOOL m_bMarkDeleted = FALSE;
    bool m_FileRead;
    bool m_AttrsRead;
    LARGE_INTEGER m_FileSize;
    FILETIME m_FileWriteTime;
    DWORD m_dwFileAttributes;
    bool m_MetaRead = false;
    CFontMeta m_Meta;

    void ReadAttrs();

public:
    CFontInfo(PCWSTR name = L"", PCWSTR value = L"");

    const CStringW& Name() const;   // Font display name stored in the registry
    const bool Valid() const;

    BOOL IsMarkDeleted() const { return m_bMarkDeleted; }
    void MarkDeleted() { m_bMarkDeleted = TRUE; }

    const CStringW& File();         // Full path or file, depending on how it's stored in the registry
    const LARGE_INTEGER& FileSize();
    const FILETIME& FileWriteTime();
    DWORD FileAttributes();

    CFontMeta& Meta();              // read lazily from the font file
    CStringW DisplayName() const;   // Name() without the "(TrueType)"/"(OpenType)" suffix
    CStringW Family();              // family from the file; the display name when unreadable
};

CStringW FontDisplayName(PCWSTR registryName);

struct FontViewItem
{
    bool bFamily;
    CStringW RegName;   // registry value name (of the representative for a family)
    CStringW Key;       // family grouping key
};

class CFontCache
{
private:
    CSimpleArray<CFontInfo> m_Fonts;
    CStringW m_FontFolderPath;

protected:
    CFontCache();

    void Insert(CAtlList<CFontInfo>& fonts, const CStringW& KeyName, PCWSTR Value);

public:
    void Read();

    void SetFontDir(const LPCWSTR Path);
    const CStringW& FontPath() const { return m_FontFolderPath; }
    CStringW GetFontFilePath(const PCWSTR Path) const;

    size_t Size();
    CStringW Name(size_t Index);    // Font display name stored in the registry
    CStringW File(size_t Index);

    CFontInfo* Find(const FontPidlEntry* fontEntry);
    CFontInfo* FindByName(PCWSTR registryName);
    // Items of the root (one per family, plain items for single-style families) or of one family
    void BuildView(const CStringW* pFamilyKey, CAtlArray<FontViewItem>& items);
    bool IsHidden(CFontInfo* info);
    void SetHidden(CFontInfo* info, bool hidden);
    BOOL IsMarkDeleted(size_t Index) const;
    void MarkDeleted(const FontPidlEntry* fontEntry);
    CStringW Filename(CFontInfo* info, bool alwaysFullPath = false);

    friend class CFontExtModule;
};

extern CFontCache* g_FontCache;

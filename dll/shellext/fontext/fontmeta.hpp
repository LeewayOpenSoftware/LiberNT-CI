/*
 * PROJECT:     ReactOS Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Font file metadata (sfnt name/OS2 tables) for the Fonts folder columns
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#pragma once

struct CFontMeta
{
    CStringW Family;    // name ID 16, else 1
    CStringW Style;     // name ID 17, else 2
    CStringW Designer;  // name ID 9
    CStringW Foundry;   // name ID 8
    WORD fsType = 0;
    WORD weight = 400;
    bool italic = false;
    bool hasOS2 = false;
    bool valid = false;
    BYTE panoseFamily = 0;
    DWORD uni[4] = {};
    DWORD cp[2] = {};

    bool Uni(int bit) const { return (uni[bit / 32] >> (bit % 32)) & 1; }
    bool Cp(int bit) const { return (cp[bit / 32] >> (bit % 32)) & 1; }
};

// Reads the first face of a TrueType/OpenType font or collection. Returns false on any parse error.
bool ReadFontMeta(PCWSTR path, CFontMeta& meta);

CStringW FontMeta_DesignedFor(const CFontMeta& meta);
// 0 = Text, 1 = Display, 2 = Symbol
int FontMeta_Category(const CFontMeta& meta);
// 0 = Installable, 1 = Editable, 2 = Preview and print, 3 = Restricted license
int FontMeta_Embeddability(const CFontMeta& meta);

// Sample-icon provider ("Abc" rendered in the font) for the Fonts folder items
struct FontIconSpec
{
    CStringW File;      // full path of the font file
    CStringW Face;      // family name to render with
    WORD Weight;
    bool Italic;
    bool Stack;         // family stack: draw a pile of pages
};
HRESULT _CFontIcon_Create(const FontIconSpec* spec, REFIID riid, LPVOID* ppvOut);

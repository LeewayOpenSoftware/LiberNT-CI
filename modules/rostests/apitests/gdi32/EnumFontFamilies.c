/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Test for EnumFontFamilies[Ex]
 * PROGRAMMERS:     Thomas Faber <thomas.faber@reactos.org>
 */

#include "precomp.h"

static BYTE ContextContinue;
static BYTE ContextStop;

static int EnumProcCalls;

typedef int WRAP_ENUM_FONT_FAMILIES(_In_ HDC, _In_ PCWSTR, _In_ PVOID, _In_ LPARAM);
typedef WRAP_ENUM_FONT_FAMILIES *PWRAP_ENUM_FONT_FAMILIES;

static
int
WrapEnumFontFamiliesA(
    _In_ HDC hdc,
    _In_ PCWSTR Family,
    _In_ PVOID EnumProc,
    _In_ LPARAM lParam)
{
    char FamilyA[100];
    WideCharToMultiByte(CP_ACP, 0, Family, -1, FamilyA, sizeof(FamilyA), NULL, NULL);
    return EnumFontFamiliesA(hdc, FamilyA, EnumProc, lParam);
}

static
int
WrapEnumFontFamiliesW(
    _In_ HDC hdc,
    _In_ PCWSTR Family,
    _In_ PVOID EnumProc,
    _In_ LPARAM lParam)
{
    return EnumFontFamiliesW(hdc, Family, EnumProc, lParam);
}

static
int
WrapEnumFontFamiliesExA(
    _In_ HDC hdc,
    _In_ PCWSTR Family,
    _In_ PVOID EnumProc,
    _In_ LPARAM lParam)
{
    LOGFONTA lf;

    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = 0;
    WideCharToMultiByte(CP_ACP, 0, Family, -1, lf.lfFaceName, sizeof(lf.lfFaceName), NULL, NULL);
    return EnumFontFamiliesExA(hdc, &lf, EnumProc, lParam, 0);
}

static
int
WrapEnumFontFamiliesExW(
    _In_ HDC hdc,
    _In_ PCWSTR Family,
    _In_ PVOID EnumProc,
    _In_ LPARAM lParam)
{
    LOGFONTW lf;

    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = 0;
    StringCbCopyW(lf.lfFaceName, sizeof(lf.lfFaceName), Family);
    return EnumFontFamiliesExW(hdc, &lf, EnumProc, lParam, 0);
}

static
int
CALLBACK
EnumProcA(
    _In_ const LOGFONTA *elf,
    _In_ const TEXTMETRICA *ntm,
    _In_ DWORD FontType,
    _In_ LPARAM lParam)
{
    EnumProcCalls++;

    ok(lParam == (LPARAM)&ContextContinue ||
       lParam == (LPARAM)&ContextStop,
       "Context is %p, expected %p or %p\n",
       (PVOID)lParam, &ContextContinue, &ContextStop);

    return lParam == (LPARAM)&ContextContinue ? 7 : 0;
}

static
int
CALLBACK
EnumProcW(
    _In_ const LOGFONTW *elf,
    _In_ const TEXTMETRICW *ntm,
    _In_ DWORD FontType,
    _In_ LPARAM lParam)
{
    EnumProcCalls++;

    ok(lParam == (LPARAM)&ContextContinue ||
       lParam == (LPARAM)&ContextStop,
       "Context is %p, expected %p or %p\n",
       (PVOID)lParam, &ContextContinue, &ContextStop);

    return lParam == (LPARAM)&ContextContinue ? 7 : 0;
}

static
void
TestEnumFontFamilies(
    _In_ HDC hdc,
    _In_ PCWSTR FontName,
    _In_ BOOLEAN ExpectToFind)
{
    const struct
    {
        PWRAP_ENUM_FONT_FAMILIES Wrapper;
        PCSTR Name;
        BOOLEAN Wide;
    } *fun, functions[] =
    {
        { WrapEnumFontFamiliesA,    "EnumFontFamiliesA",    FALSE },
        { WrapEnumFontFamiliesW,    "EnumFontFamiliesW",    TRUE },
        { WrapEnumFontFamiliesExA,  "EnumFontFamiliesExA",  FALSE },
        { WrapEnumFontFamiliesExW,  "EnumFontFamiliesExW",  TRUE },
    };
    const struct
    {
        PVOID Context;
        PCSTR Description;
    } *ctx, contexts[] =
    {
        { &ContextContinue, "Continue" },
        { &ContextStop,     "Stop" },
    };
    int ret;
    DWORD error;
    unsigned iFunction;
    unsigned iContext;

    for (iContext = 0; iContext < _countof(contexts); iContext++)
    {
        ctx = &contexts[iContext];
        for (iFunction = 0; iFunction < _countof(functions); iFunction++)
        {
            fun = &functions[iFunction];
            EnumProcCalls = 0;
            SetLastError(0xdeadbeef);
            ret = fun->Wrapper(hdc,
                               FontName,
                               fun->Wide ? (PVOID)EnumProcW : (PVOID)EnumProcA,
                               (LPARAM)ctx->Context);
            error = GetLastError();
            ok(error == 0xdeadbeef, "[%s, %s, '%ls'] error is %lu\n", fun->Name, ctx->Description, FontName, error);
            if (ExpectToFind)
            {
                if (ctx->Context == &ContextContinue)
                {
                    ok(ret == 7, "[%s, %s, '%ls'] ret is %d, expected 7\n", fun->Name, ctx->Description, FontName, ret);
                    ok(EnumProcCalls >= 1, "[%s, %s, '%ls'] EnumProcCalls is %d\n", fun->Name, ctx->Description, FontName, EnumProcCalls);
                }
                else
                {
                    ok(ret == 0, "[%s, %s, '%ls'] ret is %d, expected 0\n", fun->Name, ctx->Description, FontName, ret);
                    ok(EnumProcCalls == 1, "[%s, %s, '%ls'] EnumProcCalls is %d\n", fun->Name, ctx->Description, FontName, EnumProcCalls);
                }
            }
            else
            {
                ok(ret == 1, "[%s, %s, '%ls'] ret is %d, expected 1\n", fun->Name, ctx->Description, FontName, ret);
                ok(EnumProcCalls == 0, "[%s, %s, '%ls'] EnumProcCalls is %d\n", fun->Name, ctx->Description, FontName, EnumProcCalls);
            }
        }
    }
}

static
int
CALLBACK
SubstEnumProc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD FontType, LPARAM lParam)
{
    const ENUMLOGFONTEXW *elf = (const ENUMLOGFONTEXW *)lf;

    trace("alias entry: face='%ls' full='%ls' style='%ls' type=0x%lx\n", lf->lfFaceName, elf->elfFullName,
          elf->elfStyle, FontType);
    (*(int *)lParam)++;
    return 1;
}

typedef struct { int Total; int Failed; WCHAR First[LF_FACESIZE]; } REALIZE_CTX;

static
int
CALLBACK
RealizeEnumProc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD FontType, LPARAM lParam)
{
    REALIZE_CTX *ctx = (REALIZE_CTX *)lParam;
    HDC hdc = CreateCompatibleDC(NULL);
    HFONT hfont, old;
    OUTLINETEXTMETRICW otm;

    if (FontType == RASTER_FONTTYPE || lf->lfFaceName[0] == '@')
    {
        DeleteDC(hdc);
        return 1;
    }
    /* GDI+ selects the enumerated logfont as is and needs the outline metrics of that font */
    hfont = CreateFontIndirectW(lf);
    old = SelectObject(hdc, hfont);
    otm.otmSize = sizeof(otm);
    ctx->Total++;
    if (!GetOutlineTextMetricsW(hdc, sizeof(otm), &otm) || GetFontData(hdc, 0x61656868, 0, NULL, 0) == GDI_ERROR)
    {
        if (!ctx->Failed++)
            wcscpy(ctx->First, lf->lfFaceName);
    }
    SelectObject(hdc, old);
    DeleteObject(hfont);
    DeleteDC(hdc);
    return 1;
}

static void PutBE16(BYTE *p, UINT v)
{
    p[0] = v >> 8; p[1] = v;
}

static void PutBE32(BYTE *p, UINT v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* A copy of a TrueType font whose name table only names the family */
static BYTE *MakeRenamedFont(const WCHAR *path, const char *name, DWORD *pSize)
{
    static const USHORT ids[] = { 1, 2, 4, 6 };
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD len, got = 0, numTables, i, nameLen, pos;
    BYTE *in = NULL, *out = NULL, *tab, *str;
    size_t n = strlen(name), j;

    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    len = GetFileSize(h, NULL);
    in = HeapAlloc(GetProcessHeap(), 0, len);
    if (!in || !ReadFile(h, in, len, &got, NULL) || got != len || len < 12)
        goto fail;
    numTables = (in[4] << 8) | in[5];
    if (12 + numTables * 16 > len)
        goto fail;
    nameLen = 6 + 12 * _countof(ids) + 2 * n * _countof(ids);
    out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len + nameLen + 4);
    if (!out)
        goto fail;
    memcpy(out, in, len);
    pos = (len + 3) & ~3;
    for (i = 0; i < numTables; i++)
    {
        BYTE *e = out + 12 + i * 16;

        if (!memcmp(e, "name", 4))
        {
            PutBE32(e + 8, pos);
            PutBE32(e + 12, nameLen);
            break;
        }
    }
    if (i == numTables)
        goto fail;
    tab = out + pos;
    PutBE16(tab + 2, _countof(ids));
    PutBE16(tab + 4, 6 + 12 * _countof(ids));
    str = tab + 6 + 12 * _countof(ids);
    for (i = 0; i < _countof(ids); i++)
    {
        BYTE *r = tab + 6 + 12 * i;

        PutBE16(r, 3);
        PutBE16(r + 2, 1);
        PutBE16(r + 4, 0x409);
        PutBE16(r + 6, ids[i]);
        PutBE16(r + 8, 2 * n);
        PutBE16(r + 10, str - (tab + 6 + 12 * _countof(ids)));
        for (j = 0; j < n; j++)
            PutBE16(str + 2 * j, name[j]);
        str += 2 * n;
    }
    HeapFree(GetProcessHeap(), 0, in);
    CloseHandle(h);
    *pSize = pos + nameLen;
    return out;

fail:
    CloseHandle(h);
    if (in) HeapFree(GetProcessHeap(), 0, in);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    return NULL;
}

static void PutLE16(BYTE *p, UINT v)
{
    p[0] = v; p[1] = v >> 8;
}

static void PutLE32(BYTE *p, UINT v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* A minimal Windows 3.0 FNT raster font: 8x8 cells for the characters 32..127 with the given face name */
static BYTE *MakeRasterFont(const char *name, DWORD *pSize)
{
    const UINT first = 32, last = 127, count = last - first + 2;
    const UINT table = 148, face = table + count * 6, bits = (face + 40 + 3) & ~3, size = bits + count * 8;
    BYTE *font = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    UINT i;

    if (!font)
        return NULL;
    PutLE16(font, 0x300);
    PutLE32(font + 2, size);
    PutLE16(font + 68, 8);          /* points */
    PutLE16(font + 70, 96);
    PutLE16(font + 72, 96);
    PutLE16(font + 74, 7);          /* ascent */
    PutLE16(font + 83, FW_NORMAL);
    font[85] = ANSI_CHARSET;
    PutLE16(font + 86, 0);          /* pixel width, variable */
    PutLE16(font + 88, 8);          /* pixel height */
    font[90] = FF_SWISS | 1;
    PutLE16(font + 91, 8);          /* average width */
    PutLE16(font + 93, 8);          /* maximum width */
    font[95] = first;
    font[96] = last;
    font[97] = 0;                   /* default char, relative to first */
    font[98] = 0;                   /* break char, relative to first */
    PutLE16(font + 99, 1);          /* width bytes */
    PutLE32(font + 105, face);
    PutLE32(font + 113, bits);
    PutLE32(font + 118, 0x2);       /* proportional, flags */
    for (i = 0; i < count; i++)
    {
        PutLE16(font + table + i * 6, 8);
        PutLE32(font + table + i * 6 + 2, bits + i * 8);
    }
    strcpy((char *)font + face, name);
    for (i = 0; i < count * 8; i++)
        font[bits + i] = (i & 1) ? 0x55 : 0xAA;
    *pSize = size;
    return font;
}

static BOOL WriteTempFile(const WCHAR *name, const BYTE *data, DWORD size, WCHAR *path)
{
    DWORD written;
    HANDLE h;

    GetTempPathW(MAX_PATH, path);
    wcscat(path, name);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!WriteFile(h, data, size, &written, NULL) || written != size)
    {
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);
    return TRUE;
}

static int CALLBACK CountRasterProc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM param)
{
    if (type & RASTER_FONTTYPE)
        (*(INT *)param)++;
    return 1;
}

static BOOL RealizesTrueType(HDC hdc, const WCHAR *name, BYTE precision, LONG height)
{
    LOGFONTW lf = { 0 };
    HFONT hfont, old;
    BOOL ret;

    lf.lfHeight = height;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = precision;
    wcscpy(lf.lfFaceName, name);
    hfont = CreateFontIndirectW(&lf);
    old = SelectObject(hdc, hfont);
    ret = GetFontData(hdc, 0x61656868, 0, NULL, 0) != GDI_ERROR; /* hhea: only TrueType has it */
    SelectObject(hdc, old);
    DeleteObject(hfont);
    return ret;
}

/* A raster and a TrueType font of one name: the output precision decides which one is realized */
static
void
TestSameNamePair(HDC hdc)
{
    static const DWORD flags[] = { FR_PRIVATE, 0 };
    static const WCHAR name[] = L"Zzz Pair";
    WCHAR srcPath[MAX_PATH], rasterPath[MAX_PATH] = L"", ttPath[MAX_PATH] = L"";
    LOGFONTW lf = { 0 };
    BYTE *raster = NULL, *tt = NULL;
    DWORD rasterSize, ttSize;
    INT rasters;
    UINT i;

    GetWindowsDirectoryW(srcPath, _countof(srcPath));
    wcscat(srcPath, L"\\Fonts\\arial.ttf");
    raster = MakeRasterFont("Zzz Pair", &rasterSize);
    tt = MakeRenamedFont(srcPath, "Zzz Pair", &ttSize);
    if (!raster || !tt || !WriteTempFile(L"zzzpair.fnt", raster, rasterSize, rasterPath) ||
        !WriteTempFile(L"zzzpair.ttf", tt, ttSize, ttPath))
    {
        skip("Cannot create the test fonts\n");
        goto done;
    }

    for (i = 0; i < _countof(flags); i++)
    {
        if (!AddFontResourceExW(rasterPath, flags[i], 0))
        {
            skip("Cannot add the raster font (flags 0x%lx)\n", flags[i]);
            continue;
        }

        rasters = 0;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy(lf.lfFaceName, name);
        EnumFontFamiliesExW(hdc, &lf, CountRasterProc, (LPARAM)&rasters, 0);
        if (!rasters)
        {
            skip("The raster font is not enumerated as raster (flags 0x%lx)\n", flags[i]);
            RemoveFontResourceExW(rasterPath, flags[i], 0);
            continue;
        }

        ok(!RealizesTrueType(hdc, name, OUT_RASTER_PRECIS, 8),
           "Raster face (flags 0x%lx) with OUT_RASTER_PRECIS realizes TrueType\n", flags[i]);

        if (AddFontResourceExW(ttPath, flags[i], 0))
        {
            /* A positive height is a cell height, a negative one a character height: both fit the 8 pixel cell */
            ok(!RealizesTrueType(hdc, name, OUT_RASTER_PRECIS, 8),
               "Pair (flags 0x%lx) with OUT_RASTER_PRECIS and height 8 realizes TrueType\n", flags[i]);
            ok(!RealizesTrueType(hdc, name, OUT_RASTER_PRECIS, -8),
               "Pair (flags 0x%lx) with OUT_RASTER_PRECIS and height -8 realizes TrueType\n", flags[i]);
            ok(RealizesTrueType(hdc, name, OUT_TT_ONLY_PRECIS, -8),
               "Pair (flags 0x%lx) with OUT_TT_ONLY_PRECIS does not realize TrueType\n", flags[i]);
            ok(RealizesTrueType(hdc, name, OUT_OUTLINE_PRECIS, -8),
               "Pair (flags 0x%lx) with OUT_OUTLINE_PRECIS does not realize TrueType\n", flags[i]);
            RemoveFontResourceExW(ttPath, flags[i], 0);
        }
        else
            skip("Cannot add the TrueType font (flags 0x%lx)\n", flags[i]);
        RemoveFontResourceExW(rasterPath, flags[i], 0);
    }

done:
    DeleteFileW(rasterPath);
    DeleteFileW(ttPath);
    HeapFree(GetProcessHeap(), 0, raster);
    HeapFree(GetProcessHeap(), 0, tt);
}

/* The output precision decides between raster and TrueType candidates of the same family */
static
void
TestOutputPrecision(HDC hdc)
{
    static const BYTE precisions[] = { OUT_DEFAULT_PRECIS, OUT_TT_PRECIS, OUT_STROKE_PRECIS, OUT_TT_ONLY_PRECIS,
                                       OUT_OUTLINE_PRECIS, OUT_SCREEN_OUTLINE_PRECIS };
    LOGFONTW lf = { 0 };
    TEXTMETRICW tm;
    HFONT hfont, old;
    UINT i;

    for (i = 0; i < _countof(precisions); i++)
    {
        lf.lfHeight = -12;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = precisions[i];
        wcscpy(lf.lfFaceName, L"Arial");
        hfont = CreateFontIndirectW(&lf);
        old = SelectObject(hdc, hfont);
        ok(GetTextMetricsW(hdc, &tm) && (tm.tmPitchAndFamily & TMPF_TRUETYPE),
           "Arial with precision %u is not TrueType\n", precisions[i]);
        ok(GetFontData(hdc, 0x61656868, 0, NULL, 0) != GDI_ERROR, "Arial with precision %u has no font data\n", precisions[i]);
        SelectObject(hdc, old);
        DeleteObject(hfont);
    }
}

/* An enumerated logfont must realize as an outline font again */
static
void
TestEnumeratedLogfontRealizes(HDC hdc)
{
    LOGFONTW lf = { 0 };
    REALIZE_CTX ctx = { 0 };

    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(hdc, &lf, RealizeEnumProc, (LPARAM)&ctx, 0);
    ok(ctx.Total > 0, "No outline fonts enumerated\n");
    ok(ctx.Failed == 0, "%d of %d enumerated logfonts do not realize as an outline font, first '%ls'\n",
       ctx.Failed, ctx.Total, ctx.First);
}

/* A FontSubstitutes alias is enumerated like the family it maps to */
static
void
TestEnumFontSubstitute(HDC hdc)
{
    LOGFONTW lf = { 0 };
    int count = 0;

    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    EnumFontFamiliesExW(hdc, &lf, SubstEnumProc, (LPARAM)&count, 0);
    ok(count >= 1, "EnumFontFamiliesExW(Segoe UI) enumerated %d fonts\n", count);

    count = 0;
    wcscpy(lf.lfFaceName, L"Selawik");
    EnumFontFamiliesExW(hdc, &lf, SubstEnumProc, (LPARAM)&count, 0);
    ok(count >= 1, "EnumFontFamiliesExW(Selawik) enumerated %d fonts\n", count);

    count = 0;
    lf.lfFaceName[0] = 0;
    EnumFontFamiliesExW(hdc, &lf, SubstEnumProc, (LPARAM)&count, 0);
    trace("all families: %d entries\n", count);
    TestEnumeratedLogfontRealizes(hdc);
    TestOutputPrecision(hdc);
    TestSameNamePair(hdc);
}

START_TEST(EnumFontFamilies)
{
    HDC hdc;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc)
    {
        skip("No DC\n");
        return;
    }

    TestEnumFontFamilies(hdc, L"ThisFontDoesNotExist", FALSE);
    TestEnumFontSubstitute(hdc);
    /* Basic fonts that should be installed */
    TestEnumFontFamilies(hdc, L"MS Sans Serif", TRUE);
    TestEnumFontFamilies(hdc, L"Tahoma", TRUE);
    TestEnumFontFamilies(hdc, L"System", TRUE);
    /* Show case insensitivity */
    TestEnumFontFamilies(hdc, L"tahOmA", TRUE);
    /* Special fonts that we have a hack for in win32k ;) */
    TestEnumFontFamilies(hdc, L"Marlett", TRUE);
    TestEnumFontFamilies(hdc, L"Symbol", TRUE);
    TestEnumFontFamilies(hdc, L"VGA", FALSE);

    DeleteDC(hdc);
}


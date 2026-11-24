/*
 * PROJECT:     LiberNT Usermode Font Driver Host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host process for user-mode font drivers, serving win32k's UMFD channel
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winternl.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <reactos/umfd.h>
#include <ft2build.h>
#include FT_FREETYPE_H

INT WINAPI NtGdiExtEscape(HDC hdc, PWCHAR pDriver, INT nDriver, INT Escape,
                          INT InSize, LPSTR InData, INT OutSize, LPSTR OutData);

#define VALIDATE_PPEM 32

static FT_Library g_Library;
static UMFD_HEADER g_Request[64 * 1024 / sizeof(UMFD_HEADER)];

static struct
{
    UMFD_HEADER Header;
    UMFD_LOAD_REPLY Load;
} g_Reply;

/* Opens one face and renders each of its glyphs once at a fixed size */
static BOOL
ValidateFace(const FT_Byte *pFile, FT_Long cbFile, FT_Long Index, FT_Long *pNumFaces)
{
    FT_Face Face;
    FT_Long Glyph;
    BOOL Ok = TRUE;

    if (FT_New_Memory_Face(g_Library, pFile, cbFile, Index, &Face))
        return FALSE;
    *pNumFaces = Face->num_faces;

    if (FT_IS_SCALABLE(Face) && FT_Set_Pixel_Sizes(Face, 0, VALIDATE_PPEM))
        Ok = FALSE;
    else if (!FT_IS_SCALABLE(Face) && Face->num_fixed_sizes > 0 && FT_Select_Size(Face, 0))
        Ok = FALSE;

    for (Glyph = 0; Ok && Glyph < Face->num_glyphs; ++Glyph)
    {
        if (FT_Load_Glyph(Face, (FT_UInt)Glyph, FT_LOAD_DEFAULT) ||
            FT_Render_Glyph(Face->glyph, FT_RENDER_MODE_NORMAL))
        {
            Ok = FALSE;
        }
    }

    FT_Done_Face(Face);
    return Ok;
}

/* Returns the NTSTATUS for the reply header */
static LONG
HandleLoad(const UMFD_HEADER *Request, UMFD_LOAD_REPLY *Result)
{
    const UMFD_LOAD_REQUEST *Load = (const UMFD_LOAD_REQUEST *)(Request + 1);
    const FT_Byte *pFile;
    FT_Long NumFaces = 0, Face, Count;

    if (Request->cbPayload != sizeof(*Load) || !(Load->Flags & UMFD_LOAD_VALIDATE_ONLY) ||
        Load->FaceIndex != -1 || Load->ViewSize == 0 || Load->ViewSize > MAXLONG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* win32k's read-only copy of the file, mapped into this process */
    pFile = (const FT_Byte *)(ULONG_PTR)Load->ViewBase;
    if (!ValidateFace(pFile, (FT_Long)Load->ViewSize, 0, &NumFaces) ||
        NumFaces <= 0 || NumFaces > UMFD_MAX_FACES)
    {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    for (Face = 1; Face < NumFaces; ++Face)
    {
        if (!ValidateFace(pFile, (FT_Long)Load->ViewSize, Face, &Count))
            return STATUS_INVALID_IMAGE_FORMAT;
    }

    Result->NumFaces = (ULONG)NumFaces;
    Result->Reserved = 0;
    return STATUS_SUCCESS;
}

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    INT cbReply = sizeof(UMFD_HEADER);
    INT Result;
    DWORD Error = 0;

    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    /* The host defines no options */
    if (lpCmdLine && *lpCmdLine)
        return ERROR_INVALID_PARAMETER;

    if (FT_Init_FreeType(&g_Library))
        return ERROR_NOT_ENOUGH_MEMORY;

    g_Reply.Header.Magic = UMFD_MAGIC;
    g_Reply.Header.Version = UMFD_VERSION;
    g_Reply.Header.RequestId = 0;
    g_Reply.Header.cbPayload = 0;

    for (;;)
    {
        Result = NtGdiExtEscape(NULL, NULL, 0, UMFD_ESCAPE_SERVE, cbReply, (LPSTR)&g_Reply,
                                sizeof(g_Request), (LPSTR)g_Request);
        if (Result <= 0)
        {
            Error = GetLastError();
            if (Error == WAIT_TIMEOUT)
            {
                cbReply = sizeof(UMFD_HEADER);
                g_Reply.Header.RequestId = 0;
                g_Reply.Header.cbPayload = 0;
                continue;
            }
            break;
        }

        g_Reply.Header.Op = g_Request->Op;
        g_Reply.Header.RequestId = g_Request->RequestId;
        g_Reply.Header.FontId = g_Request->FontId;
        g_Reply.Header.cbPayload = 0;
        if (g_Request->Op == UMFD_OP_LOAD)
        {
            g_Reply.Header.Status = HandleLoad(g_Request, &g_Reply.Load);
            if (g_Reply.Header.Status >= 0)
                g_Reply.Header.cbPayload = sizeof(g_Reply.Load);
        }
        else
        {
            g_Reply.Header.Status = STATUS_NOT_SUPPORTED;
        }
        cbReply = (INT)(sizeof(UMFD_HEADER) + g_Reply.Header.cbPayload);
    }

    FT_Done_FreeType(g_Library);

    /* Not started as the registered host, or the channel is off or gone: do not pretend to serve */
    if (Error == ERROR_NOT_SUPPORTED)
        OutputDebugStringW(L"fontdrvhost: win32k's user-mode font driver channel is off; fonts stay on the kernel path\n");
    else if (Error == ERROR_ACCESS_DENIED)
        OutputDebugStringW(L"fontdrvhost: not registered by the session launcher\n");
    return Error ? (int)Error : ERROR_NOT_SUPPORTED;
}

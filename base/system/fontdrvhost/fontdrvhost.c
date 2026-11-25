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
#include <pseh/pseh2.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

INT WINAPI NtGdiExtEscape(HDC hdc, PWCHAR pDriver, INT nDriver, INT Escape,
                          INT InSize, LPSTR InData, INT OutSize, LPSTR OutData);

#define VALIDATE_PPEM 32
#define VALIDATE_MAX_GLYPHS (4 * 1024 * 1024)   /* summed over every face of one file */

typedef struct _BUDGET
{
    DWORD Start;
    ULONG Glyphs;
} BUDGET;

static UMFD_HEADER g_Request[64 * 1024 / sizeof(UMFD_HEADER)];

static struct
{
    UMFD_HEADER Header;
    UMFD_LOAD_REPLY Load;
} g_Reply;

/* The tree's static FreeType leaves its debug output to each consumer */
void
FT_Message(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    _vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = 0;
    OutputDebugStringA(buffer);
}

/* An assertion inside FreeType becomes an exception, so HandleLoad refuses that one file */
void
FT_Panic(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    _vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = 0;
    OutputDebugStringA(buffer);
    RaiseException(STATUS_ASSERTION_FAILURE, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

/* Fail-closed: a file too slow or too large to check is refused, never waved through */
static BOOL
OverBudget(BUDGET *Budget)
{
    return GetTickCount() - Budget->Start > UMFD_LOAD_BUDGET_MS;
}

/* Opens one face and renders each of its glyphs once at a fixed size */
static BOOL
ValidateFace(FT_Library Library, const FT_Byte *pFile, FT_Long cbFile, FT_Long Index,
             BUDGET *Budget, FT_Long *pNumFaces)
{
    FT_Face Face;
    FT_Long Glyph;
    BOOL Ok = TRUE;

    if (OverBudget(Budget) || FT_New_Memory_Face(Library, pFile, cbFile, Index, &Face))
        return FALSE;
    *pNumFaces = Face->num_faces;

    if (FT_IS_SCALABLE(Face) && FT_Set_Pixel_Sizes(Face, 0, VALIDATE_PPEM))
        Ok = FALSE;
    else if (!FT_IS_SCALABLE(Face) && Face->num_fixed_sizes > 0 && FT_Select_Size(Face, 0))
        Ok = FALSE;

    for (Glyph = 0; Ok && Glyph < Face->num_glyphs; ++Glyph)
    {
        if (++Budget->Glyphs > VALIDATE_MAX_GLYPHS || OverBudget(Budget) ||
            FT_Load_Glyph(Face, (FT_UInt)Glyph, FT_LOAD_DEFAULT) ||
            FT_Render_Glyph(Face->glyph, FT_RENDER_MODE_NORMAL))
        {
            Ok = FALSE;
        }
    }

    FT_Done_Face(Face);
    return Ok;
}

#ifdef UMFD_TEST_HOOKS
#include <reactos/umfdtest.h>

static const BYTE *g_StalledView;

/* Test images only: TRUE if the view is a test command, with the reply it gets */
static BOOL
TestHook(const BYTE *pView, ULONG64 cbView, ULONG *pNumFaces, LONG *pStatus)
{
    const UMFD_TEST_BLOB *Blob = (const UMFD_TEST_BLOB *)pView;
    MEMORY_BASIC_INFORMATION Info;
    volatile BYTE Sum = 0;
    ULONG64 i;

    /* win32k unmaps a stalled request's view at the SERVE after it, so it must be gone by now */
    if (g_StalledView && g_StalledView != pView &&
        (!VirtualQuery(g_StalledView, &Info, sizeof(Info)) || Info.State != MEM_FREE))
    {
        ExitProcess(UMFD_TEST_EXIT_VIEW_LEAKED);
    }
    g_StalledView = NULL;

    if (cbView < sizeof(*Blob) || memcmp(Blob->Magic, UMFD_TEST_MAGIC, sizeof(Blob->Magic)) != 0)
        return FALSE;

    *pStatus = STATUS_SUCCESS;
    *pNumFaces = 1;
    switch (Blob->Command)
    {
        case UMFD_TEST_CRASH:
            /* Param is 0: a real access violation the compiler cannot see coming */
            *(volatile LONG *)(ULONG_PTR)Blob->Param = 0;
            break;

        case UMFD_TEST_STALL:
            Sleep(Blob->Param);
            /* By now win32k has given up on this request; the view must still be mapped */
            _SEH2_TRY
            {
                for (i = 0; i < cbView; i += 4096)
                    Sum += pView[i];
                Sum += pView[cbView - 1];
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                ExitProcess(UMFD_TEST_EXIT_VIEW_GONE);
            }
            _SEH2_END;
            g_StalledView = pView;
            break;

        case UMFD_TEST_FREEZE:
            Sleep(Blob->Param);
            ExitProcess(UMFD_TEST_EXIT_FROZEN);

        case UMFD_TEST_EXIT:
            ExitProcess(UMFD_TEST_EXIT_ON_REQUEST);

        default:
            *pStatus = STATUS_INVALID_PARAMETER;
            break;
    }
    return TRUE;
}
#endif

static LONG
ValidateFile(FT_Library Library, const FT_Byte *pFile, FT_Long cbFile, ULONG *pNumFaces)
{
    BUDGET Budget = { GetTickCount(), 0 };
    FT_Long NumFaces = 0, Face, Count;

    if (!ValidateFace(Library, pFile, cbFile, 0, &Budget, &NumFaces) ||
        NumFaces <= 0 || NumFaces > UMFD_MAX_FACES)
    {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    for (Face = 1; Face < NumFaces; ++Face)
    {
        if (!ValidateFace(Library, pFile, cbFile, Face, &Budget, &Count))
            return STATUS_INVALID_IMAGE_FORMAT;
    }
    *pNumFaces = (ULONG)NumFaces;
    return STATUS_SUCCESS;
}

/* Returns the NTSTATUS for the reply header */
static LONG
HandleLoad(const UMFD_HEADER *Request, UMFD_LOAD_REPLY *Result)
{
    const UMFD_LOAD_REQUEST *Load = (const UMFD_LOAD_REQUEST *)(Request + 1);
    FT_Library Library;
    ULONG NumFaces = 0;
    LONG Status;
    BOOL Faulted = FALSE;

    if (Request->cbPayload != sizeof(*Load) || !(Load->Flags & UMFD_LOAD_VALIDATE_ONLY) ||
        Load->FaceIndex != -1 || Load->ViewSize == 0 || Load->ViewSize > MAXLONG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* A library per file, so a fault in one cannot leave state behind for the next */
    if (FT_Init_FreeType(&Library))
        return STATUS_NO_MEMORY;

    /* win32k's read-only copy of the file, mapped into this process; a fault costs this file only */
    _SEH2_TRY
    {
#ifdef UMFD_TEST_HOOKS
        if (!TestHook((const BYTE *)(ULONG_PTR)Load->ViewBase, Load->ViewSize, &NumFaces, &Status))
#endif
        Status = ValidateFile(Library, (const FT_Byte *)(ULONG_PTR)Load->ViewBase,
                              (FT_Long)Load->ViewSize, &NumFaces);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = STATUS_INVALID_IMAGE_FORMAT;
        Faulted = TRUE;
    }
    _SEH2_END;

    /* After a fault its state cannot be trusted: abandoned, not freed */
    if (!Faulted)
        FT_Done_FreeType(Library);
    if (Status < 0)
        return Status;

    Result->NumFaces = NumFaces;
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

    /* Not started as the registered host, or the channel is off or gone: do not pretend to serve */
    if (Error == ERROR_NOT_SUPPORTED)
        OutputDebugStringW(L"fontdrvhost: win32k's user-mode font driver channel is off; fonts stay on the kernel path\n");
    else if (Error == ERROR_ACCESS_DENIED)
        OutputDebugStringW(L"fontdrvhost: not registered by the session launcher\n");
    return Error ? (int)Error : ERROR_NOT_SUPPORTED;
}

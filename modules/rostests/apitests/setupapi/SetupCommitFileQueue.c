/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Source subdirectories of copies whose media the queue callback skips
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <apitest.h>
#include <winuser.h>
#include <winreg.h>
#include <setupapi.h>
#include <strsafe.h>

#define COPY_COUNT 6

typedef struct _COMMIT_CONTEXT
{
    WCHAR Root[MAX_PATH];
    WCHAR SubDirs[COPY_COUNT][64];
    ULONG NeedMedia;
    ULONG Unexpected;
} COMMIT_CONTEXT;

static UINT CALLBACK
SkipMediaCallback(PVOID Context, UINT Notification, UINT_PTR Param1, UINT_PTR Param2)
{
    COMMIT_CONTEXT *Ctx = Context;
    const SOURCE_MEDIA_W *Media;
    WCHAR Expected[MAX_PATH];

    switch (Notification)
    {
        case SPFILENOTIFY_STARTQUEUE:
        case SPFILENOTIFY_STARTSUBQUEUE:
            return TRUE;
        case SPFILENOTIFY_ENDQUEUE:
        case SPFILENOTIFY_ENDSUBQUEUE:
            return 0;
        case SPFILENOTIFY_NEEDMEDIA:
            Media = (const SOURCE_MEDIA_W *)Param1;
            trace("NEEDMEDIA %lu path '%ls' file '%ls'\n", Ctx->NeedMedia, Media->SourcePath, Media->SourceFile);
            ok(lstrlenW(Media->SourcePath) < MAX_PATH, "NEEDMEDIA %lu: source path of %d characters\n",
               Ctx->NeedMedia, lstrlenW(Media->SourcePath));
            if (Ctx->NeedMedia < COPY_COUNT)
            {
                StringCchPrintfW(Expected, ARRAYSIZE(Expected), L"%s\\%s", Ctx->Root, Ctx->SubDirs[Ctx->NeedMedia]);
                ok(!lstrcmpiW(Media->SourcePath, Expected), "NEEDMEDIA %lu: source path '%ls', expected '%ls'\n",
                   Ctx->NeedMedia, Media->SourcePath, Expected);
                ok(!lstrcmpiW(Media->SourceFile, L"missing.bin"), "NEEDMEDIA %lu: source file '%ls'\n",
                   Ctx->NeedMedia, Media->SourceFile);
            }
            Ctx->NeedMedia++;
            return FILEOP_SKIP;
        default:
            trace("notification 0x%x\n", Notification);
            Ctx->Unexpected++;
            return FILEOP_SKIP;
    }
}

START_TEST(SetupCommitFileQueue)
{
    WCHAR Temp[MAX_PATH], Dest[MAX_PATH], Target[32];
    COMMIT_CONTEXT Ctx;
    HSPFILEQ Queue;
    ULONG Index;

    ZeroMemory(&Ctx, sizeof(Ctx));
    GetTempPathW(ARRAYSIZE(Temp), Temp);
    StringCchPrintfW(Ctx.Root, ARRAYSIZE(Ctx.Root), L"%ssetupq_root", Temp);
    StringCchPrintfW(Dest, ARRAYSIZE(Dest), L"%ssetupq_dest", Temp);
    CreateDirectoryW(Ctx.Root, NULL);
    CreateDirectoryW(Dest, NULL);

    Queue = SetupOpenFileQueue();
    ok(Queue != INVALID_HANDLE_VALUE, "SetupOpenFileQueue failed: %lu\n", GetLastError());
    if (Queue == INVALID_HANDLE_VALUE)
        return;

    for (Index = 0; Index < COPY_COUNT; Index++)
    {
        StringCchPrintfW(Ctx.SubDirs[Index], ARRAYSIZE(Ctx.SubDirs[Index]),
                         L"subdirectory_%lu_with_a_name_long_enough_to_fill_the_root_buffer", Index);
        StringCchPrintfW(Target, ARRAYSIZE(Target), L"copy%lu.bin", Index);
        ok(SetupQueueCopyW(Queue, Ctx.Root, Ctx.SubDirs[Index], L"missing.bin", NULL, NULL, Dest, Target, 0),
           "SetupQueueCopyW %lu failed: %lu\n", Index, GetLastError());
    }

    ok(SetupCommitFileQueueW(NULL, Queue, SkipMediaCallback, &Ctx), "SetupCommitFileQueueW failed: %lu\n",
       GetLastError());
    ok_long(Ctx.NeedMedia, COPY_COUNT);
    ok_long(Ctx.Unexpected, 0);

    SetupCloseFileQueue(Queue);
    RemoveDirectoryW(Dest);
    RemoveDirectoryW(Ctx.Root);
}

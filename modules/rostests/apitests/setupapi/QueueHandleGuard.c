/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The public file-queue entry points must reject a NULL or
 *              INVALID_HANDLE_VALUE queue handle cleanly instead of crashing,
 *              each with the return value and last-error Windows uses.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <windows.h>
#include <setupapi.h>

/* These entry points are exported by setupapi but not declared in the public SDK
   setupapi.h; declare them here as the loader resolves them by name. */
BOOL  WINAPI SetupGetFileQueueCount( HSPFILEQ handle, UINT op, PUINT result );
BOOL  WINAPI SetupGetFileQueueFlags( HSPFILEQ handle, PDWORD flags );
BOOL  WINAPI SetupSetFileQueueFlags( HSPFILEQ handle, DWORD mask, DWORD flags );
DWORD WINAPI pSetupGetQueueFlags( HSPFILEQ handle );
BOOL  WINAPI pSetupSetQueueFlags( HSPFILEQ handle, DWORD flags );

/* Windows 11 return / last-error table for an invalid queue handle (measured):
     Copy / CopyIndirect             -> FALSE + ERROR_INVALID_PARAMETER
     DefaultCopy / CopySection       -> FALSE + ERROR_INVALID_HANDLE
     Close / Commit / Scan / Count /
       Get / SetFileQueueFlags       -> FALSE + ERROR_INVALID_HANDLE
     QueueDelete / QueueRename       -> hardening: Windows faults; we return FALSE + ERROR_INVALID_HANDLE
     pSetupGetQueueFlags             -> 0, last error untouched (matches Windows)
     pSetupSetQueueFlags             -> FALSE; hardening: Windows faults (last error ends up
                                        0xC0000005 from its SEH); we return cleanly and leave
                                        the last error untouched. */

static void test_bad_queue( HSPFILEQ bad, const char *tag )
{
    SP_FILE_COPY_PARAMS_W copy;
    DWORD flags, prev;
    UINT count;
    BOOL ret;

    /* --- Copy / CopyIndirect: FALSE + ERROR_INVALID_PARAMETER (Windows 11) --- */
    SetLastError( 0xdeadbeef );
    ret = SetupQueueCopyW( bad, L"root", NULL, L"src.sys", NULL, NULL, L"dir", L"dst.sys", 0 );
    ok( !ret, "%s: SetupQueueCopyW should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_PARAMETER,
        "%s: SetupQueueCopyW error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_PARAMETER );

    ZeroMemory( &copy, sizeof(copy) );
    copy.cbSize        = sizeof(copy);
    copy.QueueHandle   = bad;
    copy.SourceRootPath = L"root";
    copy.SourceFilename = L"src.sys";
    copy.TargetFilename = L"dst.sys";
    SetLastError( 0xdeadbeef );
    ret = SetupQueueCopyIndirectW( &copy );
    ok( !ret, "%s: SetupQueueCopyIndirectW should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_PARAMETER,
        "%s: SetupQueueCopyIndirectW error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_PARAMETER );

    /* DefaultCopyW and CopySectionW are higher-level: Windows 11 reports the bad queue
       as ERROR_INVALID_HANDLE, not ERROR_INVALID_PARAMETER. Our guard runs before the
       hinf work, so a NULL hinf does not shadow the handle result. */
    SetLastError( 0xdeadbeef );
    ret = SetupQueueDefaultCopyW( bad, NULL, L"root", L"src.sys", L"dst.sys", 0 );
    ok( !ret, "%s: SetupQueueDefaultCopyW should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupQueueDefaultCopyW error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    SetLastError( 0xdeadbeef );
    ret = SetupQueueCopySectionW( bad, L"root", NULL, NULL, L"Section", 0 );
    ok( !ret, "%s: SetupQueueCopySectionW should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupQueueCopySectionW error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    /* --- Delete / Rename: FALSE + ERROR_INVALID_HANDLE (hardened; Windows crashes) --- */
    SetLastError( 0xdeadbeef );
    ret = SetupQueueDeleteW( bad, L"dir", L"file.sys" );
    ok( !ret, "%s: SetupQueueDeleteW should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupQueueDeleteW error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    SetLastError( 0xdeadbeef );
    ret = SetupQueueRenameW( bad, L"dir", L"old.sys", L"dir", L"new.sys" );
    ok( !ret, "%s: SetupQueueRenameW should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupQueueRenameW error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    /* --- Close / Commit / Scan / Count / Get / Set: FALSE + ERROR_INVALID_HANDLE --- */
    SetLastError( 0xdeadbeef );
    ret = SetupCloseFileQueue( bad );
    ok( !ret, "%s: SetupCloseFileQueue should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupCloseFileQueue error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    SetLastError( 0xdeadbeef );
    ret = SetupCommitFileQueueW( NULL, bad, NULL, NULL );
    ok( !ret, "%s: SetupCommitFileQueueW should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupCommitFileQueueW error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    flags = 0;
    SetLastError( 0xdeadbeef );
    ret = SetupScanFileQueueW( bad, SPQ_SCAN_USE_CALLBACK, NULL, NULL, NULL, &flags );
    ok( !ret, "%s: SetupScanFileQueueW should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupScanFileQueueW error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    count = 0;
    SetLastError( 0xdeadbeef );
    ret = SetupGetFileQueueCount( bad, FILEOP_COPY, &count );
    ok( !ret, "%s: SetupGetFileQueueCount should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupGetFileQueueCount error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    flags = 0;
    SetLastError( 0xdeadbeef );
    ret = SetupGetFileQueueFlags( bad, &flags );
    ok( !ret, "%s: SetupGetFileQueueFlags should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupGetFileQueueFlags error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    SetLastError( 0xdeadbeef );
    ret = SetupSetFileQueueFlags( bad, 0, 0 );
    ok( !ret, "%s: SetupSetFileQueueFlags should fail\n", tag );
    ok( GetLastError() == ERROR_INVALID_HANDLE,
        "%s: SetupSetFileQueueFlags error %lu, expected %u\n", tag, GetLastError(), ERROR_INVALID_HANDLE );

    /* pSetupGetQueueFlags: 0 with the last error untouched, matching Windows. */
    SetLastError( 0xdeadbeef );
    flags = pSetupGetQueueFlags( bad );
    prev = GetLastError();
    ok( flags == 0, "%s: pSetupGetQueueFlags returned %lu, expected 0\n", tag, flags );
    ok( prev == 0xdeadbeef, "%s: pSetupGetQueueFlags touched last error (%lu)\n", tag, prev );

    /* pSetupSetQueueFlags: FALSE. Windows faults here and its SEH leaves 0xC0000005 as the
       last error; our hardened path returns cleanly and leaves the last error untouched.
       This asserts our deterministic behaviour, which is a deviation from Windows, not parity. */
    SetLastError( 0xdeadbeef );
    ret = pSetupSetQueueFlags( bad, 0 );
    prev = GetLastError();
    ok( !ret, "%s: pSetupSetQueueFlags should fail\n", tag );
    ok( prev == 0xdeadbeef, "%s: pSetupSetQueueFlags touched last error (%lu)\n", tag, prev );
}

START_TEST(QueueHandleGuard)
{
    test_bad_queue( NULL, "NULL" );
    test_bad_queue( INVALID_HANDLE_VALUE, "INVALID_HANDLE_VALUE" );
}

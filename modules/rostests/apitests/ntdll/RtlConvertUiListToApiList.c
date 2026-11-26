/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for the legacy RtlConvertUiListToApiList export
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *FN_RtlConvertUiListToApiList)(PUNICODE_STRING, PUNICODE_STRING, BOOLEAN);

START_TEST(RtlConvertUiListToApiList)
{
    FN_RtlConvertUiListToApiList pRtlConvertUiListToApiList;
    UNICODE_STRING UiList = RTL_CONSTANT_STRING(L"a, b c");
    UNICODE_STRING ApiList;
    NTSTATUS Status;

    /* Windows 11 no longer exports it; there is no modern reference */
    pRtlConvertUiListToApiList = (FN_RtlConvertUiListToApiList)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                              "RtlConvertUiListToApiList");
    if (!pRtlConvertUiListToApiList)
    {
        skip("RtlConvertUiListToApiList is not exported (as on Windows 11)\n");
        return;
    }

    /* Orosi keeps the export for old binaries but reports no fake success */
    RtlInitUnicodeString(&ApiList, L"stale");
    Status = pRtlConvertUiListToApiList(&UiList, &ApiList, TRUE);
    ok_ntstatus(Status, STATUS_NOT_IMPLEMENTED);
    ok(ApiList.Buffer == NULL, "ApiList.Buffer %p\n", ApiList.Buffer);
    ok_int(ApiList.Length, 0);
    ok_int(ApiList.MaximumLength, 0);
}

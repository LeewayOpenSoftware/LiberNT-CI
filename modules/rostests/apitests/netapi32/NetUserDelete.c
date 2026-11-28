/*
 * PROJECT:     LiberNT netapi32.dll API Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for deleting and re-creating local accounts with NetUserDel and NetUserAdd
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <lm.h>

#define TEST_USER L"LntUserDelTest"
#define TEST_PASSWORD L"Lnt!UserDel#2026q"

static
NET_API_STATUS
AddTestUser(VOID)
{
    USER_INFO_1 Info = { 0 };

    Info.usri1_name = (PWSTR)TEST_USER;
    Info.usri1_password = (PWSTR)TEST_PASSWORD;
    Info.usri1_priv = USER_PRIV_USER;
    Info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    return NetUserAdd(NULL, 1, (LPBYTE)&Info, NULL);
}

START_TEST(NetUserDelete)
{
    NET_API_STATUS Status;
    PUSER_INFO_1 Info = NULL;
    int i;

    /* Remove a leftover from an earlier run */
    NetUserDel(NULL, TEST_USER);

    for (i = 0; i < 2; i++)
    {
        Status = AddTestUser();
        ok(Status == NERR_Success, "NetUserAdd returned %lu in round %d\n", Status, i);
        if (Status != NERR_Success)
            return;

        Status = NetUserGetInfo(NULL, TEST_USER, 1, (LPBYTE *)&Info);
        ok(Status == NERR_Success, "NetUserGetInfo returned %lu in round %d\n", Status, i);
        if (Info)
            NetApiBufferFree(Info);
        Info = NULL;

        /* Deleting must work for a user without any alias membership of its own */
        Status = NetUserDel(NULL, TEST_USER);
        ok(Status == NERR_Success, "NetUserDel returned %lu in round %d\n", Status, i);

        Status = NetUserGetInfo(NULL, TEST_USER, 1, (LPBYTE *)&Info);
        ok(Status == NERR_UserNotFound, "NetUserGetInfo returned %lu after the delete in round %d\n", Status, i);
        if (Info)
            NetApiBufferFree(Info);
        Info = NULL;
    }

    /* Deleting an account that does not exist */
    Status = NetUserDel(NULL, TEST_USER);
    ok(Status == NERR_UserNotFound, "NetUserDel returned %lu for a missing user\n", Status);
}

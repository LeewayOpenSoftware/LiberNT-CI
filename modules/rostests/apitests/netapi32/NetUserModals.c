/*
 * PROJECT:     LiberNT netapi32.dll API Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for NetUserModalsSet and the minimum password length it sets
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <lm.h>

#define TEST_USER  L"LntModalsTest"
#define TEST_USER2 L"LntModalsAdd"
#define LONG_PW    L"Lnt!Modals#2026q"

static
PUSER_MODALS_INFO_0
GetModals0(VOID)
{
    PUSER_MODALS_INFO_0 Info = NULL;
    NET_API_STATUS Status;

    Status = NetUserModalsGet(NULL, 0, (LPBYTE *)&Info);
    ok(Status == NERR_Success, "NetUserModalsGet(0) returned %lu\n", Status);
    return (Status == NERR_Success) ? Info : NULL;
}

static
NET_API_STATUS
SetPassword(PCWSTR User, PCWSTR Password)
{
    USER_INFO_1003 Info;

    Info.usri1003_password = (PWSTR)Password;
    return NetUserSetInfo(NULL, User, 1003, (LPBYTE)&Info, NULL);
}

START_TEST(NetUserModals)
{
    PUSER_MODALS_INFO_0 Saved, Info;
    USER_MODALS_INFO_1001 MinLength;
    USER_MODALS_INFO_1005 History;
    USER_INFO_1 Add = { 0 };
    PUSER_INFO_1 UserInfo = NULL;
    NET_API_STATUS Status;
    DWORD ParmErr;

    Saved = GetModals0();
    if (Saved == NULL)
        return;

    /* Level 1001 changes only the minimum length */
    MinLength.usrmod1001_min_passwd_len = 8;
    ParmErr = 0xdead;
    Status = NetUserModalsSet(NULL, 1001, (LPBYTE)&MinLength, &ParmErr);
    ok(Status == NERR_Success, "NetUserModalsSet(1001) returned %lu\n", Status);
    Info = GetModals0();
    if (Info != NULL)
    {
        ok(Info->usrmod0_min_passwd_len == 8, "min_passwd_len is %lu\n", Info->usrmod0_min_passwd_len);
        ok(Info->usrmod0_max_passwd_age == Saved->usrmod0_max_passwd_age, "max_passwd_age changed to %lu\n", Info->usrmod0_max_passwd_age);
        ok(Info->usrmod0_password_hist_len == Saved->usrmod0_password_hist_len, "password_hist_len changed\n");
        NetApiBufferFree(Info);
    }

    /* Below the minimum: 2245 on a set and on an add; the add leaves no account (Win11 26100) */
    NetUserDel(NULL, TEST_USER);
    NetUserDel(NULL, TEST_USER2);
    Add.usri1_name = (PWSTR)TEST_USER;
    Add.usri1_password = (PWSTR)LONG_PW;
    Add.usri1_priv = USER_PRIV_USER;
    Add.usri1_flags = UF_SCRIPT;
    Status = NetUserAdd(NULL, 1, (LPBYTE)&Add, NULL);
    ok(Status == NERR_Success, "NetUserAdd with a long password returned %lu\n", Status);
    if (Status == NERR_Success)
    {
        ok(SetPassword(TEST_USER, L"abc") == NERR_PasswordTooShort, "A 3-character password was not refused with 2245\n");
        ok(SetPassword(TEST_USER, L"abcdefg") == NERR_PasswordTooShort, "A 7-character password was not refused with 2245\n");
        ok(SetPassword(TEST_USER, L"abcdefgh") == NERR_Success, "An 8-character password was refused\n");
        NetUserDel(NULL, TEST_USER);
    }

    Add.usri1_name = (PWSTR)TEST_USER2;
    Add.usri1_password = (PWSTR)L"abc";
    Status = NetUserAdd(NULL, 1, (LPBYTE)&Add, NULL);
    ok(Status == NERR_PasswordTooShort, "NetUserAdd with a 3-character password returned %lu\n", Status);
    Status = NetUserGetInfo(NULL, TEST_USER2, 1, (LPBYTE *)&UserInfo);
    ok(Status == NERR_UserNotFound, "The refused add left the account behind (%lu)\n", Status);
    if (UserInfo != NULL)
        NetApiBufferFree(UserInfo);
    if (Status == NERR_Success)
        NetUserDel(NULL, TEST_USER2);

    /* Level 1005 changes only the history length */
    History.usrmod1005_password_hist_len = 5;
    Status = NetUserModalsSet(NULL, 1005, (LPBYTE)&History, NULL);
    ok(Status == NERR_Success, "NetUserModalsSet(1005) returned %lu\n", Status);
    Info = GetModals0();
    if (Info != NULL)
    {
        ok(Info->usrmod0_password_hist_len == 5, "password_hist_len is %lu\n", Info->usrmod0_password_hist_len);
        ok(Info->usrmod0_min_passwd_len == 8, "min_passwd_len changed to %lu\n", Info->usrmod0_min_passwd_len);
        NetApiBufferFree(Info);
    }

    /* Level 0 restores everything, and a round trip keeps each value */
    Status = NetUserModalsSet(NULL, 0, (LPBYTE)Saved, NULL);
    ok(Status == NERR_Success, "NetUserModalsSet(0) returned %lu\n", Status);
    Info = GetModals0();
    if (Info != NULL)
    {
        ok(Info->usrmod0_min_passwd_len == Saved->usrmod0_min_passwd_len, "min_passwd_len %lu, expected %lu\n",
           Info->usrmod0_min_passwd_len, Saved->usrmod0_min_passwd_len);
        ok(Info->usrmod0_max_passwd_age == Saved->usrmod0_max_passwd_age, "max_passwd_age %lu, expected %lu\n",
           Info->usrmod0_max_passwd_age, Saved->usrmod0_max_passwd_age);
        ok(Info->usrmod0_min_passwd_age == Saved->usrmod0_min_passwd_age, "min_passwd_age %lu, expected %lu\n",
           Info->usrmod0_min_passwd_age, Saved->usrmod0_min_passwd_age);
        ok(Info->usrmod0_force_logoff == Saved->usrmod0_force_logoff, "force_logoff %lu, expected %lu\n",
           Info->usrmod0_force_logoff, Saved->usrmod0_force_logoff);
        ok(Info->usrmod0_password_hist_len == Saved->usrmod0_password_hist_len, "password_hist_len %lu, expected %lu\n",
           Info->usrmod0_password_hist_len, Saved->usrmod0_password_hist_len);
        NetApiBufferFree(Info);
    }

    /* Level 1004 (logoff) round trip, then level 0 puts the saved value back */
    {
        USER_MODALS_INFO_1004 Logoff = { 3600 };

        Status = NetUserModalsSet(NULL, 1004, (LPBYTE)&Logoff, NULL);
        ok(Status == NERR_Success, "NetUserModalsSet(1004) returned %lu\n", Status);
        Info = GetModals0();
        if (Info != NULL)
        {
            ok(Info->usrmod0_force_logoff == 3600, "force_logoff is %lu\n", Info->usrmod0_force_logoff);
            NetApiBufferFree(Info);
        }
        Status = NetUserModalsSet(NULL, 0, (LPBYTE)Saved, NULL);
        ok(Status == NERR_Success, "Restoring level 0 returned %lu\n", Status);
    }

    /* Level 3 (lockout) round trip, restored afterwards */
    {
        PUSER_MODALS_INFO_3 Lockout = NULL, Check = NULL;
        USER_MODALS_INFO_3 New;

        Status = NetUserModalsGet(NULL, 3, (LPBYTE *)&Lockout);
        ok(Status == NERR_Success, "NetUserModalsGet(3) returned %lu\n", Status);
        if (Lockout != NULL)
        {
            New.usrmod3_lockout_duration = 1800;
            New.usrmod3_lockout_observation_window = 900;
            New.usrmod3_lockout_threshold = 7;
            Status = NetUserModalsSet(NULL, 3, (LPBYTE)&New, NULL);
            ok(Status == NERR_Success, "NetUserModalsSet(3) returned %lu\n", Status);
            if (NetUserModalsGet(NULL, 3, (LPBYTE *)&Check) == NERR_Success)
            {
                ok(Check->usrmod3_lockout_duration == 1800, "lockout_duration is %lu\n", Check->usrmod3_lockout_duration);
                ok(Check->usrmod3_lockout_observation_window == 900, "observation_window is %lu\n", Check->usrmod3_lockout_observation_window);
                ok(Check->usrmod3_lockout_threshold == 7, "lockout_threshold is %lu\n", Check->usrmod3_lockout_threshold);
                NetApiBufferFree(Check);
            }
            Status = NetUserModalsSet(NULL, 3, (LPBYTE)Lockout, NULL);
            ok(Status == NERR_Success, "Restoring level 3 returned %lu\n", Status);
            NetApiBufferFree(Lockout);
        }
    }

    ok(NetUserModalsSet(NULL, 42, (LPBYTE)Saved, NULL) == ERROR_INVALID_LEVEL, "Level 42 was accepted\n");

    NetApiBufferFree(Saved);
}

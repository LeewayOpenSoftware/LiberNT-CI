/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     UAC token information classes of the current process token
 *              (UAC-PLAN.md section 5; P0 baseline, UAC off)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Queries TokenElevation, TokenElevationType, TokenLinkedToken, TokenIntegrityLevel,
 * TokenVirtualizationAllowed/Enabled and TokenUIAccess, logs every answer and checks
 * the values Windows returns with UAC off. The same test is re-run after P1 (UAC on)
 * with the expectations switched by the EnableLUA policy value.
 */

#include "precomp.h"
#include <winbase.h>
#include <sddl.h>

#define LWUI_RID_UNTRUSTED  0x0000
#define LWUI_RID_LOW        0x1000
#define LWUI_RID_MEDIUM     0x2000
#define LWUI_RID_HIGH       0x3000
#define LWUI_RID_SYSTEM     0x4000

static HANDLE g_hToken;

static BOOL GetTokenDword(TOKEN_INFORMATION_CLASS cls, DWORD *pValue, DWORD *pError)
{
    DWORD cb = 0;
    BOOL ret;

    *pValue = 0xDEADBEEF;
    SetLastError(0xDEADBEEF);
    ret = GetTokenInformation(g_hToken, cls, pValue, sizeof(*pValue), &cb);
    *pError = ret ? ERROR_SUCCESS : GetLastError();
    if (ret)
        ok(cb == sizeof(DWORD), "class %d: returned length %lu\n", (int)cls, cb);
    return ret;
}

static BOOL IsAdministratorToken(void)
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID pAdmins = NULL;
    BOOL isMember = FALSE;

    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &pAdmins))
    {
        CheckTokenMembership(NULL, pAdmins, &isMember);
        FreeSid(pAdmins);
    }
    return isMember;
}

static BOOL IsLuaEnabled(void)
{
    HKEY hKey;
    DWORD value = 0, cb = sizeof(value), type;
    BOOL enabled = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                      0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, L"EnableLUA", NULL, &type, (LPBYTE)&value, &cb) == ERROR_SUCCESS &&
            type == REG_DWORD)
            enabled = (value != 0);
        RegCloseKey(hKey);
    }
    return enabled;
}

static void TestElevation(BOOL isAdmin, BOOL lua)
{
    TOKEN_ELEVATION elev = { 0xDEADBEEF };
    DWORD cb = 0, error;
    BOOL ret;

    SetLastError(0xDEADBEEF);
    ret = GetTokenInformation(g_hToken, TokenElevation, &elev, sizeof(elev), &cb);
    error = GetLastError();
    ok(ret, "TokenElevation failed with %lu\n", error);
    if (!ret)
        return;
    ok(cb == sizeof(elev), "TokenElevation length %lu\n", cb);
    trace("TokenElevation.TokenIsElevated = %lu (admin group member: %d)\n", elev.TokenIsElevated, isAdmin);
    ok(elev.TokenIsElevated <= 1, "TokenIsElevated = %lu\n", elev.TokenIsElevated);
    if (!lua)
        ok(elev.TokenIsElevated == (DWORD)isAdmin, "UAC off: TokenIsElevated = %lu, admin = %d\n",
           elev.TokenIsElevated, isAdmin);
}

static TOKEN_ELEVATION_TYPE TestElevationType(BOOL lua)
{
    DWORD type, error;
    BOOL ret = GetTokenDword(TokenElevationType, &type, &error);

    ok(ret, "TokenElevationType failed with %lu\n", error);
    if (!ret)
        return TokenElevationTypeDefault;
    trace("TokenElevationType = %lu (1 default, 2 full, 3 limited)\n", type);
    ok(type >= TokenElevationTypeDefault && type <= TokenElevationTypeLimited, "type = %lu\n", type);
    if (!lua)
        ok(type == TokenElevationTypeDefault, "UAC off: TokenElevationType = %lu, expected 1\n", type);
    return (TOKEN_ELEVATION_TYPE)type;
}

static void TestLinkedToken(TOKEN_ELEVATION_TYPE elevType, BOOL lua)
{
    TOKEN_LINKED_TOKEN linked = { NULL };
    DWORD cb = 0, error;
    BOOL ret;

    SetLastError(0xDEADBEEF);
    ret = GetTokenInformation(g_hToken, TokenLinkedToken, &linked, sizeof(linked), &cb);
    error = ret ? ERROR_SUCCESS : GetLastError();
    trace("TokenLinkedToken: ret = %d, error = %lu, handle = %p\n", ret, error, ret ? linked.LinkedToken : NULL);
    if (ret)
    {
        ok(linked.LinkedToken != NULL, "linked token handle is NULL\n");
        if (linked.LinkedToken)
            CloseHandle(linked.LinkedToken);
    }
    if (!lua || elevType == TokenElevationTypeDefault)
    {
        /* No split token: Windows fails the query (ERROR_NO_SUCH_LOGON_SESSION on 7+) */
        ok(!ret, "unsplit token returned a linked token\n");
        ok(error == ERROR_NO_SUCH_LOGON_SESSION || error == ERROR_NOT_FOUND ||
           error == ERROR_INVALID_PARAMETER || error == ERROR_ACCESS_DENIED,
           "unsplit token: unexpected error %lu\n", error);
    }
    else
    {
        ok(ret, "split token (type %d): TokenLinkedToken failed with %lu\n", (int)elevType, error);
    }
}

/*
 * P0.1 ORACLE: GetTokenInformation(TokenIntegrityLevel) must SUCCEED and return an
 * S-1-16-<rid> label. Today (tokencls.c) it fails with ERROR_NOT_FOUND because no label
 * is stamped at logon; Windows returns High for an administrator (UAC off), Medium for a
 * standard user and System for SYSTEM. This is the check the Office Click-to-Run
 * installer ("2005, privileges required") is believed to trip on.
 */
static void TestIntegrityLevel(BOOL isAdmin, BOOL lua, TOKEN_ELEVATION_TYPE elevType)
{
    BYTE buf[256];
    PTOKEN_MANDATORY_LABEL pLabel = (PTOKEN_MANDATORY_LABEL)buf;
    DWORD cb = 0, error, rid;
    PSID_IDENTIFIER_AUTHORITY pAuth;
    BOOL ret;

    SetLastError(0xDEADBEEF);
    ret = GetTokenInformation(g_hToken, TokenIntegrityLevel, buf, sizeof(buf), &cb);
    error = ret ? ERROR_SUCCESS : GetLastError();
    ok(ret, "P0.1 oracle: TokenIntegrityLevel failed with %lu (expected a S-1-16 label)\n", error);
    if (!ret)
        return;

    ok(pLabel->Label.Sid != NULL && IsValidSid(pLabel->Label.Sid), "label SID is invalid\n");
    if (!pLabel->Label.Sid || !IsValidSid(pLabel->Label.Sid))
        return;
    pAuth = GetSidIdentifierAuthority(pLabel->Label.Sid);
    ok(pAuth->Value[5] == 16 && !pAuth->Value[0] && !pAuth->Value[1] && !pAuth->Value[2] &&
       !pAuth->Value[3] && !pAuth->Value[4], "label authority is not SECURITY_MANDATORY_LABEL_AUTHORITY\n");
    ok(*GetSidSubAuthorityCount(pLabel->Label.Sid) == 1, "label sub-authority count %u\n",
       *GetSidSubAuthorityCount(pLabel->Label.Sid));
    rid = *GetSidSubAuthority(pLabel->Label.Sid, 0);
    {
        LPWSTR psz = NULL;
        if (ConvertSidToStringSidW(pLabel->Label.Sid, &psz))
        {
            trace("TokenIntegrityLevel = %ls (attributes 0x%lx)\n", psz, pLabel->Label.Attributes);
            LocalFree(psz);
        }
    }
    ok(rid == LWUI_RID_UNTRUSTED || rid == LWUI_RID_LOW || rid == LWUI_RID_MEDIUM ||
       rid == LWUI_RID_HIGH || rid == LWUI_RID_SYSTEM, "unexpected integrity RID 0x%lx\n", rid);
    ok((pLabel->Label.Attributes & SE_GROUP_INTEGRITY) != 0, "label attributes 0x%lx\n", pLabel->Label.Attributes);

    if (!lua && isAdmin)
        ok(rid >= LWUI_RID_HIGH, "UAC off, administrator: integrity RID 0x%lx, expected >= High (0x3000)\n", rid);
    else if (lua && elevType == TokenElevationTypeLimited)
        ok(rid == LWUI_RID_MEDIUM, "limited token: integrity RID 0x%lx, expected Medium\n", rid);
    else if (lua && elevType == TokenElevationTypeFull)
        ok(rid >= LWUI_RID_HIGH, "full token: integrity RID 0x%lx, expected >= High\n", rid);
    else
        ok(rid >= LWUI_RID_MEDIUM, "integrity RID 0x%lx, expected >= Medium\n", rid);
}

static void TestVirtualization(BOOL lua)
{
    DWORD allowed, enabled, errAllowed, errEnabled;
    BOOL retAllowed = GetTokenDword(TokenVirtualizationAllowed, &allowed, &errAllowed);
    BOOL retEnabled = GetTokenDword(TokenVirtualizationEnabled, &enabled, &errEnabled);

    ok(retAllowed, "TokenVirtualizationAllowed failed with %lu\n", errAllowed);
    ok(retEnabled, "TokenVirtualizationEnabled failed with %lu\n", errEnabled);
    if (!retAllowed || !retEnabled)
        return;
    trace("TokenVirtualizationAllowed = %lu, TokenVirtualizationEnabled = %lu\n", allowed, enabled);
    ok(allowed <= 1 && enabled <= 1, "allowed %lu, enabled %lu\n", allowed, enabled);
    ok(!enabled || allowed, "virtualization enabled but not allowed\n");
    if (!lua)
        ok(!enabled, "UAC off: virtualization is enabled\n");
}

static void TestUIAccess(void)
{
    DWORD value, error;
    BOOL ret = GetTokenDword(TokenUIAccess, &value, &error);

    ok(ret, "TokenUIAccess failed with %lu\n", error);
    if (!ret)
        return;
    trace("TokenUIAccess = %lu\n", value);
    ok(value == 0, "ordinary process has TokenUIAccess = %lu\n", value);
}

START_TEST(TokenUac)
{
    BOOL isAdmin, lua;
    TOKEN_ELEVATION_TYPE elevType;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &g_hToken))
    {
        skip("OpenProcessToken failed with %lu\n", GetLastError());
        return;
    }

    isAdmin = IsAdministratorToken();
    lua = IsLuaEnabled();
    trace("administrator group enabled: %d, EnableLUA: %d\n", isAdmin, lua);

    TestElevation(isAdmin, lua);
    elevType = TestElevationType(lua);
    TestLinkedToken(elevType, lua);
    TestIntegrityLevel(isAdmin, lua, elevType);
    TestVirtualization(lua);
    TestUIAccess();

    CloseHandle(g_hToken);
}

/*
 * PROJECT:     LiberNT COM security
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RpcSs launch and activation checks, owners of class registrations (DCOM step 5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Launch and activation: the class's AppID LaunchPermission, else the machine's
 * DefaultLaunchPermission, else Local System, Administrators and INTERACTIVE; the
 * machine-wide limit MachineLaunchRestriction applies in every case. Values come from
 * HKLM\Software\Classes and HKLM\Software\Microsoft\Ole only.
 *
 * Owners: a class object registered by a process is handed only to clients of the same
 * user and logon session whose integrity level is not above the registering process's,
 * unless the registering process runs as Local System, Local Service or Network Service,
 * belongs to a service, or the class's AppID names a RunAs identity.
 *
 * Off unless HKLM\Software\LiberNT\COM Enforcement is 1 (audit) or 2 (enforce). RpcSs
 * reads the switch on every request, so a change takes effect without a restart.
 */

#include "precomp.h"

#include <stdlib.h>
#include <winreg.h>
#include <objbase.h>
#include <comsec.h>
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ole);


HKEY scm_open_appid_key(const GUID *clsid, REGSAM view);

/* The class's AppID key, native registration first, then the 32-bit one */
static HKEY open_appid(const GUID *clsid)
{
    HKEY key;

    if (!(key = scm_open_appid_key(clsid, KEY_WOW64_64KEY)))
        key = scm_open_appid_key(clsid, KEY_WOW64_32KEY);
    return key;
}

static HANDLE client_token(handle_t h)
{
    HANDLE token = NULL;

    if (RpcImpersonateClient(h) == RPC_S_OK)
    {
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
            token = NULL;
        RpcRevertToSelf();
    }
    return token;
}

static BOOL appid_has_value(const GUID *clsid, const WCHAR *value)
{
    HKEY key;
    BOOL ret;

    if (!(key = open_appid(clsid)))
        return FALSE;
    ret = !RegQueryValueExW(key, value, NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    return ret;
}

static PSECURITY_DESCRIPTOR launch_sd(const GUID *clsid)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    HKEY key;

    if ((key = open_appid(clsid)))
    {
        /* A damaged value refuses (comsec_read_sd_key); only an absent one falls back */
        sd = comsec_read_sd_key(key, L"LaunchPermission");
        RegCloseKey(key);
    }
    if (!sd)
        sd = comsec_read_sd(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Ole", L"DefaultLaunchPermission");
    if (!sd)
        sd = comsec_default_launch_sd();
    return sd;
}

/* rights: COM_RIGHTS_EXECUTE_LOCAL to start a server, COM_RIGHTS_ACTIVATE_LOCAL to reach a running one */
HRESULT scm_check_launch(handle_t h, const GUID *clsid, DWORD rights, const WCHAR *what)
{
    PSECURITY_DESCRIPTOR sd, machine;
    HANDLE token;
    DWORD mode;
    ULONG pid = 0;
    BOOL allowed;

    if ((mode = comsec_read_mode()) == COMSEC_OFF)
        return S_OK;

    token = client_token(h);
    sd = launch_sd(clsid);
    machine = comsec_read_sd(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Ole", L"MachineLaunchRestriction");
    allowed = sd && comsec_check(token, machine, rights) && comsec_check(token, sd, rights);
    if (!allowed)
    {
        I_RpcBindingInqLocalClientPID(h, &pid);
        WARN("%s of %s by process %lu %s\n", debugstr_w(what), debugstr_guid(clsid), pid,
             mode == COMSEC_ENFORCE ? "refused" : "would be refused");
        comsec_report_denial(mode, what, clsid, token, pid);
    }
    LocalFree(machine);
    LocalFree(sd);
    if (token)
        CloseHandle(token);
    return allowed || mode != COMSEC_ENFORCE ? S_OK : E_ACCESSDENIED;
}

struct scm_owner
{
    PSID user;
    LUID logon;
    DWORD integrity;
    BOOL serves_all;
};

static BOOL token_is_service(HANDLE token, PSID user)
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID service = NULL;
    BOOL member = FALSE;

    if (IsWellKnownSid(user, WinLocalSystemSid) || IsWellKnownSid(user, WinLocalServiceSid) ||
        IsWellKnownSid(user, WinNetworkServiceSid))
        return TRUE;
    if (AllocateAndInitializeSid(&nt, 1, SECURITY_SERVICE_RID, 0, 0, 0, 0, 0, 0, 0, &service))
    {
        if (!CheckTokenMembership(token, service, &member))
            member = FALSE;
        FreeSid(service);
    }
    return member;
}

/* Called while the registering client is identified; NULL when its token cannot be read */
struct scm_owner *scm_owner_capture(handle_t h, const GUID *clsid)
{
    struct scm_owner *owner;
    HANDLE token;

    if (!(token = client_token(h)))
        return NULL;
    if ((owner = calloc(1, sizeof(*owner))) && (owner->user = comsec_token_user(token)) &&
        comsec_token_logon(token, &owner->logon))
    {
        owner->integrity = comsec_token_integrity(token);
        owner->serves_all = token_is_service(token, owner->user) || appid_has_value(clsid, L"RunAs");
    }
    else if (owner)
    {
        LocalFree(owner->user);
        free(owner);
        owner = NULL;
    }
    CloseHandle(token);
    return owner;
}

void scm_owner_free(struct scm_owner *owner)
{
    if (!owner)
        return;
    LocalFree(owner->user);
    free(owner);
}

struct scm_client
{
    DWORD mode;
    HANDLE token;
    PSID user;
    LUID logon;
    DWORD integrity;
    BOOL known;
};

void scm_client_begin(handle_t h, struct scm_client **client)
{
    struct scm_client *c;

    *client = NULL;
    if (!(c = calloc(1, sizeof(*c))))
        return;
    if ((c->mode = comsec_read_mode()) != COMSEC_OFF && (c->token = client_token(h)))
    {
        c->user = comsec_token_user(c->token);
        c->known = c->user && comsec_token_logon(c->token, &c->logon);
        c->integrity = comsec_token_integrity(c->token);
    }
    *client = c;
}

void scm_client_end(struct scm_client *client)
{
    if (!client)
        return;
    if (client->token)
        CloseHandle(client->token);
    LocalFree(client->user);
    free(client);
}

/* TRUE when the registration may be handed to this client */
BOOL scm_owner_serves(handle_t h, const struct scm_owner *owner, const struct scm_client *client, const GUID *clsid)
{
    ULONG pid = 0;
    BOOL match;

    if (!client)
        return comsec_read_mode() != COMSEC_ENFORCE;
    if (client->mode == COMSEC_OFF)
        return TRUE;
    match = owner && (owner->serves_all ||
            (client->known && EqualSid(owner->user, client->user) &&
             owner->logon.LowPart == client->logon.LowPart && owner->logon.HighPart == client->logon.HighPart &&
             owner->integrity >= client->integrity));
    if (!match)
    {
        I_RpcBindingInqLocalClientPID(h, &pid);
        WARN("registration of %s not handed to process %lu (%s)\n", debugstr_guid(clsid), pid,
             client->mode == COMSEC_ENFORCE ? "skipped" : "audit");
        comsec_report_denial(client->mode, L"class registration of another identity", clsid, client->token, pid);
    }
    return match || client->mode != COMSEC_ENFORCE;
}

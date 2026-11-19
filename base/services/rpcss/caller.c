/*
 * PROJECT:     LiberNT
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Who is calling RpcSs: the caller's integrity level and process
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "rpc.h"
#include "rpcdcep.h"

#include "caller.h"

static DWORD rpcss_token_integrity(HANDLE token)
{
    BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    DWORD size;
    PSID sid;

    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size))
        return 0;
    sid = ((TOKEN_MANDATORY_LABEL *)buffer)->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}

/* The integrity level RID of the caller, or 0 when it cannot be read */
DWORD rpcss_caller_integrity(handle_t h)
{
    DWORD rid = 0;
    HANDLE token, process;
    ULONG pid;

    if (RpcImpersonateClient(h) == RPC_S_OK)
    {
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
        {
            rid = rpcss_token_integrity(token);
            CloseHandle(token);
        }
        RpcRevertToSelf();
    }

    /* Without a client token, the process at the other end of the pipe (named by the kernel) */
    if (!rid && (pid = rpcss_caller_pid(h)) &&
        (process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))
    {
        if (OpenProcessToken(process, TOKEN_QUERY, &token))
        {
            rid = rpcss_token_integrity(token);
            CloseHandle(token);
        }
        CloseHandle(process);
    }
    return rid;
}

/* The calling process, or 0 when the transport cannot tell */
ULONG rpcss_caller_pid(handle_t h)
{
    ULONG pid;

    return I_RpcBindingInqLocalClientPID(h, &pid) == RPC_S_OK ? pid : 0;
}

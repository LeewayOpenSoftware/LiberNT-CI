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

/* The integrity level RID of the calling token, or 0 when it cannot be read */
DWORD rpcss_caller_integrity(handle_t h)
{
    BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    DWORD size, rid = 0;
    HANDLE token;

    if (RpcImpersonateClient(h) != RPC_S_OK)
        return 0;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
    {
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size))
        {
            PSID sid = ((TOKEN_MANDATORY_LABEL *)buffer)->Label.Sid;
            rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        CloseHandle(token);
    }
    RpcRevertToSelf();
    return rid;
}

/* The calling process, or 0 when the transport cannot tell */
ULONG rpcss_caller_pid(handle_t h)
{
    ULONG pid;

    return I_RpcBindingInqLocalClientPID(h, &pid) == RPC_S_OK ? pid : 0;
}

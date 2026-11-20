/*
 * PROJECT:     LiberNT
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Who is calling RpcSs
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

DWORD rpcss_caller_integrity(handle_t h);
ULONG rpcss_caller_pid(handle_t h);

/* A registration made at system integrity (services) is visible to every caller; any other only to
   callers at the same level, as Windows separates low, medium and elevated processes. A level that
   could not be read (0) matches nothing */
static inline BOOL rpcss_integrity_visible(DWORD registrant, DWORD caller)
{
    if (registrant >= SECURITY_MANDATORY_SYSTEM_RID)
        return TRUE;
    return registrant && registrant == caller;
}

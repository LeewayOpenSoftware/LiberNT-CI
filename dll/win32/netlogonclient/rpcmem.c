/*
 * PROJECT:     LiberNT Netlogon client
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later.html)
 * PURPOSE:     RPC allocation callbacks for the Netlogon client
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <windef.h>
#include <winbase.h>
#include <rpc.h>

void __RPC_FAR * __RPC_USER midl_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
}

void __RPC_USER midl_user_free(void __RPC_FAR *ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}

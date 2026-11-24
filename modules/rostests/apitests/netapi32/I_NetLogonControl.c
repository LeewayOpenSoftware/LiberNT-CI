/*
 * PROJECT:     ReactOS netapi32.dll API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests that I_NetLogonControl / I_NetLogonControl2 match Win11's
 *              client-side behaviour: only I_NetLogonControl2's level range is
 *              checked locally, everything else is passed to the RPC layer. The
 *              wire-bound asserts hold with or without a domain controller.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <lmcons.h>
#include <lmaccess.h>
#include <lmapibuf.h>
#include <lmerr.h>

START_TEST(I_NetLogonControl)
{
    NET_API_STATUS status;
    PBYTE Buffer;

    /* --- I_NetLogonControl2 --- */

    /* NULL Buffer is not rejected client-side; the call reaches the RPC layer. */
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_QUERY, 1, NULL, NULL);
    ok(status != ERROR_INVALID_PARAMETER && status != ERROR_INVALID_LEVEL,
       "NULL Buffer: client-rejected %lu\n", status);

    /* QueryLevel 0 is out of range (valid 1..4): rejected client-side, Buffer cleared. */
    Buffer = (PBYTE)(ULONG_PTR)0xdeadbeef;
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_QUERY, 0, NULL, &Buffer);
    ok(status == ERROR_INVALID_LEVEL, "QueryLevel 0: got %lu\n", status);
    ok(Buffer == NULL, "QueryLevel 0: Buffer %p\n", Buffer);

    /* QueryLevel 5 is out of range. */
    Buffer = (PBYTE)(ULONG_PTR)0xdeadbeef;
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_QUERY, 5, NULL, &Buffer);
    ok(status == ERROR_INVALID_LEVEL, "QueryLevel 5: got %lu\n", status);
    ok(Buffer == NULL, "QueryLevel 5: Buffer %p\n", Buffer);

    /* REDISCOVER with NULL Data is not rejected client-side; it reaches the wire and
     * the caller's Buffer is left untouched unless the call actually succeeds. */
    Buffer = (PBYTE)(ULONG_PTR)0xdeadbeef;
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_REDISCOVER, 2, NULL, &Buffer);
    ok(status != ERROR_INVALID_PARAMETER && status != ERROR_INVALID_LEVEL,
       "REDISCOVER no data: client-rejected %lu\n", status);
    ok(Buffer == (PBYTE)(ULONG_PTR)0xdeadbeef || status == NERR_Success,
       "REDISCOVER no data: Buffer %p\n", Buffer);

    /* F1 regression: QUERY carries no data, but Data is a [ref] param, so the wrapper
     * must pass a non-NULL block, else RPC_X_NULL_REF_POINTER (1780). */
    Buffer = NULL;
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_QUERY, 1, NULL, &Buffer);
    ok(status != RPC_X_NULL_REF_POINTER, "QUERY no data: got %lu (must not be 1780)\n", status);
    if (status == NERR_Success && Buffer != NULL)
        NetApiBufferFree(Buffer);

    /* --- I_NetLogonControl (old API: no client-side validation at all) --- */

    /* NULL Buffer is not rejected client-side. */
    status = I_NetLogonControl(NULL, NETLOGON_CONTROL_QUERY, 1, NULL);
    ok(status != ERROR_INVALID_PARAMETER && status != ERROR_INVALID_LEVEL,
       "I_NetLogonControl NULL Buffer: client-rejected %lu\n", status);

    /* The old API does not range-check the level; it reaches the wire and the
     * caller's Buffer is left untouched unless the call succeeds. */
    Buffer = (PBYTE)(ULONG_PTR)0xdeadbeef;
    status = I_NetLogonControl(NULL, NETLOGON_CONTROL_QUERY, 99, &Buffer);
    ok(status != ERROR_INVALID_PARAMETER && status != ERROR_INVALID_LEVEL,
       "I_NetLogonControl level 99: client-rejected %lu\n", status);
    ok(Buffer == (PBYTE)(ULONG_PTR)0xdeadbeef || status == NERR_Success,
       "I_NetLogonControl level 99: Buffer %p\n", Buffer);
}

/*
 * PROJECT:     ReactOS netapi32.dll API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for I_NetLogonControl / I_NetLogonControl2 parameter
 *              validation (the paths that return before any RPC, so no DC).
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

    /* NULL output buffer is rejected. */
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_QUERY, 1, NULL, NULL);
    ok(status == ERROR_INVALID_PARAMETER, "NULL Buffer: got %lu\n", status);

    /* QueryLevel 0 is out of range (valid 1..4) and the buffer is cleared. */
    Buffer = (PBYTE)(ULONG_PTR)0xdeadbeef;
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_QUERY, 0, NULL, &Buffer);
    ok(status == ERROR_INVALID_LEVEL, "QueryLevel 0: got %lu\n", status);
    ok(Buffer == NULL, "QueryLevel 0: Buffer %p\n", Buffer);

    /* QueryLevel 5 is out of range. */
    Buffer = (PBYTE)(ULONG_PTR)0xdeadbeef;
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_QUERY, 5, NULL, &Buffer);
    ok(status == ERROR_INVALID_LEVEL, "QueryLevel 5: got %lu\n", status);
    ok(Buffer == NULL, "QueryLevel 5: Buffer %p\n", Buffer);

    /* A data-bearing FunctionCode (REDISCOVER) with NULL data is rejected
     * before the wire. */
    Buffer = (PBYTE)(ULONG_PTR)0xdeadbeef;
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_REDISCOVER, 2, NULL, &Buffer);
    ok(status == ERROR_INVALID_PARAMETER, "REDISCOVER no data: got %lu\n", status);
    ok(Buffer == NULL, "REDISCOVER no data: Buffer %p\n", Buffer);

    /* F1 regression: QUERY carries no data, but NetrLogonControl2's Data is a
     * [ref] pointer, so the wrapper must pass a non-NULL block. A NULL would come
     * back as RPC_X_NULL_REF_POINTER (1780); without a server we expect an RPC
     * "server unavailable"-class error (or a real answer), never 1780. */
    Buffer = NULL;
    status = I_NetLogonControl2(NULL, NETLOGON_CONTROL_QUERY, 1, NULL, &Buffer);
    ok(status != RPC_X_NULL_REF_POINTER, "QUERY no data: got %lu (must not be 1780)\n", status);
    if (status == NERR_Success && Buffer != NULL)
        NetApiBufferFree(Buffer);

    /* --- I_NetLogonControl --- */

    /* NULL output buffer is rejected. */
    status = I_NetLogonControl(NULL, NETLOGON_CONTROL_QUERY, 1, NULL);
    ok(status == ERROR_INVALID_PARAMETER, "I_NetLogonControl NULL Buffer: got %lu\n", status);

    /* Out-of-range QueryLevel is rejected and the buffer cleared. */
    Buffer = (PBYTE)(ULONG_PTR)0xdeadbeef;
    status = I_NetLogonControl(NULL, NETLOGON_CONTROL_QUERY, 99, &Buffer);
    ok(status == ERROR_INVALID_LEVEL, "I_NetLogonControl level 99: got %lu\n", status);
    ok(Buffer == NULL, "I_NetLogonControl level 99: Buffer %p\n", Buffer);
}

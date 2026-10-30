/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for CM_Reenumerate_DevNode and the retry-installation flag
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <winreg.h>
#include <cfgmgr32.h>

START_TEST(ReenumerateDevNode)
{
    DEVINST RootDevInst;
    CONFIGRET cr;

    cr = CM_Reenumerate_DevNode(0, 0);
    ok(cr == CR_INVALID_DEVNODE,
       "CM_Reenumerate_DevNode(0, 0) returned %lu, expected CR_INVALID_DEVNODE\n", cr);

    cr = CM_Reenumerate_DevNode(0, CM_REENUMERATE_RETRY_INSTALLATION);
    ok(cr == CR_INVALID_DEVNODE,
       "CM_Reenumerate_DevNode(0, RETRY_INSTALLATION) returned %lu, expected CR_INVALID_DEVNODE\n", cr);

    cr = CM_Locate_DevNodeW(&RootDevInst, (DEVINSTID_W)L"HTREE\\ROOT\\0", CM_LOCATE_DEVNODE_NORMAL);
    ok(cr == CR_SUCCESS, "CM_Locate_DevNodeW(HTREE\\ROOT\\0) returned %lu\n", cr);
    if (cr != CR_SUCCESS)
        return;

    cr = CM_Reenumerate_DevNode(RootDevInst, CM_REENUMERATE_BITS + 1);
    ok(cr == CR_INVALID_FLAG,
       "CM_Reenumerate_DevNode with an invalid flag returned %lu, expected CR_INVALID_FLAG\n", cr);

    cr = CM_Reenumerate_DevNode(RootDevInst, CM_REENUMERATE_RETRY_INSTALLATION);
    ok(cr == CR_SUCCESS,
       "CM_Reenumerate_DevNode with RETRY_INSTALLATION returned %lu\n", cr);

    cr = CM_Reenumerate_DevNode(RootDevInst,
                                CM_REENUMERATE_RETRY_INSTALLATION | CM_REENUMERATE_SYNCHRONOUS);
    ok(cr == CR_SUCCESS,
       "CM_Reenumerate_DevNode with RETRY_INSTALLATION | SYNCHRONOUS returned %lu\n", cr);
}

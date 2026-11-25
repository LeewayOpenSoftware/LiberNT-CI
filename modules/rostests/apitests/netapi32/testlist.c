/*
 * PROJECT:     ReactOS netapi32.dll API Tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Test list
 * COPYRIGHT:   Copyright 2017 Colin Finck (colin@reactos.org)
 */

#define STANDALONE
#include <apitest.h>

extern void func_DsRoleGetPrimaryDomainInformation(void);
extern void func_NetUserDelete(void);
extern void func_NetLocalGroupAddMembers(void);
extern void func_NetUserEnum(void);
extern void func_NetUserGetLocalGroups(void);
extern void func_NetUserModals(void);
extern void func_NetUserMustChange(void);

const struct test winetest_testlist[] =
{
    { "DsRoleGetPrimaryDomainInformation", func_DsRoleGetPrimaryDomainInformation },
    { "NetUserDelete", func_NetUserDelete },
    { "NetLocalGroupAddMembers", func_NetLocalGroupAddMembers },
    { "NetUserEnum", func_NetUserEnum },
    { "NetUserGetLocalGroups", func_NetUserGetLocalGroups },
    { "NetUserModals", func_NetUserModals },
    { "NetUserMustChange", func_NetUserMustChange },

    { 0, 0 }
};

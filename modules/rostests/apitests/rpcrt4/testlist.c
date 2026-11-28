/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test list for rpcrt4.dll
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#define STANDALONE
#include <apitest.h>

extern void func_EmbeddedPointers(void);
extern void func_LocalAuth(void);
extern void func_ServerIdentity(void);
extern void func_ServerRestart(void);
extern void func_TcpListen(void);

const struct test winetest_testlist[] =
{
    { "EmbeddedPointers", func_EmbeddedPointers },
    { "LocalAuth", func_LocalAuth },
    { "ServerIdentity", func_ServerIdentity },
    { "ServerRestart", func_ServerRestart },
    { "TcpListen", func_TcpListen },
    { 0, 0 }
};

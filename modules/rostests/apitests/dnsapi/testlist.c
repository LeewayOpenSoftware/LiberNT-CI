#define STANDALONE
#include <apitest.h>

extern void func_DnsQuery(void);
extern void func_HostsFile(void);

const struct test winetest_testlist[] =
{
    { "DnsQuery", func_DnsQuery },
    { "HostsFile", func_HostsFile },
    { 0, 0 }
};

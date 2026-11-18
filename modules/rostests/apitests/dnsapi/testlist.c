#define STANDALONE
#include <apitest.h>

extern void func_DnsQuery(void);
extern void func_DnsQueryEx(void);
extern void func_ExtractRecords(void);
extern void func_HostsFile(void);
extern void func_HostsCache(void);
extern void func_NegativeCache(void);

const struct test winetest_testlist[] =
{
    { "DnsQuery", func_DnsQuery },
    { "DnsQueryEx", func_DnsQueryEx },
    { "ExtractRecords", func_ExtractRecords },
    { "HostsFile", func_HostsFile },
    { "HostsCache", func_HostsCache },
    { "NegativeCache", func_NegativeCache },
    { 0, 0 }
};

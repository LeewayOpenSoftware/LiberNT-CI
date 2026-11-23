#define STANDALONE
#include <apitest.h>

extern void func_Abi(void);
extern void func_CredUIPack(void);
extern void func_Serialization(void);

const struct test winetest_testlist[] =
{
    { "Abi", func_Abi },
    { "CredUIPack", func_CredUIPack },
    { "Serialization", func_Serialization },
    { 0, 0 }
};

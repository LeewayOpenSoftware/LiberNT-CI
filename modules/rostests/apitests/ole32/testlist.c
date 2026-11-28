
#define STANDALONE
#include <wine/test.h>

extern void func_callcontrol(void);
extern void func_globaloptions(void);
extern void func_initializespy(void);
extern void func_localserver(void);
extern void func_neutral(void);
extern void func_security(void);
extern void func_storagestat(void);
extern void func_surrogate(void);
extern void func_uninitquit(void);

const struct test winetest_testlist[] =
{
    { "callcontrol", func_callcontrol },
    { "globaloptions", func_globaloptions },
    { "initializespy", func_initializespy },
    { "localserver", func_localserver },
    { "neutral", func_neutral },
    { "security", func_security },
    { "storagestat", func_storagestat },
    { "surrogate", func_surrogate },
    { "uninitquit", func_uninitquit },

    { 0, 0 }
};

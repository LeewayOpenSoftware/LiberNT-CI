/* Win11 standalone build: map ReactOS PSEH2 macros onto native SEH. */
#pragma once
#define _SEH2_TRY __try
#define _SEH2_FINALLY __finally
#define _SEH2_EXCEPT(x) __except(x)
#define _SEH2_END

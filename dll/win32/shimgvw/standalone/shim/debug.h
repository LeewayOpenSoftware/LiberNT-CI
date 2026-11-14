/* Win11 standalone build: ReactOS debug macros reduced to no-ops. */
#pragma once
#define DPRINT(...) ((void)0)
#define DPRINT1(...) ((void)0)
#define ASSERT(x) ((void)0)
#define UNIMPLEMENTED ((void)0)

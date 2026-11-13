/* Win11 standalone build: ReactOS debug macros reduced to no-ops/ASSERT. */
#pragma once
#include <assert.h>
#define DPRINT(...) ((void)0)
#define DPRINT1(...) ((void)0)
#define ASSERT(x) ((void)0)
#define UNIMPLEMENTED ((void)0)

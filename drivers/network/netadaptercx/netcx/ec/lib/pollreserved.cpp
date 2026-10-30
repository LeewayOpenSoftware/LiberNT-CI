// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precompiled.hpp"
#include "pollreserved.hpp"
#include "executioncontextpoll.h"

NONPAGEDX
_Use_decl_annotations_
PollReserved &
ContextFromPoll(
    EXECUTION_CONTEXT_POLL * Poll
)
{
    return reinterpret_cast<PollReserved&>(Poll->Reserved[0]);
}

// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "dispatcher.hpp"

NTSTATUS
InitializeDispatcherSubsystem(
    void
);

void
CleanupDispatcherSubsystem(
    void
);

Dispatcher &
GetProcessorDispatcher(
    _In_ PROCESSOR_NUMBER & Processor
);

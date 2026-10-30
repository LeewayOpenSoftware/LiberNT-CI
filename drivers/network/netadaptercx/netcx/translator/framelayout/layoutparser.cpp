// Copyright (C) Microsoft Corporation. All rights reserved.


#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#include <ntassert.h>

#include "layoutparser.hpp"

#include "layer2layout.hpp"
#include "layer3layout.hpp"
#include "layer4layout.hpp"

FUNC_PARSE_LAYER_LAYOUT* const ParseLayerDispatch[3] = {
    Layer2::ParseLayout,
    Layer3::ParseLayout,
    Layer4::ParseLayout
};

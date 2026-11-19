/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shared definitions of the neutral threading model test and its server
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/* {6D3A9B52-1C7E-4F08-A2D4-93B5E0C61F27} */
static const CLSID CLSID_NeutralTest = { 0x6d3a9b52, 0x1c7e, 0x4f08, { 0xa2, 0xd4, 0x93, 0xb5, 0xe0, 0xc6, 0x1f, 0x27 } };

#define NEUTRAL_CLASS_KEY L"Software\\Classes\\CLSID\\{6D3A9B52-1C7E-4F08-A2D4-93B5E0C61F27}"
#define NEUTRAL_SERVER_NAME L"ole32_neutral.dll"

#define NEUTRAL_DISPID_THREAD               1
#define NEUTRAL_DISPID_TYPE                 2
#define NEUTRAL_DISPID_QUALIFIER            3
#define NEUTRAL_DISPID_CALLBACK             4
#define NEUTRAL_DISPID_DISCONNECT           5
#define NEUTRAL_DISPID_CREATED_TYPE         6
#define NEUTRAL_DISPID_CREATED_QUALIFIER    7
#define NEUTRAL_DISPID_INIT_STA             8
#define NEUTRAL_DISPID_INIT_MTA             9

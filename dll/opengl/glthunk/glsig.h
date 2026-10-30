/*
 * PROJECT:     LiberNT OpenGL thunk
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Argument signatures of OpenGL commands
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#define GLTHUNK_TABLE_ENTRIES 336

typedef struct _GLTHUNK_SIGNATURE
{
    const char *Name;
    const char *Signature;
} GLTHUNK_SIGNATURE;

extern const GLTHUNK_SIGNATURE GlThunkSignatures[];
extern const unsigned int GlThunkSignatureCount;
extern const char *const GlThunkTableSignatures[GLTHUNK_TABLE_ENTRIES];

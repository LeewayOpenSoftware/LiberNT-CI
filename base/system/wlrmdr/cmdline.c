/*
 * PROJECT:     Orosi Logon Reminder
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Parser for the -s/-f/-t/-m/-a switches
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <stdlib.h>
#include <wchar.h>
#include <errno.h>

#include "wlrmdr.h"

static BOOL ParseNumber(PCWSTR Text, LONG *Value)
{
    PWSTR End;
    LONG Number;

    if (!Text[0])
        return FALSE;
    errno = 0;
    Number = wcstol(Text, &End, 10);
    if (*End || errno == ERANGE)
        return FALSE;
    *Value = Number;
    return TRUE;
}

VOID WlrParseCmdLine(INT argc, PWSTR *argv, WLRMDR_ARGS *Args)
{
    INT i;
    LONG Number;

    Args->TimeoutMs = WLRMDR_TIMEOUT_INFINITE;
    Args->Flags = 0;
    Args->Action = 0;
    Args->HasAction = FALSE;
    Args->Title = NULL;
    Args->Message = NULL;

    for (i = 1; i < argc; i++)
    {
        PCWSTR Sw = argv[i];

        if (Sw[0] != L'-' || !Sw[1] || Sw[2] || i + 1 >= argc)
            continue;

        /* The value is the next token even when it starts with '-' (-s -1) */
        switch (Sw[1])
        {
            case L's':
                if (ParseNumber(argv[i + 1], &Number))
                {
                    Args->TimeoutMs = Number;
                    i++;
                }
                break;
            case L'f':
                if (ParseNumber(argv[i + 1], &Number))
                {
                    Args->Flags = (ULONG)Number;
                    i++;
                }
                break;
            case L'a':
                if (ParseNumber(argv[i + 1], &Number))
                {
                    Args->Action = (ULONG)Number;
                    Args->HasAction = TRUE;
                    i++;
                }
                break;
            case L't':
                Args->Title = argv[++i];
                break;
            case L'm':
                Args->Message = argv[++i];
                break;
            default:
                break;
        }
    }
}

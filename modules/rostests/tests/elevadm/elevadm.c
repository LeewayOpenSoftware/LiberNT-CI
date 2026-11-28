/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A requireAdministrator program that reports the integrity level and elevation it runs with
 * COPYRIGHT:   Copyright 2026 Leeway Open Software <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    UCHAR Buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    PTOKEN_MANDATORY_LABEL Label = (PTOKEN_MANDATORY_LABEL)Buffer;
    TOKEN_ELEVATION Elevation = { 0 };
    TOKEN_ELEVATION_TYPE Type = 0;
    DWORD Rid = 0, Length;
    HANDLE Token;
    char Line[128];
    FILE *Out;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        printf("OpenProcessToken failed %lu\n", GetLastError());
        return 1;
    }
    if (GetTokenInformation(Token, TokenIntegrityLevel, Label, sizeof(Buffer), &Length))
        Rid = *GetSidSubAuthority(Label->Label.Sid, *GetSidSubAuthorityCount(Label->Label.Sid) - 1);
    GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Length);
    GetTokenInformation(Token, TokenElevationType, &Type, sizeof(Type), &Length);
    CloseHandle(Token);

    sprintf(Line, "elevadm: integrity 0x%lx elevated %lu elevation type %d\n", Rid, Elevation.TokenIsElevated, (int)Type);
    printf("%s", Line);
    if (argc > 1)
    {
        Out = fopen(argv[1], "w");
        if (Out)
        {
            fputs(Line, Out);
            fclose(Out);
        }
    }
    else
    {
        printf("Press Enter to exit\n");
        getchar();
    }
    return (int)Rid;
}

/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Start a command line with a Medium integrity copy of the caller's token, or run a program with the runas verb
 * COPYRIGHT:   Copyright 2026 Leeway Open Software <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#define MEDIUM_RID 0x2000

static int RunAs(int argc, char **argv)
{
    char params[1024];
    SHELLEXECUTEINFOA Sei;
    DWORD Exit = 1;
    int i;

    params[0] = 0;
    for (i = 3; i < argc; i++)
    {
        if (i > 3)
            strcat(params, " ");
        strncat(params, argv[i], sizeof(params) - strlen(params) - 2);
    }
    ZeroMemory(&Sei, sizeof(Sei));
    Sei.cbSize = sizeof(Sei);
    Sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    Sei.lpVerb = "runas";
    Sei.lpFile = argv[2];
    Sei.lpParameters = params[0] ? params : NULL;
    Sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&Sei))
    {
        printf("ShellExecuteEx(runas) failed %lu\n", GetLastError());
        return 1;
    }
    if (!Sei.hProcess)
    {
        printf("ShellExecuteEx(runas) returned no process\n");
        return 1;
    }
    WaitForSingleObject(Sei.hProcess, INFINITE);
    GetExitCodeProcess(Sei.hProcess, &Exit);
    CloseHandle(Sei.hProcess);
    printf("runas child exit code %lu\n", Exit);
    return (int)Exit;
}

int main(int argc, char **argv)
{
    static SID_IDENTIFIER_AUTHORITY LabelAuthority = { { 0, 0, 0, 0, 0, 16 } };
    char cmdline[1024];
    HANDLE Token = NULL, Primary = NULL;
    PSID Label = NULL;
    TOKEN_MANDATORY_LABEL Tml;
    STARTUPINFOA Si;
    PROCESS_INFORMATION Pi;
    DWORD Exit = 1;
    int i;

    if (argc < 2)
    {
        printf("usage: lowil <command line>\n       lowil --runas <program> [arguments]\n");
        return 2;
    }
    if (!strcmp(argv[1], "--runas"))
    {
        if (argc < 3)
        {
            printf("usage: lowil --runas <program> [arguments]\n");
            return 2;
        }
        return RunAs(argc, argv);
    }
    cmdline[0] = 0;
    for (i = 1; i < argc; i++)
    {
        if (i > 1)
            strcat(cmdline, " ");
        strncat(cmdline, argv[i], sizeof(cmdline) - strlen(cmdline) - 2);
    }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &Token))
    {
        printf("OpenProcessToken failed %lu\n", GetLastError());
        return 1;
    }
    if (!DuplicateTokenEx(Token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &Primary))
    {
        printf("DuplicateTokenEx failed %lu\n", GetLastError());
        return 1;
    }
    if (!AllocateAndInitializeSid(&LabelAuthority, 1, MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0, &Label))
    {
        printf("AllocateAndInitializeSid failed %lu\n", GetLastError());
        return 1;
    }
    Tml.Label.Sid = Label;
    Tml.Label.Attributes = SE_GROUP_INTEGRITY;
    if (!SetTokenInformation(Primary, TokenIntegrityLevel, &Tml, sizeof(Tml) + GetLengthSid(Label)))
    {
        printf("SetTokenInformation(TokenIntegrityLevel) failed %lu\n", GetLastError());
        return 1;
    }
    ZeroMemory(&Si, sizeof(Si));
    Si.cb = sizeof(Si);
    if (!CreateProcessAsUserA(Primary, NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &Si, &Pi))
    {
        printf("CreateProcessAsUser failed %lu\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(Pi.hProcess, INFINITE);
    GetExitCodeProcess(Pi.hProcess, &Exit);
    CloseHandle(Pi.hProcess);
    CloseHandle(Pi.hThread);
    FreeSid(Label);
    CloseHandle(Primary);
    CloseHandle(Token);
    return (int)Exit;
}

/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests bcdedit editing a /store hive: each verb's exit code and the bytes it writes
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

/* The elements used below, by identifier, so the test checks the stored bytes directly. */
#define EL_DESCRIPTION  0x12000004  /* string  -> REG_SZ */
#define EL_INHERIT      0x14000006  /* list    -> REG_MULTI_SZ */
#define EL_DEBUG        0x260000a0  /* boolean -> REG_BINARY, 1 byte */
#define EL_DEFAULT      0x23000003  /* object  -> REG_SZ, on {bootmgr} */
#define EL_TIMEOUT      0x25000004  /* integer -> REG_BINARY, 8 bytes LE, on {bootmgr} */
#define EL_DISPLAYORDER 0x24000001  /* list    -> REG_MULTI_SZ, on {bootmgr} */

static const WCHAR Loader[]  = L"{11111111-2222-3333-4444-555555555555}";
static const WCHAR Other[]   = L"{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}";
static const WCHAR BootMgr[] = L"{9dea862c-5cdd-4e70-acc1-f32b344d4795}";

/* Create the hive file (RegLoadAppKeyW makes it if absent) and seed two bare objects, so /set
 * has an existing object to edit; /create is a later change. The handle is closed, flushing the
 * hive to the file, before bcdedit opens the same file. */
static BOOL
SeedStore(PCWSTR File)
{
    HKEY Root, Object, Description;
    DWORD Type;
    LONG Error;
    int i;
    const WCHAR *Objects[] = { Loader, BootMgr };
    const DWORD Types[] = { 0x10200003, 0x10100002 };

    Error = RegLoadAppKeyW(File, &Root, KEY_ALL_ACCESS, 0, 0);
    if (Error != ERROR_SUCCESS)
        return FALSE;

    for (i = 0; i < 2; i++)
    {
        WCHAR Path[128];
        StringCchPrintfW(Path, ARRAYSIZE(Path), L"Objects\\%s\\Description", Objects[i]);
        if (RegCreateKeyExW(Root, Path, 0, NULL, 0, KEY_WRITE, NULL, &Description, NULL) == ERROR_SUCCESS)
        {
            Type = Types[i];
            RegSetValueExW(Description, L"Type", 0, REG_DWORD, (const BYTE *)&Type, sizeof(Type));
            RegCloseKey(Description);
        }
        StringCchPrintfW(Path, ARRAYSIZE(Path), L"Objects\\%s\\Elements", Objects[i]);
        if (RegCreateKeyExW(Root, Path, 0, NULL, 0, KEY_WRITE, NULL, &Object, NULL) == ERROR_SUCCESS)
            RegCloseKey(Object);
    }

    RegCloseKey(Root);
    return TRUE;
}

/* Run "bcdedit <Args>" to completion and return its exit code, or (DWORD)-1 if it could not be
 * started. When Out is given, stdout is captured into it (NUL-terminated); bcdedit writes a guid
 * as plain ASCII, so the id it prints for /create and /copy can be read back. */
static DWORD
RunBcdCapture(PCWSTR Args, char *Out, DWORD OutSize)
{
    WCHAR CmdLine[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE Sink = INVALID_HANDLE_VALUE, ReadEnd = INVALID_HANDLE_VALUE;
    DWORD Code = (DWORD)-1;

    StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"bcdedit %s", Args);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (Out != NULL)
    {
        if (OutSize > 0)
            Out[0] = 0;
        if (!CreatePipe(&ReadEnd, &Sink, &sa, 0))
            return Code;
        SetHandleInformation(ReadEnd, HANDLE_FLAG_INHERIT, 0);
    }
    else
    {
        Sink = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, NULL);
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = Sink;
    si.hStdError = Sink;

    if (CreateProcessW(NULL, CmdLine, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        if (Out != NULL)
        {
            DWORD Total = 0, Got;
            CloseHandle(Sink);          /* the child owns the only write end now */
            Sink = INVALID_HANDLE_VALUE;
            while (Total + 1 < OutSize &&
                   ReadFile(ReadEnd, Out + Total, OutSize - 1 - Total, &Got, NULL) && Got > 0)
                Total += Got;
            Out[Total] = 0;
        }
        /* Kill a hung child so it releases the fixture hive and the file can be deleted. */
        if (WaitForSingleObject(pi.hProcess, 20000) != WAIT_OBJECT_0)
            TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &Code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (Sink != INVALID_HANDLE_VALUE)
        CloseHandle(Sink);
    if (ReadEnd != INVALID_HANDLE_VALUE)
        CloseHandle(ReadEnd);
    return Code;
}

static DWORD
RunBcd(PCWSTR Args)
{
    return RunBcdCapture(Args, NULL, 0);
}

/* Read Objects\<Id>\Elements\<Type>\Element from the hive file into Buffer; returns its registry
 * type and byte length, or sets *Length to (DWORD)-1 when the element is absent. */
static DWORD
ReadElement(PCWSTR File, PCWSTR Id, ULONG Type, BYTE *Buffer, DWORD *Length)
{
    HKEY Root, Key;
    WCHAR Path[160];
    DWORD RegType = 0, Size = *Length;
    LONG Error;

    /* Clear the buffer so an absent element leaves an empty string, not the previous read. */
    memset(Buffer, 0, *Length);
    *Length = (DWORD)-1;
    if (RegLoadAppKeyW(File, &Root, KEY_READ, 0, 0) != ERROR_SUCCESS)
        return 0;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"Objects\\%s\\Elements\\%08lx", Id, Type);
    Error = RegOpenKeyExW(Root, Path, 0, KEY_READ, &Key);
    if (Error == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(Key, L"Element", NULL, &RegType, Buffer, &Size) == ERROR_SUCCESS)
            *Length = Size;
        RegCloseKey(Key);
    }
    RegCloseKey(Root);
    return RegType;
}

/* Objects\<Id>\Description\Type from the hive file, or 0 if the object or value is absent. */
static DWORD
ReadObjectType(PCWSTR File, PCWSTR Id)
{
    HKEY Root, Key;
    WCHAR Path[160];
    DWORD Type = 0, Size = sizeof(Type), RegType = 0;

    if (RegLoadAppKeyW(File, &Root, KEY_READ, 0, 0) != ERROR_SUCCESS)
        return 0;
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"Objects\\%s\\Description", Id);
    if (RegOpenKeyExW(Root, Path, 0, KEY_READ, &Key) == ERROR_SUCCESS)
    {
        RegQueryValueExW(Key, L"Type", NULL, &RegType, (BYTE *)&Type, &Size);
        RegCloseKey(Key);
    }
    RegCloseKey(Root);
    return Type;
}

START_TEST(BcdEdit)
{
    WCHAR File[MAX_PATH], Dir[MAX_PATH], Args[512];
    WCHAR Buffer[128], NewId[64];
    BYTE *B = (BYTE *)Buffer;
    char Out[256];
    DWORD Length, RegType;
    DWORD Code;
    static const WCHAR Fresh[] = L"{cccccccc-1111-2222-3333-444444444444}";
    static const WCHAR Src[]   = L"{dddddddd-1111-2222-3333-444444444444}";

    GetTempPathW(ARRAYSIZE(Dir), Dir);
    StringCchPrintfW(File, ARRAYSIZE(File), L"%sbcdedit_fixture_%lu.hiv", Dir, GetCurrentProcessId());
    DeleteFileW(File);

    if (!SeedStore(File))
    {
        skip("Could not create the fixture store (RegLoadAppKeyW failed)\n");
        DeleteFileW(File);
        return;
    }

    /* string round-trip: /set <loader> description "Boot Entry" -> REG_SZ. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s description \"Boot Entry\"", File, Loader);
    Code = RunBcd(Args);
    ok(Code == 0, "/set description exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, Loader, EL_DESCRIPTION, B, &Length);
    ok(RegType == REG_SZ && Length == (DWORD)((wcslen(L"Boot Entry") + 1) * sizeof(WCHAR)) &&
       wcscmp(Buffer, L"Boot Entry") == 0, "description bytes: type %lu len %lu\n", RegType, Length);

    /* integer round-trip, hex input: /set <loader> timeout 0x1e -> 30 as 8 bytes LE. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s timeout 0x1e", File, Loader);
    Code = RunBcd(Args);
    ok(Code == 0, "/set timeout 0x1e exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, Loader, EL_TIMEOUT, B, &Length);
    ok(RegType == REG_BINARY && Length == 8 && B[0] == 30 && B[1] == 0 && B[7] == 0,
       "timeout bytes: type %lu len %lu b0 %u\n", RegType, Length, B[0]);

    /* boolean round-trip: /set <loader> debug yes -> one byte 0x01. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s debug yes", File, Loader);
    Code = RunBcd(Args);
    ok(Code == 0, "/set debug yes exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, Loader, EL_DEBUG, B, &Length);
    ok(RegType == REG_BINARY && Length == 1 && B[0] == 1, "debug bytes: type %lu len %lu b0 %u\n",
       RegType, Length, B[0]);

    /* object-list round-trip: /set <loader> inherit <other> -> REG_MULTI_SZ, one id. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s inherit %s", File, Loader, Other);
    Code = RunBcd(Args);
    ok(Code == 0, "/set inherit exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, Loader, EL_INHERIT, B, &Length);
    ok(RegType == REG_MULTI_SZ && wcscmp(Buffer, Other) == 0 &&
       Length == (DWORD)((wcslen(Other) + 2) * sizeof(WCHAR)),
       "inherit type %lu len %lu\n", RegType, Length);

    /* /timeout and /default target {bootmgr} by its well-known id. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /timeout 15", File);
    Code = RunBcd(Args);
    ok(Code == 0, "/timeout exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, BootMgr, EL_TIMEOUT, B, &Length);
    ok(RegType == REG_BINARY && Length == 8 && B[0] == 15, "/timeout bytes: type %lu b0 %u\n",
       RegType, B[0]);

    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /default %s", File, Loader);
    Code = RunBcd(Args);
    ok(Code == 0, "/default exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, BootMgr, EL_DEFAULT, B, &Length);
    ok(RegType == REG_SZ && wcscmp(Buffer, Loader) == 0, "/default stored %ls\n", Buffer);

    /* /displayorder sets {bootmgr} DisplayOrder to exactly the given ids, as a REG_MULTI_SZ. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /displayorder %s", File, Loader);
    Code = RunBcd(Args);
    ok(Code == 0, "/displayorder set exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, BootMgr, EL_DISPLAYORDER, B, &Length);
    ok(RegType == REG_MULTI_SZ && wcscmp(Buffer, Loader) == 0 &&
       Length == (DWORD)((wcslen(Loader) + 2) * sizeof(WCHAR)),
       "/displayorder set: type %lu len %lu\n", RegType, Length);

    /* /addlast appends: the list becomes loader then other. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /displayorder %s /addlast", File, Other);
    Code = RunBcd(Args);
    ok(Code == 0, "/displayorder addlast exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, BootMgr, EL_DISPLAYORDER, B, &Length);
    ok(RegType == REG_MULTI_SZ && wcscmp(Buffer, Loader) == 0 &&
       wcscmp(Buffer + wcslen(Loader) + 1, Other) == 0, "/displayorder addlast order wrong\n");

    /* /remove drops loader, leaving other as the single entry. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /displayorder %s /remove", File, Loader);
    Code = RunBcd(Args);
    ok(Code == 0, "/displayorder remove exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, BootMgr, EL_DISPLAYORDER, B, &Length);
    ok(RegType == REG_MULTI_SZ && wcscmp(Buffer, Other) == 0 &&
       Length == (DWORD)((wcslen(Other) + 2) * sizeof(WCHAR)),
       "/displayorder remove: type %lu len %lu\n", RegType, Length);

    /* /addfirst prepends: the list becomes loader then other. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /displayorder %s /addfirst", File, Loader);
    Code = RunBcd(Args);
    ok(Code == 0, "/displayorder addfirst exit %lu\n", Code);
    Length = sizeof(Buffer);
    RegType = ReadElement(File, BootMgr, EL_DISPLAYORDER, B, &Length);
    ok(RegType == REG_MULTI_SZ && wcscmp(Buffer, Loader) == 0 &&
       wcscmp(Buffer + wcslen(Loader) + 1, Other) == 0, "/displayorder addfirst order wrong\n");

    /* A non-guid id is rejected. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /displayorder notaguid", File);
    ok(RunBcd(Args) == 1, "/displayorder with a non-guid id should be rejected\n");

    /* /deletevalue removes the element; deleting it again, now absent, fails with exit 1. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /deletevalue %s description", File, Loader);
    Code = RunBcd(Args);
    ok(Code == 0, "/deletevalue present exit %lu\n", Code);
    Length = sizeof(Buffer);
    ReadElement(File, Loader, EL_DESCRIPTION, B, &Length);
    ok(Length == (DWORD)-1, "description should be gone, len %lu\n", Length);
    Code = RunBcd(Args);
    ok(Code == 1, "/deletevalue missing should exit 1, got %lu\n", Code);

    /* Rejections, each exit 1: an unresolved {default} id, an unknown element, a non-numeric
     * integer, a non-boolean, and a value that is not a valid guid for a list. */
    /* {default}/{current} are rejected until their resolution lands; then this becomes a success
     * case, so a later failure here is the change, not a regression. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set {default} description X", File);
    ok(RunBcd(Args) == 1, "{default} id should be rejected\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s notanelement X", File, Loader);
    ok(RunBcd(Args) == 1, "unknown element should be rejected\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s timeout 12x", File, Loader);
    ok(RunBcd(Args) == 1, "non-numeric timeout should be rejected\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s timeout \" 7\"", File, Loader);
    ok(RunBcd(Args) == 1, "leading-space integer should be rejected\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s debug maybe", File, Loader);
    ok(RunBcd(Args) == 1, "non-boolean should be rejected\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s inherit notaguid", File, Loader);
    ok(RunBcd(Args) == 1, "non-guid list value should be rejected\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set {bbbbbbbb-cccc-dddd-eeee-ffffffffffff} description X", File);
    ok(RunBcd(Args) == 1, "/set on a missing object should be rejected\n");

    /* /create makes a new OS loader object with the given description. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /create %s /d \"Fresh Entry\" /application osloader", File, Fresh);
    Code = RunBcd(Args);
    ok(Code == 0, "/create exit %lu\n", Code);
    ok(ReadObjectType(File, Fresh) == 0x10200003, "/create type %08lx\n", ReadObjectType(File, Fresh));
    Length = sizeof(Buffer);
    RegType = ReadElement(File, Fresh, EL_DESCRIPTION, B, &Length);
    ok(RegType == REG_SZ && wcscmp(Buffer, L"Fresh Entry") == 0, "/create description wrong\n");

    /* Creating the same id again fails, since the object already exists. */
    Code = RunBcd(Args);
    ok(Code == 1, "/create of an existing id should exit 1, got %lu\n", Code);

    /* A non-guid id, or a request with no class, is rejected. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /create notaguid /d X /application osloader", File);
    ok(RunBcd(Args) == 1, "/create with a non-guid id should be rejected\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /create %s /d X", File, Fresh);
    ok(RunBcd(Args) == 1, "/create with no class should be rejected\n");

    /* /copy duplicates an object's class and elements under a fresh id that it prints. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /create %s /d \"Copy Source\" /application osloader", File, Src);
    ok(RunBcd(Args) == 0, "/copy source create failed\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /set %s timeout 0x2a", File, Src);
    ok(RunBcd(Args) == 0, "/copy source /set failed\n");
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /copy %s /d \"Copied\"", File, Src);
    Out[0] = 0;
    Code = RunBcdCapture(Args, Out, sizeof(Out));
    ok(Code == 0, "/copy exit %lu\n", Code);
    {
        char *End = strchr(Out, '}');
        int Len = End ? (int)(End - Out) + 1 : 0;
        if (Len > 0 && Len < ARRAYSIZE(NewId))
        {
            MultiByteToWideChar(CP_ACP, 0, Out, Len, NewId, ARRAYSIZE(NewId));
            NewId[Len] = 0;
            ok(_wcsicmp(NewId, Src) != 0, "/copy must use a new id, not the source\n");
            ok(ReadObjectType(File, NewId) == 0x10200003, "/copy did not carry the class\n");
            Length = sizeof(Buffer);
            RegType = ReadElement(File, NewId, EL_TIMEOUT, B, &Length);
            ok(RegType == REG_BINARY && Length == 8 && B[0] == 0x2a, "/copy did not carry the timeout\n");
            Length = sizeof(Buffer);
            RegType = ReadElement(File, NewId, EL_DESCRIPTION, B, &Length);
            ok(RegType == REG_SZ && wcscmp(Buffer, L"Copied") == 0, "/copy did not set the new description\n");
        }
        else
        {
            ok(0, "/copy did not print a new id: '%.40s'\n", Out);
        }
    }

    /* /enum over the /store file opens and lists it. */
    StringCchPrintfW(Args, ARRAYSIZE(Args), L"/store \"%s\" /enum", File);
    ok(RunBcd(Args) == 0, "/enum of the store should succeed\n");

    DeleteFileW(File);
}

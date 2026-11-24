/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Runs a credential provider serialization through LsaLogonUser as SYSTEM
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <stdio.h>
#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <wincrypt.h>
#include <dbghelp.h>
#include <sddl.h>
#include <lm.h>
#include <ntsecapi.h>
#include <credentialprovider.h>
#include <credser.h>

#define PASSWORD_CHARS 257
#define SCAN_CHUNK (1024 * 1024)

static HANDLE Com1 = INVALID_HANDLE_VALUE;

static VOID
Report(_In_ PCSTR Format, ...)
{
    char Line[512];
    va_list Args;
    DWORD Written;
    int Length;

    va_start(Args, Format);
    Length = _vsnprintf(Line, sizeof(Line) - 1, Format, Args);
    va_end(Args);
    if (Length < 0)
        Length = sizeof(Line) - 1;
    Line[Length] = 0;
    fputs(Line, stdout);
    if (Com1 != INVALID_HANDLE_VALUE)
        WriteFile(Com1, Line, (DWORD)Length, &Written, NULL);
}

static VOID
WipeFile(_In_ HANDLE File)
{
    static const BYTE Zeros[64 * 1024];
    LARGE_INTEGER Size;
    DWORD Chunk, Written;

    if (!GetFileSizeEx(File, &Size) || SetFilePointer(File, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return;
    while (Size.QuadPart > 0)
    {
        Chunk = (DWORD)min(Size.QuadPart, (LONGLONG)sizeof(Zeros));
        if (!WriteFile(File, Zeros, Chunk, &Written, NULL) || !Written)
            break;
        Size.QuadPart -= Written;
    }
    FlushFileBuffers(File);
}

static BOOL
ReadPasswordFile(_In_ PCWSTR Path, _Out_writes_(PASSWORD_CHARS) PWSTR Password)
{
    PCHAR Raw;
    HANDLE File;
    DWORD Read = 0;
    BOOL Ok;
    int Chars = 0;

    Password[0] = UNICODE_NULL;
    Raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PASSWORD_CHARS + 2);
    if (!Raw)
        return FALSE;
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        HeapFree(GetProcessHeap(), 0, Raw);
        return FALSE;
    }
    Ok = ReadFile(File, Raw, PASSWORD_CHARS + 1, &Read, NULL);
    WipeFile(File);
    CloseHandle(File);
    DeleteFileW(Path);
    while (Ok && Read && (Raw[Read - 1] == '\r' || Raw[Read - 1] == '\n' || Raw[Read - 1] == ' '))
        Read--;
    if (Ok && Read && Read < PASSWORD_CHARS)
        Chars = MultiByteToWideChar(CP_ACP, 0, Raw, (int)Read, Password, PASSWORD_CHARS - 1);
    Password[Chars] = UNICODE_NULL;
    SecureZeroMemory(Raw, PASSWORD_CHARS + 2);
    HeapFree(GetProcessHeap(), 0, Raw);
    return Chars > 0;
}

static VOID
WriteRawDump(_In_ HANDLE File)
{
    MEMORY_BASIC_INFORMATION Mbi;
    PBYTE Address = NULL, Next;
    DWORD Written;

    while (VirtualQuery(Address, &Mbi, sizeof(Mbi)) == sizeof(Mbi))
    {
        if (Mbi.State == MEM_COMMIT && !(Mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && Mbi.RegionSize <= MAXDWORD)
            WriteFile(File, Mbi.BaseAddress, (DWORD)Mbi.RegionSize, &Written, NULL);
        Next = (PBYTE)Mbi.BaseAddress + Mbi.RegionSize;
        if (Next <= Address)
            break;
        Address = Next;
    }
}

static VOID
WriteSelfDump(_In_ PCWSTR Path, _In_ PCSTR Stage, _In_ BOOL Raw)
{
    LARGE_INTEGER Size;
    PCSTR Method = "minidump";
    DWORD Error = 0;
    HANDLE File;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        Report("HARNESS dump %s create error %lu\n", Stage, GetLastError());
        return;
    }
    if (Raw || !MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), File, MiniDumpWithFullMemory, NULL,
                                  NULL, NULL))
    {
        if (!Raw)
            Error = GetLastError();
        Method = "raw";
        SetFilePointer(File, 0, NULL, FILE_BEGIN);
        SetEndOfFile(File);
        WriteRawDump(File);
    }
    Size.QuadPart = 0;
    GetFileSizeEx(File, &Size);
    CloseHandle(File);
    Report("HARNESS dump %s method %s minidump-error %lu size %I64u\n", Stage, Method, Error, Size.QuadPart);
}

static ULONG
CountFrom(_In_reads_bytes_(Total) const BYTE *Data, _In_ ULONG Total, _In_ ULONGLONG Base,
          _In_reads_bytes_(Length) const BYTE *Pattern, _In_ ULONG Length, _Inout_ PULONGLONG Next)
{
    ULONG Hits = 0, Offset;

    if (Total < Length)
        return 0;
    Offset = (*Next > Base) ? (ULONG)(*Next - Base) : 0;
    for (; Offset + Length <= Total; Offset++)
    {
        if (Data[Offset] == Pattern[0] && !memcmp(Data + Offset, Pattern, Length))
            Hits++;
    }
    if (Base + Total - Length + 1 > *Next)
        *Next = Base + Total - Length + 1;
    return Hits;
}

static int
RunScan(_In_ PCWSTR DumpPath, _In_ PCWSTR Password)
{
    CHAR Narrow[PASSWORD_CHARS * 2], Hex[65];
    ULONG WideLength, NarrowLength, Keep = 0, Total, i;
    ULONG WideHits = 0, NarrowHits = 0;
    ULONGLONG Base = 0, WideNext = 0, NarrowNext = 0;
    HCRYPTPROV Prov = 0;
    HCRYPTHASH Hash = 0;
    BYTE Digest[32];
    DWORD DigestSize = sizeof(Digest), Read;
    PBYTE Chunk;
    HANDLE File;
    int Ret = 2;

    WideLength = (ULONG)wcslen(Password) * sizeof(WCHAR);
    NarrowLength = (ULONG)WideCharToMultiByte(CP_ACP, 0, Password, -1, Narrow, sizeof(Narrow), NULL, NULL);
    if (!WideLength || NarrowLength < 2)
        return 2;
    NarrowLength--;
    Chunk = HeapAlloc(GetProcessHeap(), 0, SCAN_CHUNK + sizeof(Narrow));
    File = CreateFileW(DumpPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (Chunk && File != INVALID_HANDLE_VALUE &&
        CryptAcquireContextW(&Prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(Prov, CALG_SHA_256, 0, 0, &Hash))
    {
        while (ReadFile(File, Chunk + Keep, SCAN_CHUNK, &Read, NULL) && Read)
        {
            CryptHashData(Hash, Chunk + Keep, Read, 0);
            Total = Keep + Read;
            WideHits += CountFrom(Chunk, Total, Base, (const BYTE *)Password, WideLength, &WideNext);
            NarrowHits += CountFrom(Chunk, Total, Base, (const BYTE *)Narrow, NarrowLength, &NarrowNext);
            Keep = min(Total, max(WideLength, NarrowLength) - 1);
            MoveMemory(Chunk, Chunk + Total - Keep, Keep);
            Base += Total - Keep;
        }
        if (CryptGetHashParam(Hash, HP_HASHVAL, Digest, &DigestSize, 0))
        {
            for (i = 0; i < DigestSize && i < 32; i++)
                sprintf(Hex + i * 2, "%02x", Digest[i]);
            Hex[i * 2] = 0;
            Report("HARNESS scan %ls size %I64u sha256 %s utf16-hits %lu ascii-hits %lu\n", DumpPath, Base + Keep, Hex,
                   WideHits, NarrowHits);
            Ret = 0;
        }
    }
    if (Ret)
        Report("HARNESS scan %ls error %lu\n", DumpPath, GetLastError());
    if (Hash)
        CryptDestroyHash(Hash);
    if (Prov)
        CryptReleaseContext(Prov, 0);
    if (Chunk)
    {
        SecureZeroMemory(Chunk, SCAN_CHUNK + sizeof(Narrow));
        HeapFree(GetProcessHeap(), 0, Chunk);
    }
    SecureZeroMemory(Narrow, sizeof(Narrow));
    if (File != INVALID_HANDLE_VALUE)
    {
        WipeFile(File);
        CloseHandle(File);
    }
    DeleteFileW(DumpPath);
    return Ret;
}

static NTSTATUS
LogonWithBlob(_In_ HANDLE Lsa, _In_ ULONG Msv, _In_reads_bytes_(Size) const VOID *Blob, _In_ ULONG Size,
              _Out_writes_opt_(SidChars) LPWSTR SidText, _In_ DWORD SidChars)
{
    CREDSER_LOGON Logon;
    PMSV1_0_INTERACTIVE_LOGON MsvLogon = NULL;
    ULONG MsvSize = 0, ProfileSize = 0;
    PVOID Copy = NULL, Profile = NULL;
    LSA_STRING Origin;
    TOKEN_SOURCE Source;
    LUID LogonId;
    HANDLE Token = NULL;
    QUOTA_LIMITS Quotas;
    NTSTATUS Status, SubStatus = 0;
    BYTE Buffer[256];
    DWORD Length;
    LPWSTR String;

    Status = CredSerCapture(Blob, Size, &Copy);
    if (NT_SUCCESS(Status))
        Status = CredSerUnpackLogon(Copy, Size, &Logon);
    if (NT_SUCCESS(Status))
        Status = CredSerBuildMsv1_0Logon(&Logon, &MsvLogon, &MsvSize);
    CredSerFree(Copy, Size);
    if (!NT_SUCCESS(Status))
        return Status;

    Origin.Buffer = "credprov_harness";
    Origin.Length = (USHORT)strlen(Origin.Buffer);
    Origin.MaximumLength = Origin.Length + 1;
    CopyMemory(Source.SourceName, "CredProv", TOKEN_SOURCE_LENGTH);
    AllocateLocallyUniqueId(&Source.SourceIdentifier);

    Status = LsaLogonUser(Lsa, &Origin, Interactive, Msv, MsvLogon, MsvSize, NULL, &Source, &Profile, &ProfileSize,
                          &LogonId, &Token, &Quotas, &SubStatus);
    CredSerFree(MsvLogon, MsvSize);
    if (Profile)
        LsaFreeReturnBuffer(Profile);
    if (!NT_SUCCESS(Status))
        return Status;

    if (SidText && SidChars)
    {
        SidText[0] = UNICODE_NULL;
        if (GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Length) &&
            ConvertSidToStringSidW(((TOKEN_USER *)Buffer)->User.Sid, &String))
        {
            lstrcpynW(SidText, String, SidChars);
            LocalFree(String);
        }
    }
    CloseHandle(Token);
    return Status;
}

static NTSTATUS
SerializeWithProvider(_In_ PCWSTR User, _In_ PCWSTR Password, _Out_ PBYTE *Blob, _Out_ PULONG Size)
{
    ICredentialProvider *Provider;
    ICredentialProviderCredential *Credential;
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE Response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION Cs;
    CREDENTIAL_PROVIDER_STATUS_ICON Icon;
    LPWSTR Text = NULL;
    DWORD Count, Default;
    BOOL AutoLogon;
    HRESULT hr;

    *Blob = NULL;
    *Size = 0;
    hr = CoCreateInstance(&CLSID_PasswordCredentialProvider, NULL, CLSCTX_INPROC_SERVER, &IID_ICredentialProvider,
                          (void **)&Provider);
    if (FAILED(hr))
        return STATUS_NOT_FOUND;
    hr = ICredentialProvider_SetUsageScenario(Provider, CPUS_LOGON, 0);
    if (SUCCEEDED(hr))
        hr = ICredentialProvider_GetCredentialCount(Provider, &Count, &Default, &AutoLogon);
    if (SUCCEEDED(hr))
        hr = ICredentialProvider_GetCredentialAt(Provider, 0, &Credential);
    if (FAILED(hr))
    {
        ICredentialProvider_Release(Provider);
        return STATUS_UNSUCCESSFUL;
    }
    ICredentialProviderCredential_SetStringValue(Credential, 1, User);
    ICredentialProviderCredential_SetStringValue(Credential, 2, Password);
    ZeroMemory(&Cs, sizeof(Cs));
    hr = ICredentialProviderCredential_GetSerialization(Credential, &Response, &Cs, &Text, &Icon);
    CoTaskMemFree(Text);
    ICredentialProviderCredential_SetDeselected(Credential);
    ICredentialProviderCredential_Release(Credential);
    ICredentialProvider_Release(Provider);
    if (FAILED(hr) || Response != CPGSR_RETURN_CREDENTIAL_FINISHED || !Cs.rgbSerialization)
        return STATUS_UNSUCCESSFUL;
    *Blob = Cs.rgbSerialization;
    *Size = Cs.cbSerialization;
    return STATUS_SUCCESS;
}

static int
RunLogon(_In_ HANDLE Lsa, _In_ ULONG Msv, _In_ PCWSTR User, _Inout_updates_(PASSWORD_CHARS) PWSTR Password,
         _In_opt_ PCWSTR DumpPath, _In_ BOOL DumpEarly, _In_ BOOL Raw)
{
    WCHAR Sid[128];
    PBYTE Blob;
    ULONG Size;
    NTSTATUS Status;

    Status = SerializeWithProvider(User, Password, &Blob, &Size);
    if (!NT_SUCCESS(Status))
    {
        SecureZeroMemory(Password, PASSWORD_CHARS * sizeof(WCHAR));
        Report("HARNESS logon %ls serialize 0x%08lx\n", User, Status);
        return 2;
    }
    Status = LogonWithBlob(Lsa, Msv, Blob, Size, Sid, ARRAYSIZE(Sid));
    if (DumpPath && DumpEarly)
        WriteSelfDump(DumpPath, "early", Raw);
    SecureZeroMemory(Blob, Size);
    CoTaskMemFree(Blob);
    SecureZeroMemory(Password, PASSWORD_CHARS * sizeof(WCHAR));
    if (DumpPath && !DumpEarly)
        WriteSelfDump(DumpPath, "late", Raw);
    Report("HARNESS logon %ls status 0x%08lx sid %ls\n", User, Status, NT_SUCCESS(Status) ? Sid : L"-");
    return 0;
}

static int
RunCorpus(_In_ HANDLE Lsa, _In_ ULONG Msv)
{
    UCHAR Valid[512], Blob[512];
    PKERB_INTERACTIVE_UNLOCK_LOGON Packed = (PKERB_INTERACTIVE_UNLOCK_LOGON)Blob;
    ULONG Size = sizeof(Valid), Cut, Rejected = 0, Accepted = 0, Seed = 0x1badb002, Round, i;
    NTSTATUS Status;

    Status = CredSerPackLogon(KerbInteractiveLogon, NULL, L"credprovtest", L"wrong-password", NULL, Valid, &Size);
    if (!NT_SUCCESS(Status))
        return 2;

    for (Cut = 0; Cut < Size; Cut++)
    {
        Status = LogonWithBlob(Lsa, Msv, Valid, Cut, NULL, 0);
        if (Status == STATUS_INVALID_PARAMETER)
            Rejected++;
        else
            Accepted++;
    }
    for (Round = 0; Round < 500; Round++)
    {
        CopyMemory(Blob, Valid, Size);
        for (i = 0; i < 4; i++)
        {
            Seed = Seed * 1103515245 + 12345;
            Blob[(Seed >> 8) % Size] = (UCHAR)(Seed >> 16);
        }
        Status = LogonWithBlob(Lsa, Msv, Blob, Size, NULL, 0);
        if (Status == STATUS_INVALID_PARAMETER)
            Rejected++;
        else if (NT_SUCCESS(Status))
            Report("HARNESS corpus round %lu logged on\n", Round);
    }
    CopyMemory(Blob, Valid, Size);
    Packed->Logon.UserName.Buffer = (PWSTR)(ULONG_PTR)0xFFFFFFF0;
    Status = LogonWithBlob(Lsa, Msv, Blob, Size, NULL, 0);
    if (Status == STATUS_INVALID_PARAMETER)
        Rejected++;
    else
        Accepted++;

    Report("HARNESS corpus truncation+offset rejected %lu accepted %lu\n", Rejected, Accepted);
    return Accepted ? 1 : 0;
}

static BOOL
LockoutDisabled(void)
{
    USER_MODALS_INFO_3 *Info = NULL;
    BOOL Ret;

    if (NetUserModalsGet(NULL, 3, (LPBYTE *)&Info) != NERR_Success)
        return FALSE;
    Ret = Info->usrmod3_lockout_threshold == 0;
    NetApiBufferFree(Info);
    return Ret;
}

int
wmain(int argc, WCHAR **argv)
{
    LSA_STRING Name, Package;
    LSA_OPERATIONAL_MODE Mode;
    HANDLE Lsa;
    ULONG Msv;
    NTSTATUS Status;
    PCWSTR PasswordFile = NULL, DumpPath = NULL;
    PWSTR Password = NULL;
    BOOL DumpEarly = FALSE, Raw = FALSE, IsLogon, IsScan;
    int Ret = 2, i;

    Com1 = CreateFileW(L"\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    IsLogon = argc >= 3 && !wcscmp(argv[1], L"logon");
    IsScan = argc >= 3 && !wcscmp(argv[1], L"scan");
    for (i = 3; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--pwfile") && i + 1 < argc)
            PasswordFile = argv[++i];
        else if (!wcscmp(argv[i], L"--dump") && i + 1 < argc)
            DumpPath = argv[++i];
        else if (!wcscmp(argv[i], L"--dump-early"))
            DumpEarly = TRUE;
        else if (!wcscmp(argv[i], L"--raw"))
            Raw = TRUE;
    }
    if (!IsLogon && !IsScan && (argc < 2 || wcscmp(argv[1], L"corpus")))
    {
        Report("usage: credprov_harness logon <user> --pwfile <file> [--dump <file> [--dump-early] [--raw]]\n"
               "       credprov_harness scan <dump> --pwfile <file>\n"
               "       credprov_harness corpus\n");
        goto Exit;
    }
    if (IsLogon || IsScan)
    {
        Password = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PASSWORD_CHARS * sizeof(WCHAR));
        if (!Password || !PasswordFile || !ReadPasswordFile(PasswordFile, Password))
        {
            Report("HARNESS password file missing or unreadable\n");
            goto Exit;
        }
    }
    if (IsScan)
    {
        Ret = RunScan(argv[2], Password);
        goto Exit;
    }
    if (!LockoutDisabled())
    {
        Report("HARNESS refused: the account lockout threshold must be 0\n");
        Ret = 3;
        goto Exit;
    }

    Name.Buffer = "credprov_harness";
    Name.Length = (USHORT)strlen(Name.Buffer);
    Name.MaximumLength = Name.Length + 1;
    Status = LsaRegisterLogonProcess(&Name, &Lsa, &Mode);
    if (!NT_SUCCESS(Status))
    {
        Report("HARNESS LsaRegisterLogonProcess 0x%08lx (needs SYSTEM)\n", Status);
        Ret = 3;
        goto Exit;
    }
    Package.Buffer = MSV1_0_PACKAGE_NAME;
    Package.Length = (USHORT)strlen(Package.Buffer);
    Package.MaximumLength = Package.Length + 1;
    Status = LsaLookupAuthenticationPackage(Lsa, &Package, &Msv);
    if (NT_SUCCESS(Status))
    {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (IsLogon)
            Ret = RunLogon(Lsa, Msv, argv[2], Password, DumpPath, DumpEarly, Raw);
        else
            Ret = RunCorpus(Lsa, Msv);
        CoUninitialize();
        Status = LsaLookupAuthenticationPackage(Lsa, &Package, &Msv);
        Report("HARNESS lsa alive 0x%08lx\n", Status);
    }
    LsaDeregisterLogonProcess(Lsa);

Exit:
    if (Password)
    {
        SecureZeroMemory(Password, PASSWORD_CHARS * sizeof(WCHAR));
        HeapFree(GetProcessHeap(), 0, Password);
    }
    if (Com1 != INVALID_HANDLE_VALUE)
        CloseHandle(Com1);
    return Ret;
}

/*
 * PROJECT:         LiberNT Kernel
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         Capability SID derivation and capability checks
 * COPYRIGHT:       Copyright 2026 LeewayOpenSoftware
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define RTL_NT_CURRENT_PROCESS_TOKEN          ((HANDLE)(LONG_PTR)-4)
#define RTL_NT_CURRENT_THREAD_TOKEN           ((HANDLE)(LONG_PTR)-5)
#define RTL_NT_CURRENT_THREAD_EFFECTIVE_TOKEN ((HANDLE)(LONG_PTR)-6)

#define RTL_CAPABILITY_TAG 'TAPC'

static const ULONG RtlpSha256K[64] =
{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

typedef struct _RTL_SHA256_STATE
{
    ULONG H[8];
    ULONGLONG TotalLength;
    UCHAR Block[64];
    ULONG BlockLength;
} RTL_SHA256_STATE, *PRTL_SHA256_STATE;

static
VOID
RtlpSha256Initialize(
    _Out_ PRTL_SHA256_STATE State)
{
    static const ULONG InitialH[8] =
    {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    RtlCopyMemory(State->H, InitialH, sizeof(InitialH));
    State->TotalLength = 0;
    State->BlockLength = 0;
}

#define RTL_SHA256_ROTR(_Value, _Bits) (((_Value) >> (_Bits)) | ((_Value) << (32 - (_Bits))))

static
VOID
RtlpSha256ProcessBlock(
    _Inout_ PRTL_SHA256_STATE State,
    _In_reads_(64) PCUCHAR Block)
{
    ULONG W[64];
    ULONG a, b, c, d, e, f, g, h;
    ULONG t1, t2;
    ULONG i;

    for (i = 0; i < 16; i++)
    {
        W[i] = (((ULONG)((PUCHAR)Block)[i * 4 + 0]) << 24) |
               (((ULONG)((PUCHAR)Block)[i * 4 + 1]) << 16) |
               (((ULONG)((PUCHAR)Block)[i * 4 + 2]) << 8) |
               (((ULONG)((PUCHAR)Block)[i * 4 + 3]));
    }
    for (i = 16; i < 64; i++)
    {
        ULONG s0 = RTL_SHA256_ROTR(W[i - 15], 7) ^ RTL_SHA256_ROTR(W[i - 15], 18) ^ (W[i - 15] >> 3);
        ULONG s1 = RTL_SHA256_ROTR(W[i - 2], 17) ^ RTL_SHA256_ROTR(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }

    a = State->H[0];
    b = State->H[1];
    c = State->H[2];
    d = State->H[3];
    e = State->H[4];
    f = State->H[5];
    g = State->H[6];
    h = State->H[7];

    for (i = 0; i < 64; i++)
    {
        ULONG S1 = RTL_SHA256_ROTR(e, 6) ^ RTL_SHA256_ROTR(e, 11) ^ RTL_SHA256_ROTR(e, 25);
        ULONG ch = (e & f) ^ ((~e) & g);
        ULONG S0;
        ULONG maj;

        t1 = h + S1 + ch + RtlpSha256K[i] + W[i];
        S0 = RTL_SHA256_ROTR(a, 2) ^ RTL_SHA256_ROTR(a, 13) ^ RTL_SHA256_ROTR(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    State->H[0] += a;
    State->H[1] += b;
    State->H[2] += c;
    State->H[3] += d;
    State->H[4] += e;
    State->H[5] += f;
    State->H[6] += g;
    State->H[7] += h;
}

static
VOID
RtlpSha256Update(
    _Inout_ PRTL_SHA256_STATE State,
    _In_reads_(Length) PCUCHAR Data,
    _In_ SIZE_T Length)
{
    State->TotalLength += Length;

    while (Length != 0)
    {
        ULONG Copy = 64 - State->BlockLength;
        if (Copy > Length)
            Copy = (ULONG)Length;

        RtlCopyMemory(&State->Block[State->BlockLength], Data, Copy);
        State->BlockLength += Copy;
        Data += Copy;
        Length -= Copy;

        if (State->BlockLength == 64)
        {
            RtlpSha256ProcessBlock(State, State->Block);
            State->BlockLength = 0;
        }
    }
}

static
VOID
RtlpSha256Finalize(
    _Inout_ PRTL_SHA256_STATE State,
    _Out_writes_(32) PUCHAR Digest)
{
    ULONGLONG BitCount = State->TotalLength * 8;
    UCHAR Padding[64];
    ULONG Tail;
    ULONG i;

    RtlZeroMemory(Padding, sizeof(Padding));
    Padding[0] = 0x80;

    Tail = (State->BlockLength < 56) ? (56 - State->BlockLength)
                                     : (120 - State->BlockLength);
    RtlpSha256Update(State, Padding, Tail);

    for (i = 0; i < 8; i++)
        Padding[i] = (UCHAR)(BitCount >> (56 - i * 8));
    RtlpSha256Update(State, Padding, 8);

    for (i = 0; i < 8; i++)
    {
        Digest[i * 4 + 0] = (UCHAR)(State->H[i] >> 24);
        Digest[i * 4 + 1] = (UCHAR)(State->H[i] >> 16);
        Digest[i * 4 + 2] = (UCHAR)(State->H[i] >> 8);
        Digest[i * 4 + 3] = (UCHAR)(State->H[i]);
    }
}

static
VOID
RtlpSha256Hash(
    _In_reads_(Length) PCUCHAR Data,
    _In_ SIZE_T Length,
    _Out_writes_(32) PUCHAR Digest)
{
    RTL_SHA256_STATE State;

    RtlpSha256Initialize(&State);
    RtlpSha256Update(&State, Data, Length);
    RtlpSha256Finalize(&State, Digest);
}

static const struct
{
    PCWSTR Name;
    ULONG Rid;
} RtlpLegacyCapabilities[] =
{
    { L"INTERNETCLIENT", SECURITY_CAPABILITY_INTERNET_CLIENT },
    { L"INTERNETCLIENTSERVER", SECURITY_CAPABILITY_INTERNET_CLIENT_SERVER },
    { L"PRIVATENETWORKCLIENTSERVER", SECURITY_CAPABILITY_PRIVATE_NETWORK_CLIENT_SERVER },
    { L"PICTURESLIBRARY", SECURITY_CAPABILITY_PICTURES_LIBRARY },
    { L"VIDEOSLIBRARY", SECURITY_CAPABILITY_VIDEOS_LIBRARY },
    { L"MUSICLIBRARY", SECURITY_CAPABILITY_MUSIC_LIBRARY },
    { L"DOCUMENTSLIBRARY", SECURITY_CAPABILITY_DOCUMENTS_LIBRARY },
    { L"ENTERPRISEAUTHENTICATION", SECURITY_CAPABILITY_ENTERPRISE_AUTHENTICATION },
    { L"SHAREDUSERCERTIFICATES", SECURITY_CAPABILITY_SHARED_USER_CERTIFICATES },
    { L"REMOVABLESTORAGE", SECURITY_CAPABILITY_REMOVABLE_STORAGE },
    { L"APPOINTMENTS", SECURITY_CAPABILITY_APPOINTMENTS },
    { L"CONTACTS", SECURITY_CAPABILITY_CONTACTS }
};

NTSTATUS
NTAPI
RtlDeriveCapabilitySidsFromName(
    _Inout_ PUNICODE_STRING UnicodeString,
    _Out_ PSID CapabilityGroupSid,
    _Out_ PSID CapabilitySid)
{
    UNICODE_STRING UpperName;
    UCHAR Digest[32];
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    SID_IDENTIFIER_AUTHORITY AppPackageAuthority = SECURITY_APP_PACKAGE_AUTHORITY;
    ULONG HashRids[8];
    NTSTATUS Status;
    ULONG i;

    PAGED_CODE();

    if (UnicodeString == NULL || UnicodeString->Buffer == NULL ||
        CapabilityGroupSid == NULL || CapabilitySid == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = RtlUpcaseUnicodeString(&UpperName, UnicodeString, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlpSha256Hash((PCUCHAR)UpperName.Buffer, UpperName.Length, Digest);
    for (i = 0; i < 8; i++)
        HashRids[i] = ((PULONG)Digest)[i];

    RtlInitializeSid(CapabilityGroupSid, &NtAuthority, SECURITY_INSTALLER_GROUP_CAPABILITY_RID_COUNT);
    *RtlSubAuthoritySid(CapabilityGroupSid, 0) = SECURITY_INSTALLER_GROUP_CAPABILITY_BASE;
    for (i = 0; i < 8; i++)
        *RtlSubAuthoritySid(CapabilityGroupSid, 1 + i) = HashRids[i];

    for (i = 0; i < RTL_NUMBER_OF(RtlpLegacyCapabilities); i++)
    {
        UNICODE_STRING LegacyName;

        RtlInitUnicodeString(&LegacyName, RtlpLegacyCapabilities[i].Name);
        if (RtlEqualUnicodeString(&UpperName, &LegacyName, FALSE))
        {
            RtlInitializeSid(CapabilitySid, &AppPackageAuthority, SECURITY_BUILTIN_CAPABILITY_RID_COUNT);
            *RtlSubAuthoritySid(CapabilitySid, 0) = SECURITY_CAPABILITY_BASE_RID;
            *RtlSubAuthoritySid(CapabilitySid, 1) = RtlpLegacyCapabilities[i].Rid;
            RtlFreeUnicodeString(&UpperName);
            return STATUS_SUCCESS;
        }
    }

    RtlInitializeSid(CapabilitySid, &AppPackageAuthority, SECURITY_INSTALLER_CAPABILITY_RID_COUNT);
    *RtlSubAuthoritySid(CapabilitySid, 0) = SECURITY_CAPABILITY_BASE_RID;

    if (UpperName.Length >= 14 * sizeof(WCHAR) &&
        RtlCompareMemory(UpperName.Buffer, L"ISOLATEDWIN32-", 14 * sizeof(WCHAR)) == 14 * sizeof(WCHAR))
    {
        *RtlSubAuthoritySid(CapabilitySid, 1) = SECURITY_CAPABILITY_APP_SILO_RID;
    }
    else
    {
        *RtlSubAuthoritySid(CapabilitySid, 1) = SECURITY_CAPABILITY_APP_RID;
    }
    for (i = 0; i < 8; i++)
        *RtlSubAuthoritySid(CapabilitySid, 2 + i) = HashRids[i];

    RtlFreeUnicodeString(&UpperName);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlCheckTokenCapability(
    _In_opt_ HANDLE TokenHandle,
    _In_ PSID CapabilitySidToCheck,
    _Out_ PBOOLEAN HasCapability)
{
    HANDLE Handle = NULL;
    BOOLEAN CloseHandle = FALSE;
    ULONG IsAppContainer = 0;
    ULONG ReturnLength;
    PTOKEN_GROUPS Groups = NULL;
    ULONG GroupsLength = 0;
    BOOLEAN Found = FALSE;
    NTSTATUS Status;
    ULONG i;

    PAGED_CODE();

    if (CapabilitySidToCheck == NULL || HasCapability == NULL)
        return STATUS_INVALID_PARAMETER;

    if (TokenHandle == NULL || TokenHandle == RTL_NT_CURRENT_PROCESS_TOKEN)
    {
        Status = ZwOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &Handle);
        CloseHandle = TRUE;
    }
    else if (TokenHandle == RTL_NT_CURRENT_THREAD_TOKEN)
    {
        Status = ZwOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, FALSE, &Handle);
        CloseHandle = TRUE;
    }
    else if (TokenHandle == RTL_NT_CURRENT_THREAD_EFFECTIVE_TOKEN)
    {
        Status = ZwOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, FALSE, &Handle);
        if (!NT_SUCCESS(Status))
            Status = ZwOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &Handle);
        CloseHandle = TRUE;
    }
    else
    {
        Handle = TokenHandle;
        Status = STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ZwQueryInformationToken(Handle,
                                     TokenIsAppContainer,
                                     &IsAppContainer,
                                     sizeof(IsAppContainer),
                                     &ReturnLength);
    if (!NT_SUCCESS(Status))
        goto Quit;

    if (!IsAppContainer)
    {
        *HasCapability = TRUE;
        Status = STATUS_SUCCESS;
        goto Quit;
    }

    Status = ZwQueryInformationToken(Handle, TokenCapabilities, NULL, 0, &GroupsLength);
    if (Status != STATUS_BUFFER_TOO_SMALL)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto Quit;
    }

    Groups = ExAllocatePoolWithTag(PagedPool, GroupsLength, RTL_CAPABILITY_TAG);
    if (Groups == NULL)
    {
        Status = STATUS_NO_MEMORY;
        goto Quit;
    }

    Status = ZwQueryInformationToken(Handle, TokenCapabilities, Groups, GroupsLength, &ReturnLength);
    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < Groups->GroupCount; i++)
        {
            if ((Groups->Groups[i].Attributes & SE_GROUP_ENABLED) &&
                RtlEqualSid(Groups->Groups[i].Sid, CapabilitySidToCheck))
            {
                Found = TRUE;
                break;
            }
        }
        *HasCapability = Found;
    }

Quit:
    if (Groups != NULL)
        ExFreePoolWithTag(Groups, RTL_CAPABILITY_TAG);
    if (CloseHandle)
        ZwClose(Handle);
    return Status;
}

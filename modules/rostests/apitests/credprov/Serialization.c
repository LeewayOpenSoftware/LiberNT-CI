/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the packed logon serialization library against malformed input
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define WIN32_NO_STATUS
#include <apitest.h>
#include <ntstatus.h>
#include <ntsecapi.h>
#include <security.h>
#include <credser.h>

typedef struct _GUARDED
{
    PUCHAR Region;
    SIZE_T RegionSize;
    PUCHAR Data;
} GUARDED;

static BOOL
GuardedPlace(_Out_ GUARDED *Guarded, _In_reads_bytes_(Size) const VOID *Source, _In_ ULONG Size)
{
    SYSTEM_INFO Info;
    SIZE_T Pages;
    DWORD Old;

    GetSystemInfo(&Info);
    Pages = (Size + Info.dwPageSize - 1) / Info.dwPageSize;
    Guarded->RegionSize = (Pages + 1) * Info.dwPageSize;
    Guarded->Region = VirtualAlloc(NULL, Guarded->RegionSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!Guarded->Region)
        return FALSE;
    if (!VirtualProtect(Guarded->Region + Pages * Info.dwPageSize, Info.dwPageSize, PAGE_NOACCESS, &Old))
    {
        VirtualFree(Guarded->Region, 0, MEM_RELEASE);
        return FALSE;
    }
    Guarded->Data = Guarded->Region + Pages * Info.dwPageSize - Size;
    if (Size)
        CopyMemory(Guarded->Data, Source, Size);
    return TRUE;
}

static VOID
GuardedFree(_In_ GUARDED *Guarded)
{
    VirtualFree(Guarded->Region, 0, MEM_RELEASE);
}

static NTSTATUS
UnpackLogonGuarded(_In_reads_bytes_(Size) const VOID *Blob, _In_ ULONG Size, _Out_ PCREDSER_LOGON Logon)
{
    GUARDED Guarded;
    NTSTATUS Status;

    if (!GuardedPlace(&Guarded, Blob, Size))
        return STATUS_NO_MEMORY;
    Status = CredSerUnpackLogon(Size ? Guarded.Data : NULL, Size, Logon);
    GuardedFree(&Guarded);
    return Status;
}

static NTSTATUS
UnpackChangeGuarded(_In_reads_bytes_(Size) const VOID *Blob, _In_ ULONG Size)
{
    CREDSER_CHANGE_PASSWORD Change;
    GUARDED Guarded;
    NTSTATUS Status;

    if (!GuardedPlace(&Guarded, Blob, Size))
        return STATUS_NO_MEMORY;
    Status = CredSerUnpackChangePassword(Size ? Guarded.Data : NULL, Size, &Change);
    GuardedFree(&Guarded);
    return Status;
}

static BOOL
StringIs(_In_ const UNICODE_STRING *String, _In_ PCWSTR Expected)
{
    SIZE_T Bytes = wcslen(Expected) * sizeof(WCHAR);
    return String->Length == Bytes && (!Bytes || !memcmp(String->Buffer, Expected, Bytes));
}

static ULONG
PackLogon(_In_ KERB_LOGON_SUBMIT_TYPE Type, _In_ PCWSTR Domain, _In_ PCWSTR User, _In_ PCWSTR Password,
          _Out_writes_bytes_(Capacity) PUCHAR Buffer, _In_ ULONG Capacity)
{
    LUID Id = { 0x1234, 0x56 };
    ULONG Size = Capacity;
    NTSTATUS Status;

    Status = CredSerPackLogon(Type, Domain, User, Password, &Id, Buffer, &Size);
    ok_ntstatus(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status) ? Size : 0;
}

static VOID
TestPlainLogonHeader(void)
{
    UCHAR Blob[256];
    PKERB_INTERACTIVE_LOGON Plain = (PKERB_INTERACTIVE_LOGON)Blob;
    CREDSER_LOGON Logon;
    ULONG Offset = sizeof(KERB_INTERACTIVE_LOGON);
    GUARDED Guarded;
    static const WCHAR User[] = L"someuser", Password[] = L"password";

    ZeroMemory(Blob, sizeof(Blob));
    Plain->MessageType = KerbInteractiveLogon;
    Plain->UserName.Length = Plain->UserName.MaximumLength = sizeof(User) - sizeof(WCHAR);
    Plain->UserName.Buffer = (PWSTR)(ULONG_PTR)Offset;
    CopyMemory(Blob + Offset, User, Plain->UserName.Length);
    Offset += Plain->UserName.Length;
    Plain->Password.Length = Plain->Password.MaximumLength = sizeof(Password) - sizeof(WCHAR);
    Plain->Password.Buffer = (PWSTR)(ULONG_PTR)Offset;
    CopyMemory(Blob + Offset, Password, Plain->Password.Length);
    Offset += Plain->Password.Length;
    ok(Offset >= sizeof(KERB_INTERACTIVE_UNLOCK_LOGON), "the plain blob must be at least as large as the unlock header\n");

    if (GuardedPlace(&Guarded, Blob, Offset))
    {
        ok_ntstatus(CredSerUnpackLogon(Guarded.Data, Offset, &Logon), STATUS_SUCCESS);
        ok(StringIs(&Logon.UserName, L"someuser") && StringIs(&Logon.Password, L"password"),
           "plain type 2 strings mismatch\n");
        GuardedFree(&Guarded);
    }

    Plain->MessageType = KerbWorkstationUnlockLogon;
    ok(UnpackLogonGuarded(Blob, Offset, &Logon) == STATUS_INVALID_PARAMETER,
       "a type 7 blob with strings inside the LogonId was accepted\n");
}

static VOID
TestRoundTrip(void)
{
    UCHAR Buffer[1024];
    CREDSER_LOGON Logon;
    CREDSER_CHANGE_PASSWORD Change;
    PMSV1_0_INTERACTIVE_LOGON Msv;
    GUARDED Guarded;
    ULONG Size, MsvSize;
    NTSTATUS Status;

    Size = 0;
    ok_ntstatus(CredSerPackLogon(KerbInteractiveLogon, L"DOM", L"user", L"pw", NULL, NULL, &Size), STATUS_BUFFER_TOO_SMALL);
    ok_long(Size, sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) + (4 + 5 + 3) * sizeof(WCHAR));

    Size = PackLogon(KerbInteractiveLogon, L"DOM", L"user", L"pw", Buffer, sizeof(Buffer));
    if (GuardedPlace(&Guarded, Buffer, Size))
    {
        ok_ntstatus(CredSerUnpackLogon(Guarded.Data, Size, &Logon), STATUS_SUCCESS);
        ok_long(Logon.MessageType, KerbInteractiveLogon);
        ok(StringIs(&Logon.LogonDomainName, L"DOM") && StringIs(&Logon.UserName, L"user") &&
           StringIs(&Logon.Password, L"pw"), "logon strings mismatch\n");
        ok(Logon.LogonId.LowPart == 0 && Logon.LogonId.HighPart == 0, "type 2 must not carry a LogonId\n");
        GuardedFree(&Guarded);
    }

    Size = PackLogon(KerbWorkstationUnlockLogon, NULL, L"user", L"", Buffer, sizeof(Buffer));
    if (GuardedPlace(&Guarded, Buffer, Size))
    {
        ok_ntstatus(CredSerUnpackLogon(Guarded.Data, Size, &Logon), STATUS_SUCCESS);
        ok_long(Logon.MessageType, KerbWorkstationUnlockLogon);
        ok(Logon.LogonId.LowPart == 0x1234 && Logon.LogonId.HighPart == 0x56, "type 7 lost its LogonId\n");
        ok(StringIs(&Logon.UserName, L"user") && Logon.LogonDomainName.Length == 0 && Logon.Password.Length == 0,
           "unlock strings mismatch\n");
        GuardedFree(&Guarded);
    }

    Size = PackLogon(KerbInteractiveLogon, L"DOM", L"user", L"secret", Buffer, sizeof(Buffer));
    if (GuardedPlace(&Guarded, Buffer, Size))
    {
        ok_ntstatus(CredSerUnpackLogon(Guarded.Data, Size, &Logon), STATUS_SUCCESS);
        Status = CredSerBuildMsv1_0Logon(&Logon, &Msv, &MsvSize);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok_long(Msv->MessageType, MsV1_0InteractiveLogon);
            ok(StringIs(&Msv->LogonDomainName, L"DOM") && StringIs(&Msv->UserName, L"user") &&
               StringIs(&Msv->Password, L"secret"), "MSV1_0 strings mismatch\n");
            ok((PUCHAR)Msv->UserName.Buffer > (PUCHAR)Msv && (PUCHAR)Msv->Password.Buffer < (PUCHAR)Msv + MsvSize,
               "MSV1_0 strings must point into the buffer\n");
            CredSerFree(Msv, MsvSize);
        }
        GuardedFree(&Guarded);
    }

    Size = sizeof(Buffer);
    ok_ntstatus(CredSerPackChangePassword(L"DOM", L"user", L"old", L"new", Buffer, &Size), STATUS_SUCCESS);
    if (GuardedPlace(&Guarded, Buffer, Size))
    {
        ok_ntstatus(CredSerUnpackChangePassword(Guarded.Data, Size, &Change), STATUS_SUCCESS);
        ok(StringIs(&Change.AccountName, L"user") && StringIs(&Change.OldPassword, L"old") &&
           StringIs(&Change.NewPassword, L"new"), "change password strings mismatch\n");
        ok_long(((PKERB_CHANGEPASSWORD_REQUEST)Guarded.Data)->MessageType, KerbChangePasswordMessage);
        GuardedFree(&Guarded);
    }

    TestPlainLogonHeader();
}

static VOID
TestPackages(void)
{
    CREDSER_PACKAGES Packages;
    HANDLE Lsa;
    NTSTATUS Status;

    Status = LsaConnectUntrusted(&Lsa);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    ok_ntstatus(CredSerLookupPackages(Lsa, &Packages), STATUS_SUCCESS);
    ok(Packages.HaveMsv1_0, "MSV1_0 must be found\n");
    ok_long(CredSerPreferredPackage(&Packages), Packages.HaveNegotiate ? Packages.Negotiate : Packages.Msv1_0);
    ok(CredSerIsKnownPackage(&Packages, Packages.Msv1_0), "MSV1_0 must be accepted\n");
    ok(!CredSerIsKnownPackage(&Packages, 0x7777), "a made-up package id must be rejected\n");
    LsaDeregisterLogonProcess(Lsa);
}

static VOID
ExpectBadLogon(_In_reads_bytes_(Size) const VOID *Blob, _In_ ULONG Size, _In_ PCSTR What)
{
    CREDSER_LOGON Logon;
    ok(UnpackLogonGuarded(Blob, Size, &Logon) == STATUS_INVALID_PARAMETER, "%s was accepted\n", What);
}

static VOID
TestMalformedLogon(void)
{
    UCHAR Valid[1024], Blob[CREDSER_MAX_BLOB_SIZE + 64];
    PKERB_INTERACTIVE_UNLOCK_LOGON Packed = (PKERB_INTERACTIVE_UNLOCK_LOGON)Blob;
    CREDSER_LOGON Logon;
    ULONG Size, Cut, Header = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON);
    WCHAR Long[CREDSER_MAX_NAME_CHARS + 2];
    ULONG i;

    Size = PackLogon(KerbInteractiveLogon, L"DOM", L"user", L"pw", Valid, sizeof(Valid));
#define RESET() CopyMemory(Blob, Valid, Size)

    ok_ntstatus(CredSerUnpackLogon(NULL, 16, &Logon), STATUS_INVALID_PARAMETER);
    ExpectBadLogon(Valid, 0, "an empty blob");
    ExpectBadLogon(Valid, sizeof(KERB_INTERACTIVE_LOGON) - 1, "a short header");

    RESET(); Packed->Logon.MessageType = 0; ExpectBadLogon(Blob, Size, "message type 0");
    RESET(); Packed->Logon.MessageType = 3; ExpectBadLogon(Blob, Size, "message type 3");
    RESET(); Packed->Logon.MessageType = 99; ExpectBadLogon(Blob, Size, "message type 99");

    RESET(); Packed->Logon.UserName.Length = Packed->Logon.UserName.MaximumLength + 2;
    ExpectBadLogon(Blob, Size, "Length > MaximumLength");
    RESET(); Packed->Logon.UserName.Length -= 1; ExpectBadLogon(Blob, Size, "an odd Length");
    RESET(); Packed->Logon.UserName.MaximumLength += 1; ExpectBadLogon(Blob, Size, "an odd MaximumLength");

    RESET(); Packed->Logon.Password.MaximumLength += 2; ExpectBadLogon(Blob, Size, "a string past the end");
    RESET(); Packed->Logon.UserName.Buffer = (PWSTR)(ULONG_PTR)0xFFFFFFF0; ExpectBadLogon(Blob, Size, "a wrapping offset");
    RESET(); Packed->Logon.UserName.Buffer = (PWSTR)(ULONG_PTR)8; ExpectBadLogon(Blob, Size, "an offset inside the header");

    RESET(); Packed->Logon.Password.Buffer = Packed->Logon.UserName.Buffer;
    Packed->Logon.Password.Length = Packed->Logon.Password.MaximumLength = 4;
    ExpectBadLogon(Blob, Size, "overlapping strings");
    RESET(); Packed->Logon.UserName.Length = Packed->Logon.UserName.MaximumLength = 0;
    Packed->Logon.UserName.Buffer = NULL;
    ExpectBadLogon(Blob, Size, "an empty user name");
    RESET(); Packed->Logon.LogonDomainName.Length = Packed->Logon.LogonDomainName.MaximumLength = 0;
    Packed->Logon.LogonDomainName.Buffer = NULL;
    ok_ntstatus(UnpackLogonGuarded(Blob, Size, &Logon), STATUS_SUCCESS);

    RESET(); Packed->Logon.UserName.Buffer = (PWSTR)((PUCHAR)Blob + (ULONG_PTR)Packed->Logon.UserName.Buffer);
    ExpectBadLogon(Blob, Size, "an absolute pointer");

    for (Cut = 0; Cut < Size; Cut++)
        ExpectBadLogon(Valid, Cut, "a truncated blob");

    RESET(); Packed->Logon.UserName.Buffer = (PWSTR)((ULONG_PTR)Packed->Logon.UserName.Buffer + 1);
    ExpectBadLogon(Blob, Size, "an unaligned offset");

    RESET(); ((PWCHAR)(Blob + (ULONG_PTR)Packed->Logon.UserName.Buffer))[1] = 0;
    ExpectBadLogon(Blob, Size, "a NUL inside the user name");
    RESET(); ((PWCHAR)(Blob + (ULONG_PTR)Packed->Logon.LogonDomainName.Buffer))[1] = 0;
    ExpectBadLogon(Blob, Size, "a NUL inside the domain");
    RESET(); ((PWCHAR)(Blob + (ULONG_PTR)Packed->Logon.Password.Buffer))[0] = 0;
    ok_ntstatus(UnpackLogonGuarded(Blob, Size, &Logon), STATUS_SUCCESS);

    for (i = 0; i < ARRAYSIZE(Long) - 1; i++)
        Long[i] = L'a';
    Long[CREDSER_MAX_NAME_CHARS] = 0;
    Size = PackLogon(KerbInteractiveLogon, Long, Long, Long, Blob, sizeof(Blob));
    ok(Size != 0, "256 characters per field must pack\n");
    ok_ntstatus(UnpackLogonGuarded(Blob, Size, &Logon), STATUS_SUCCESS);
    Long[CREDSER_MAX_NAME_CHARS] = L'a';
    Long[CREDSER_MAX_NAME_CHARS + 1] = 0;
    Size = sizeof(Blob);
    ok_ntstatus(CredSerPackLogon(KerbInteractiveLogon, NULL, Long, L"pw", NULL, Blob, &Size), STATUS_INVALID_PARAMETER);

    ZeroMemory(Blob, sizeof(Blob));
    Packed->Logon.MessageType = KerbInteractiveLogon;
    for (i = 0; i < 3; i++)
    {
        PUNICODE_STRING Field = i == 0 ? &Packed->Logon.LogonDomainName :
                                i == 1 ? &Packed->Logon.UserName : &Packed->Logon.Password;
        Field->Length = Field->MaximumLength = (CREDSER_MAX_NAME_CHARS + 1) * sizeof(WCHAR);
        Field->Buffer = (PWSTR)(ULONG_PTR)(Header + i * Field->MaximumLength);
    }
    for (i = Header; i < Header + 3 * (CREDSER_MAX_NAME_CHARS + 1) * sizeof(WCHAR); i += 2)
        *(PWCHAR)(Blob + i) = L'a';
    ExpectBadLogon(Blob, Header + 3 * (CREDSER_MAX_NAME_CHARS + 1) * sizeof(WCHAR), "257-character fields");

    ok_ntstatus(CredSerUnpackLogon(Valid, CREDSER_MAX_BLOB_SIZE + 1, &Logon), STATUS_INVALID_PARAMETER);
#undef RESET
}

static VOID
TestMalformedChangePassword(void)
{
    UCHAR Valid[1024], Blob[1024];
    PKERB_CHANGEPASSWORD_REQUEST Packed = (PKERB_CHANGEPASSWORD_REQUEST)Blob;
    ULONG Size = sizeof(Valid), Cut;

    ok_ntstatus(CredSerPackChangePassword(L"DOM", L"user", L"old", L"new", Valid, &Size), STATUS_SUCCESS);
#define RESET() CopyMemory(Blob, Valid, Size)
    ok_ntstatus(UnpackChangeGuarded(Valid, Size), STATUS_SUCCESS);
    for (Cut = 0; Cut < Size; Cut++)
        ok(UnpackChangeGuarded(Valid, Cut) == STATUS_INVALID_PARAMETER, "truncation to %lu was accepted\n", Cut);

    RESET(); Packed->MessageType = KerbChangeMachinePasswordMessage;
    ok_ntstatus(UnpackChangeGuarded(Blob, Size), STATUS_INVALID_PARAMETER);
    RESET(); Packed->Impersonating = TRUE;
    ok_ntstatus(UnpackChangeGuarded(Blob, Size), STATUS_INVALID_PARAMETER);
    RESET(); Packed->NewPassword.MaximumLength += 2;
    ok_ntstatus(UnpackChangeGuarded(Blob, Size), STATUS_INVALID_PARAMETER);
    RESET(); Packed->OldPassword.Length -= 1;
    ok_ntstatus(UnpackChangeGuarded(Blob, Size), STATUS_INVALID_PARAMETER);
    RESET(); Packed->AccountName.Buffer = (PWSTR)((ULONG_PTR)Packed->AccountName.Buffer + 1);
    ok_ntstatus(UnpackChangeGuarded(Blob, Size), STATUS_INVALID_PARAMETER);
    RESET(); Packed->NewPassword.Buffer = Packed->OldPassword.Buffer;
    ok_ntstatus(UnpackChangeGuarded(Blob, Size), STATUS_INVALID_PARAMETER);
    RESET(); ((PWCHAR)(Blob + (ULONG_PTR)Packed->AccountName.Buffer))[0] = 0;
    ok_ntstatus(UnpackChangeGuarded(Blob, Size), STATUS_INVALID_PARAMETER);
    RESET(); Packed->DomainName.Buffer = (PWSTR)(ULONG_PTR)0xFFFFFFF0;
    ok_ntstatus(UnpackChangeGuarded(Blob, Size), STATUS_INVALID_PARAMETER);
#undef RESET
}

static ULONG FuzzState = 0x1badb002;

static ULONG
FuzzNext(void)
{
    FuzzState = FuzzState * 1103515245 + 12345;
    return FuzzState >> 8;
}

static BOOL
Within(_In_ const UNICODE_STRING *String, _In_ const UCHAR *Base, _In_ ULONG Size)
{
    if (!String->Length)
        return TRUE;
    return (const UCHAR *)String->Buffer >= Base &&
           (const UCHAR *)String->Buffer + String->Length <= Base + Size &&
           !((ULONG_PTR)String->Buffer & 1) && !(String->Length & 1);
}

static VOID
TestFuzz(void)
{
    UCHAR Valid[512];
    CREDSER_LOGON Logon;
    GUARDED Guarded;
    ULONG Size, Round, Flips, Accepted = 0, Bad = 0, i;

    Size = PackLogon(KerbWorkstationUnlockLogon, L"DOMAIN", L"someuser", L"password", Valid, sizeof(Valid));
    if (!Size || !GuardedPlace(&Guarded, Valid, Size))
        return;
    for (Round = 0; Round < 10000; Round++)
    {
        CopyMemory(Guarded.Data, Valid, Size);
        Flips = 1 + FuzzNext() % 8;
        for (i = 0; i < Flips; i++)
            Guarded.Data[FuzzNext() % Size] = (UCHAR)FuzzNext();
        if (!NT_SUCCESS(CredSerUnpackLogon(Guarded.Data, Size, &Logon)))
            continue;
        Accepted++;
        if (!Within(&Logon.LogonDomainName, Guarded.Data, Size) || !Within(&Logon.UserName, Guarded.Data, Size) ||
            !Within(&Logon.Password, Guarded.Data, Size) || !Logon.UserName.Length ||
            Logon.UserName.Length > CREDSER_MAX_NAME_CHARS * sizeof(WCHAR) ||
            Logon.Password.Length > CREDSER_MAX_PASSWORD_CHARS * sizeof(WCHAR))
        {
            Bad++;
        }
        for (i = 0; i < Logon.UserName.Length / sizeof(WCHAR); i++)
            Bad += !Logon.UserName.Buffer[i];
    }
    GuardedFree(&Guarded);
    ok_long(Bad, 0);
    trace("fuzz: %lu of 10000 mutations accepted\n", Accepted);
}

static NTSTATUS
UnpackGenericGuarded(_In_reads_bytes_(Size) const VOID *Blob, _In_ ULONG Size, _Out_ PCREDSER_GENERIC Generic)
{
    GUARDED Guarded;
    NTSTATUS Status;

    if (!GuardedPlace(&Guarded, Blob, Size))
        return STATUS_NO_MEMORY;
    Status = CredSerUnpackGeneric(Size ? Guarded.Data : NULL, Size, Generic);
    GuardedFree(&Guarded);
    return Status;
}

typedef struct _GENERIC_LAYOUT
{
    ULONG Reserved;
    GUID Type;
    UNICODE_STRING UserName;
    UNICODE_STRING Password;
} GENERIC_LAYOUT;

static VOID
ExpectBadGeneric(_In_reads_bytes_(Size) const VOID *Blob, _In_ ULONG Size, _In_ PCSTR What)
{
    CREDSER_GENERIC Generic;
    ok(UnpackGenericGuarded(Blob, Size, &Generic) == STATUS_INVALID_PARAMETER, "%s was accepted\n", What);
}

static VOID
TestGeneric(void)
{
    UCHAR Valid[512], Blob[8192];
    GENERIC_LAYOUT *Packed = (GENERIC_LAYOUT *)Blob;
    CREDSER_GENERIC Generic;
    WCHAR Long[CREDSER_MAX_GENERIC_USER_CHARS + 2];
    ULONG Size, Cut, Round, Accepted = 0, Bad = 0, i;
    GUARDED Guarded;

    Size = sizeof(Valid);
    ok_ntstatus(CredSerPackGeneric(L"DOM\\user", L"pw", Valid, &Size), STATUS_SUCCESS);
    ok_long(Size, sizeof(GENERIC_LAYOUT) + 20);
    ok(CredSerIsGeneric(Valid, Size), "a packed generic buffer is not recognized\n");
    ok_ntstatus(UnpackGenericGuarded(Valid, Size, &Generic), STATUS_SUCCESS);
    ok_ntstatus(CredSerUnpackGeneric(Valid, Size, &Generic), STATUS_SUCCESS);
    ok(StringIs(&Generic.UserName, L"DOM\\user") && StringIs(&Generic.Password, L"pw"), "generic round trip mismatch\n");
#define RESET() CopyMemory(Blob, Valid, Size)

    ok(!CredSerIsGeneric(Valid, sizeof(GENERIC_LAYOUT) - 1), "a short generic header is recognized\n");
    RESET(); Packed->Reserved = 2; ExpectBadGeneric(Blob, Size, "a nonzero reserved field");
    RESET(); Packed->Type.Data1 ^= 1; ExpectBadGeneric(Blob, Size, "another type GUID");
    RESET(); Packed->UserName.Length = Packed->UserName.MaximumLength + 2;
    ExpectBadGeneric(Blob, Size, "Length > MaximumLength");
    RESET(); Packed->UserName.Length -= 1; Packed->UserName.MaximumLength -= 1;
    ExpectBadGeneric(Blob, Size, "an odd length");
    RESET(); Packed->Password.Length += 2; Packed->Password.MaximumLength += 2;
    ExpectBadGeneric(Blob, Size, "a string past the end");
    RESET(); Packed->UserName.Buffer = (PWSTR)(ULONG_PTR)0xFFF0; ExpectBadGeneric(Blob, Size, "an offset past the end");
    RESET(); Packed->UserName.Buffer = (PWSTR)(ULONG_PTR)0xFFFFFFF0; ExpectBadGeneric(Blob, Size, "a wrapping offset");
    RESET(); Packed->UserName.Buffer = (PWSTR)(ULONG_PTR)4; ExpectBadGeneric(Blob, Size, "an offset inside the header");
    RESET(); Packed->UserName.Buffer = (PWSTR)((ULONG_PTR)Packed->UserName.Buffer + 1);
    ExpectBadGeneric(Blob, Size, "an unaligned offset");
    RESET(); Packed->Password.Buffer = Packed->UserName.Buffer; ExpectBadGeneric(Blob, Size, "overlapping strings");
    RESET(); ((PWCHAR)(Blob + (ULONG_PTR)Packed->UserName.Buffer))[1] = 0;
    ExpectBadGeneric(Blob, Size, "a NUL inside the user name");
    RESET(); ((PWCHAR)(Blob + (ULONG_PTR)Packed->Password.Buffer))[0] = 0;
    ok_ntstatus(UnpackGenericGuarded(Blob, Size, &Generic), STATUS_SUCCESS);
    for (Cut = 0; Cut < Size; Cut++)
        ExpectBadGeneric(Valid, Cut, "a truncated generic buffer");
    ok_ntstatus(CredSerUnpackGeneric(Valid, CREDSER_MAX_BLOB_SIZE + 1, &Generic), STATUS_INVALID_PARAMETER);

    for (i = 0; i < ARRAYSIZE(Long) - 1; i++)
        Long[i] = L'a';
    Long[CREDSER_MAX_GENERIC_USER_CHARS] = 0;
    Size = sizeof(Blob);
    ok_ntstatus(CredSerPackGeneric(Long, L"pw", Blob, &Size), STATUS_SUCCESS);
    ok_ntstatus(UnpackGenericGuarded(Blob, Size, &Generic), STATUS_SUCCESS);
    Long[CREDSER_MAX_GENERIC_USER_CHARS] = L'a';
    Long[CREDSER_MAX_GENERIC_USER_CHARS + 1] = 0;
    Size = sizeof(Blob);
    ok_ntstatus(CredSerPackGeneric(Long, L"pw", Blob, &Size), STATUS_NOT_SUPPORTED);
#undef RESET

    Size = sizeof(Valid);
    CredSerPackGeneric(L"someuser", L"password", Valid, &Size);
    if (!GuardedPlace(&Guarded, Valid, Size))
        return;
    for (Round = 0; Round < 5000; Round++)
    {
        CopyMemory(Guarded.Data, Valid, Size);
        for (i = 1 + FuzzNext() % 8; i; i--)
            Guarded.Data[FuzzNext() % Size] = (UCHAR)FuzzNext();
        if (!NT_SUCCESS(CredSerUnpackGeneric(Guarded.Data, Size, &Generic)))
            continue;
        Accepted++;
        if (!Within(&Generic.UserName, Guarded.Data, Size) || !Within(&Generic.Password, Guarded.Data, Size))
            Bad++;
    }
    GuardedFree(&Guarded);
    ok_long(Bad, 0);
    trace("generic fuzz: %lu of 5000 mutations accepted\n", Accepted);
}

static VOID
TestCaptureAndFree(void)
{
    UCHAR Source[512];
    CREDSER_LOGON Logon;
    PVOID Copy = NULL;
    ULONG Size;

    Size = PackLogon(KerbInteractiveLogon, L"DOM", L"user", L"pw", Source, sizeof(Source));
    ok_ntstatus(CredSerCapture(Source, 0, &Copy), STATUS_INVALID_PARAMETER);
    ok_ntstatus(CredSerCapture(Source, CREDSER_MAX_BLOB_SIZE + 1, &Copy), STATUS_INVALID_PARAMETER);
    ok_ntstatus(CredSerCapture(Source, Size, &Copy), STATUS_SUCCESS);
    if (!Copy)
        return;

    FillMemory(Source, Size, 0xcc);
    ok_ntstatus(CredSerUnpackLogon(Copy, Size, &Logon), STATUS_SUCCESS);
    ok(StringIs(&Logon.UserName, L"user"), "the private copy changed with its source\n");

    CredSerFree(Copy, Size);
}

START_TEST(Serialization)
{
    TestRoundTrip();
    TestPackages();
    TestMalformedLogon();
    TestMalformedChangePassword();
    TestFuzz();
    TestGeneric();
    TestCaptureAndFree();
}

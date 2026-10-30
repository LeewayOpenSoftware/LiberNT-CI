/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Windows Lockdown Policy queries on a system without an application control policy
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#define WIN32_NO_STATUS
#include <apitest.h>
#define COBJMACROS
#include <objbase.h>
#include <shlwapi.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <wldp.h>

#define NT_HR(Status) ((HRESULT)((Status) | FACILITY_NT_BIT))

static const GUID HostOther = {0x626cbec3, 0xe1fa, 0x4227, {0x98, 0x00, 0xed, 0x21, 0x02, 0x74, 0xcf, 0x7c}};
static const GUID HostPowerShell = {0x8e9aaa7c, 0x198b, 0x4879, {0xae, 0x41, 0xa5, 0x0d, 0x47, 0xad, 0x64, 0x58}};
static const GUID ZeroGuid;
static const CLSID ClsidShellLink = {0x00021401, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

typedef struct _WLDP_UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} WLDP_UNICODE_STRING;

static HRESULT (WINAPI *pWldpQueryWindowsLockdownMode)(WLDP_WINDOWS_LOCKDOWN_MODE *);
static HRESULT (WINAPI *pWldpQueryWindowsLockdownRestriction)(WLDP_WINDOWS_LOCKDOWN_RESTRICTION *);
static HRESULT (WINAPI *pWldpSetWindowsLockdownRestriction)(WLDP_WINDOWS_LOCKDOWN_RESTRICTION);
static HRESULT (WINAPI *pWldpIsDynamicCodePolicyEnabled)(BOOL *);
static HRESULT (WINAPI *pWldpGetLockdownPolicy)(WLDP_HOST_INFORMATION *, DWORD *, DWORD);
static HRESULT (WINAPI *pWldpIsClassInApprovedList)(REFCLSID, WLDP_HOST_INFORMATION *, BOOL *, DWORD);
static HRESULT (WINAPI *pWldpQueryPolicySettingEnabled)(WLDP_POLICY_SETTING, BOOL *);
static HRESULT (WINAPI *pWldpQueryPolicySettingEnabled2)(const WCHAR *, BOOL *);
static HRESULT (WINAPI *pWldpIsProductionConfiguration)(BOOL *);
static HRESULT (WINAPI *pWldpIsWcosProductionConfiguration)(BOOL *);
static HRESULT (WINAPI *pWldpResetProductionConfiguration)(void);
static HRESULT (WINAPI *pWldpResetWcosProductionConfiguration)(void);
static HRESULT (WINAPI *pWldpCanExecuteFile)(REFGUID, WLDP_EXECUTION_EVALUATION_OPTIONS, HANDLE, const WCHAR *, WLDP_EXECUTION_POLICY *);
static HRESULT (WINAPI *pWldpCanExecuteBuffer)(REFGUID, WLDP_EXECUTION_EVALUATION_OPTIONS, const BYTE *, ULONG, const WCHAR *, WLDP_EXECUTION_POLICY *);
static HRESULT (WINAPI *pWldpCanExecuteStream)(REFGUID, WLDP_EXECUTION_EVALUATION_OPTIONS, IStream *, const WCHAR *, WLDP_EXECUTION_POLICY *);
static HRESULT (WINAPI *pWldpQueryDynamicCodeTrust)(HANDLE, void *, ULONG);
static HRESULT (WINAPI *pWldpSetDynamicCodeTrust)(HANDLE);
static HRESULT (WINAPI *pWldpQueryDeviceSecurityInformation)(WLDP_DEVICE_SECURITY_INFORMATION *, DWORD, DWORD *);
static HRESULT (WINAPI *pWldpIsAppApprovedByPolicy)(const WCHAR *, ULONGLONG);
static HRESULT (WINAPI *pWldpQuerySecurityPolicy)(const WLDP_UNICODE_STRING *, const WLDP_UNICODE_STRING *, const WLDP_UNICODE_STRING *,
                                                  WLDP_SECURE_SETTING_VALUE_TYPE *, void *, ULONG *);

static void
InitHost(WLDP_HOST_INFORMATION *Host, DWORD Revision, WLDP_HOST_ID Id, const WCHAR *Source, HANDLE SourceHandle)
{
    Host->dwRevision = Revision;
    Host->dwHostId = Id;
    Host->szSource = Source;
    Host->hSource = SourceHandle;
}

static void
InitString(WLDP_UNICODE_STRING *String, PWSTR Text)
{
    String->Buffer = Text;
    String->Length = String->MaximumLength = (USHORT)(lstrlenW(Text) * sizeof(WCHAR));
}

static void
TestLockdownState(HANDLE File, const WCHAR *Path)
{
    WLDP_WINDOWS_LOCKDOWN_MODE Mode = 0xdead;
    WLDP_WINDOWS_LOCKDOWN_RESTRICTION Restriction = 0xdead;
    WLDP_HOST_INFORMATION Host;
    DWORD State;
    BOOL Flag = 0xdead;

    ok_hr(pWldpQueryWindowsLockdownMode(&Mode), S_OK);
    ok_int(Mode, WLDP_WINDOWS_LOCKDOWN_MODE_UNLOCKED);

    ok_hr(pWldpQueryWindowsLockdownRestriction(&Restriction), S_OK);
    ok_int(Restriction, WLDP_WINDOWS_LOCKDOWN_RESTRICTION_NONE);
    ok_hr(pWldpQueryWindowsLockdownRestriction(NULL), E_INVALIDARG);
    ok_hr(pWldpSetWindowsLockdownRestriction(WLDP_WINDOWS_LOCKDOWN_RESTRICTION_NONE), S_OK);
    ok_hr(pWldpSetWindowsLockdownRestriction(WLDP_WINDOWS_LOCKDOWN_RESTRICTION_MAX), E_INVALIDARG);

    ok_hr(pWldpIsDynamicCodePolicyEnabled(&Flag), S_OK);
    ok_int(Flag, FALSE);

    State = 0xdead;
    ok_hr(pWldpGetLockdownPolicy(NULL, &State, 0), S_OK);
    ok(State & WLDP_LOCKDOWN_DEFINED_FLAG, "state %08lx\n", State);
    InitHost(&Host, WLDP_HOST_INFORMATION_REVISION, WLDP_HOST_ID_GLOBAL, NULL, NULL);
    State = 0xdead;
    ok_hr(pWldpGetLockdownPolicy(&Host, &State, 0), S_OK);
    ok(State & WLDP_LOCKDOWN_DEFINED_FLAG, "state %08lx\n", State);
    State = 0xdead;
    ok_hr(pWldpGetLockdownPolicy(&Host, &State, 1), S_OK);
    ok(State & WLDP_LOCKDOWN_DEFINED_FLAG, "state %08lx\n", State);
    InitHost(&Host, WLDP_HOST_INFORMATION_REVISION, WLDP_HOST_ID_POWERSHELL, Path, File);
    State = 0xdead;
    ok_hr(pWldpGetLockdownPolicy(&Host, &State, 0), S_OK);
    ok_hex(State, WLDP_LOCKDOWN_DEFINED_FLAG);
    InitHost(&Host, 2, WLDP_HOST_ID_GLOBAL, NULL, NULL);
    State = 0xdead;
    ok_hr(pWldpGetLockdownPolicy(&Host, &State, 0), E_INVALIDARG);
    ok_hex(State, 0xdead);
    InitHost(&Host, WLDP_HOST_INFORMATION_REVISION, WLDP_HOST_ID_MAX, NULL, NULL);
    ok_hr(pWldpGetLockdownPolicy(&Host, &State, 0), E_INVALIDARG);
    ok_hex(State, 0xdead);
    ok_hr(pWldpGetLockdownPolicy(NULL, NULL, 0), E_INVALIDARG);
}

static void
TestApprovedClass(void)
{
    WLDP_HOST_INFORMATION Host;
    BOOL Approved;

    InitHost(&Host, WLDP_HOST_INFORMATION_REVISION, WLDP_HOST_ID_POWERSHELL, NULL, NULL);
    Approved = 0xdead;
    ok_hr(pWldpIsClassInApprovedList(&ClsidShellLink, &Host, &Approved, 0), S_OK);
    ok_int(Approved, TRUE);
    Approved = 0xdead;
    ok_hr(pWldpIsClassInApprovedList(&ClsidShellLink, &Host, &Approved, 1), S_OK);
    ok_int(Approved, TRUE);
    InitHost(&Host, WLDP_HOST_INFORMATION_REVISION, WLDP_HOST_ID_GLOBAL, NULL, NULL);
    Approved = 0xdead;
    ok_hr(pWldpIsClassInApprovedList(&ClsidShellLink, &Host, &Approved, 0), E_INVALIDARG);
    ok_int(Approved, 0xdead);
    InitHost(&Host, 2, WLDP_HOST_ID_POWERSHELL, NULL, NULL);
    Approved = 0xdead;
    ok_hr(pWldpIsClassInApprovedList(&ClsidShellLink, &Host, &Approved, 0), E_INVALIDARG);
    ok_int(Approved, 0xdead);
}

static void
TestSettings(void)
{
    BOOL Flag;

    Flag = 0xdead;
    ok_hr(pWldpQueryPolicySettingEnabled(WLDP_POLICY_SETTING_AV_PERF_MODE, &Flag), S_OK);
    ok_int(Flag, FALSE);
    Flag = 0xdead;
    ok_hr(pWldpQueryPolicySettingEnabled(7, &Flag), NT_HR(STATUS_INVALID_PARAMETER));
    ok_int(Flag, FALSE);
    ok_hr(pWldpQueryPolicySettingEnabled(WLDP_POLICY_SETTING_AV_PERF_MODE, NULL), NT_HR(STATUS_INVALID_PARAMETER));

    Flag = 0xdead;
    ok_hr(pWldpQueryPolicySettingEnabled2(L"AvPerfMode", &Flag), S_OK);
    ok_int(Flag, FALSE);
    Flag = 0xdead;
    ok_hr(pWldpQueryPolicySettingEnabled2(L"NoSuchSetting", &Flag), S_OK);
    ok_int(Flag, FALSE);
    Flag = 0xdead;
    ok_hr(pWldpQueryPolicySettingEnabled2(NULL, &Flag), NT_HR(STATUS_INVALID_PARAMETER));
    ok_int(Flag, 0xdead);

    Flag = 0xdead;
    ok_hr(pWldpIsProductionConfiguration(&Flag), S_OK);
    ok_int(Flag, TRUE);
    Flag = 0xdead;
    ok_hr(pWldpIsWcosProductionConfiguration(&Flag), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    ok_int(Flag, FALSE);
    ok_hr(pWldpResetProductionConfiguration(), S_OK);
    ok_hr(pWldpResetWcosProductionConfiguration(), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
}

static void
TestCanExecute(HANDLE File)
{
    static const BYTE Script[16] = "Write-Host test";
    WLDP_EXECUTION_POLICY Result;
    IStream *Stream;

    Result = 0xdead;
    ok_hr(pWldpCanExecuteFile(&HostOther, WLDP_EXECUTION_EVALUATION_OPTION_NONE, File, NULL, &Result), S_OK);
    ok_int(Result, WLDP_EXECUTION_POLICY_ALLOWED);
    Result = 0xdead;
    ok_hr(pWldpCanExecuteFile(&HostPowerShell, WLDP_EXECUTION_EVALUATION_OPTION_EXECUTE_IN_INTERACTIVE_SESSION, File, L"audit", &Result), S_OK);
    ok_int(Result, WLDP_EXECUTION_POLICY_ALLOWED);
    Result = 0xdead;
    ok_hr(pWldpCanExecuteFile(&HostOther, 2, File, NULL, &Result), S_OK);
    ok_int(Result, WLDP_EXECUTION_POLICY_ALLOWED);
    Result = 0xdead;
    ok_hr(pWldpCanExecuteFile(&ZeroGuid, WLDP_EXECUTION_EVALUATION_OPTION_NONE, File, NULL, &Result), E_INVALIDARG);
    ok_int(Result, 0xdead);
    ok_hr(pWldpCanExecuteFile(&HostOther, WLDP_EXECUTION_EVALUATION_OPTION_NONE, NULL, NULL, &Result), E_INVALIDARG);
    ok_hr(pWldpCanExecuteFile(&HostOther, WLDP_EXECUTION_EVALUATION_OPTION_NONE, INVALID_HANDLE_VALUE, NULL, &Result), E_INVALIDARG);
    ok_int(Result, 0xdead);

    Result = 0xdead;
    ok_hr(pWldpCanExecuteBuffer(&HostPowerShell, WLDP_EXECUTION_EVALUATION_OPTION_NONE, Script, sizeof(Script), NULL, &Result), S_OK);
    ok_int(Result, WLDP_EXECUTION_POLICY_ALLOWED);
    Result = 0xdead;
    ok_hr(pWldpCanExecuteBuffer(&HostPowerShell, WLDP_EXECUTION_EVALUATION_OPTION_NONE, Script, 0, NULL, &Result), E_INVALIDARG);
    ok_hr(pWldpCanExecuteBuffer(&HostPowerShell, WLDP_EXECUTION_EVALUATION_OPTION_NONE, NULL, sizeof(Script), NULL, &Result), E_INVALIDARG);
    ok_int(Result, 0xdead);

    Stream = SHCreateMemStream(Script, sizeof(Script));
    ok(Stream != NULL, "SHCreateMemStream failed\n");
    if (Stream)
    {
        Result = 0xdead;
        ok_hr(pWldpCanExecuteStream(&HostPowerShell, WLDP_EXECUTION_EVALUATION_OPTION_NONE, Stream, NULL, &Result), S_OK);
        ok_int(Result, WLDP_EXECUTION_POLICY_ALLOWED);
        IStream_Release(Stream);
    }
    Result = 0xdead;
    ok_hr(pWldpCanExecuteStream(&HostPowerShell, WLDP_EXECUTION_EVALUATION_OPTION_NONE, NULL, NULL, &Result), E_INVALIDARG);
    ok_int(Result, 0xdead);
}

static void
TestDynamicCodeAndDevice(HANDLE File)
{
    BYTE Image[16] = {0};
    BYTE Buffer[256];
    WLDP_UNICODE_STRING Provider, Key, Value;
    WLDP_SECURE_SETTING_VALUE_TYPE Type;
    DWORD Length;
    ULONG Size;

    ok_hr(pWldpQueryDynamicCodeTrust(File, NULL, 0), S_OK);
    ok_hr(pWldpQueryDynamicCodeTrust(NULL, Image, sizeof(Image)), S_OK);
    ok_hr(pWldpQueryDynamicCodeTrust(NULL, NULL, 0), E_INVALIDARG);
    ok_hr(pWldpSetDynamicCodeTrust(File), NT_HR(STATUS_ACCESS_DENIED));
    ok_hr(pWldpSetDynamicCodeTrust(NULL), NT_HR(STATUS_INVALID_HANDLE));

    Length = 0xdead;
    ok_hr(pWldpQueryDeviceSecurityInformation(NULL, 0, &Length), S_OK);
    ok_hex(Length, 0xdead);
    memset(Buffer, 0x55, sizeof(Buffer));
    ok_hr(pWldpQueryDeviceSecurityInformation((WLDP_DEVICE_SECURITY_INFORMATION *)Buffer, sizeof(Buffer), &Length), S_OK);
    ok_hex(Length, 0xdead);
    ok_hex(Buffer[0], 0x55);

    ok_hr(pWldpIsAppApprovedByPolicy(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe", 0), NT_HR(STATUS_SUCCESS));
    ok_hr(pWldpIsAppApprovedByPolicy(L"", 0), NT_HR(STATUS_SUCCESS));
    ok_hr(pWldpIsAppApprovedByPolicy(NULL, 0), NT_HR(STATUS_SUCCESS));

    InitString(&Provider, L"WldpApiTest");
    InitString(&Key, L"Key");
    InitString(&Value, L"Value");
    Type = 0xdead;
    Size = sizeof(Buffer);
    ok_hr(pWldpQuerySecurityPolicy(&Provider, &Key, &Value, &Type, Buffer, &Size), NT_HR(STATUS_NOT_FOUND));
    ok_int(Type, 0xdead);
    ok_int(Size, sizeof(Buffer));
    Size = 0;
    ok_hr(pWldpQuerySecurityPolicy(&Provider, &Key, &Value, &Type, NULL, &Size), NT_HR(STATUS_INVALID_PARAMETER));
    ok_int(Type, 0xdead);
    ok_int(Size, 0);
}

START_TEST(WldpPolicy)
{
    HMODULE Wldp = LoadLibraryW(L"wldp.dll");
    WCHAR Path[MAX_PATH];
    HANDLE File;

    ok(Wldp != NULL, "wldp.dll not loaded: %lu\n", GetLastError());
    if (!Wldp)
        return;

#define LOAD(Name) p##Name = (void *)GetProcAddress(Wldp, #Name); ok(p##Name != NULL, #Name " missing\n"); if (!p##Name) return
    LOAD(WldpQueryWindowsLockdownMode);
    LOAD(WldpQueryWindowsLockdownRestriction);
    LOAD(WldpSetWindowsLockdownRestriction);
    LOAD(WldpIsDynamicCodePolicyEnabled);
    LOAD(WldpGetLockdownPolicy);
    LOAD(WldpIsClassInApprovedList);
    LOAD(WldpQueryPolicySettingEnabled);
    LOAD(WldpQueryPolicySettingEnabled2);
    LOAD(WldpIsProductionConfiguration);
    LOAD(WldpIsWcosProductionConfiguration);
    LOAD(WldpResetProductionConfiguration);
    LOAD(WldpResetWcosProductionConfiguration);
    LOAD(WldpCanExecuteFile);
    LOAD(WldpCanExecuteBuffer);
    LOAD(WldpCanExecuteStream);
    LOAD(WldpQueryDynamicCodeTrust);
    LOAD(WldpSetDynamicCodeTrust);
    LOAD(WldpQueryDeviceSecurityInformation);
    LOAD(WldpIsAppApprovedByPolicy);
    LOAD(WldpQuerySecurityPolicy);
#undef LOAD

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "cannot open %ls: %lu\n", Path, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;

    TestLockdownState(File, Path);
    TestApprovedClass();
    TestSettings();
    TestCanExecute(File);
    TestDynamicCodeAndDevice(File);

    CloseHandle(File);
    CoUninitialize();
}

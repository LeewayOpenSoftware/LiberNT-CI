/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     What an application manifest declares reaches the code that consumes it:
 *              supportedOS identifiers and the windowsSettings values
 */

#include "precomp.h"

#include <winbase.h>

#define NS_2005 L"http://schemas.microsoft.com/SMI/2005/WindowsSettings"
#define NS_2016 L"http://schemas.microsoft.com/SMI/2016/WindowsSettings"
#define NS_2017 L"http://schemas.microsoft.com/SMI/2017/WindowsSettings"
#define NS_2019 L"http://schemas.microsoft.com/SMI/2019/WindowsSettings"

/* The identifiers published in "Targeting your application for Windows" */
static const GUID Guid_Win10 = {0x8e0f7a12, 0xbfb3, 0x4fe8, {0xb9, 0xa5, 0x48, 0xfd, 0x50, 0xa1, 0x5a, 0x9a}};
static const GUID Guid_Win81 = {0x1f676c76, 0x80e1, 0x4239, {0x95, 0xbb, 0x83, 0xd0, 0xf6, 0xd0, 0xda, 0x78}};
static const GUID Guid_Win7 = {0x35138b9a, 0x5d96, 0x4fbd, {0x8e, 0x2d, 0xa2, 0x44, 0x02, 0x25, 0xf9, 0x3a}};

static const char g_Manifest[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n"
    "  <assemblyIdentity type=\"win32\" name=\"LiberNT.ManifestSettings.Test\" version=\"1.0.0.0\"/>\r\n"
    "  <compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\r\n"
    "    <application>\r\n"
    "      <supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/>\r\n"
    "      <supportedOS Id=\"{35138b9a-5d96-4fbd-8e2d-a2440225f93a}\"/>\r\n"
    "    </application>\r\n"
    "  </compatibility>\r\n"
    "  <application xmlns=\"urn:schemas-microsoft-com:asm.v3\">\r\n"
    "    <windowsSettings>\r\n"
    "      <dpiAware xmlns=\"http://schemas.microsoft.com/SMI/2005/WindowsSettings\">true/pm</dpiAware>\r\n"
    "      <dpiAwareness xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">permonitorv2,permonitor</dpiAwareness>\r\n"
    "      <longPathAware xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">true</longPathAware>\r\n"
    "      <gdiScaling xmlns=\"http://schemas.microsoft.com/SMI/2017/WindowsSettings\">true</gdiScaling>\r\n"
    "      <activeCodePage xmlns=\"http://schemas.microsoft.com/SMI/2019/WindowsSettings\">UTF-8</activeCodePage>\r\n"
    "    </windowsSettings>\r\n"
    "  </application>\r\n"
    "</assembly>\r\n";

static
BOOL
HasOsGuid(const ACTIVATION_CONTEXT_COMPATIBILITY_INFORMATION *Info, const GUID *Guid)
{
    DWORD i;

    for (i = 0; i < Info->ElementCount; i++)
    {
        if (Info->Elements[i].Type == ACTCTX_COMPATIBILITY_ELEMENT_TYPE_OS &&
            IsEqualGUID(&Info->Elements[i].Id, Guid))
            return TRUE;
    }
    return FALSE;
}

static
void
CheckSetting(HANDLE Context, PCWSTR Namespace, PCWSTR Name, PCWSTR Expected)
{
    WCHAR Buffer[64] = L"?";
    SIZE_T Needed = 0;
    BOOL Result;

    SetLastError(0xDEADBEEF);
    Result = QueryActCtxSettingsW(0, Context, Namespace, Name, Buffer, ARRAYSIZE(Buffer), &Needed);
    ok(Result, "%ls: QueryActCtxSettingsW failed with %lu\n", Name, GetLastError());
    if (Result)
        ok(!wcscmp(Buffer, Expected), "%ls: got '%ls', expected '%ls'\n", Name, Buffer, Expected);
}

static
void
Test_Manifest(void)
{
    WCHAR Temp[MAX_PATH], Path[MAX_PATH];
    ACTCTXW ActCtx = { sizeof(ActCtx) };
    ACTIVATION_CONTEXT_COMPATIBILITY_INFORMATION *Info;
    HANDLE Context, File;
    DWORD Written;
    SIZE_T Needed = 0;
    BOOL Result;
    WCHAR Buffer[16];

    GetTempPathW(ARRAYSIZE(Temp), Temp);
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%lsmanifest-settings.%lu.manifest", Temp, GetCurrentProcessId());
    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        skip("Could not write the manifest\n");
        return;
    }
    WriteFile(File, g_Manifest, sizeof(g_Manifest) - 1, &Written, NULL);
    CloseHandle(File);

    ActCtx.lpSource = Path;
    Context = CreateActCtxW(&ActCtx);
    ok(Context != INVALID_HANDLE_VALUE, "CreateActCtxW failed with %lu\n", GetLastError());
    if (Context == INVALID_HANDLE_VALUE)
    {
        DeleteFileW(Path);
        return;
    }

    /* supportedOS: the declared identifiers, and only those */
    Result = QueryActCtxW(0, Context, NULL, CompatibilityInformationInActivationContext, NULL, 0, &Needed);
    ok(!Result && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Size query returned %d, error %lu\n", Result, GetLastError());
    Info = HeapAlloc(GetProcessHeap(), 0, Needed ? Needed : 1);
    if (Info && Needed)
    {
        Result = QueryActCtxW(0, Context, NULL, CompatibilityInformationInActivationContext, Info, Needed, &Needed);
        ok(Result, "QueryActCtxW failed with %lu\n", GetLastError());
        if (Result)
        {
            ok(HasOsGuid(Info, &Guid_Win10), "The Windows 10 identifier is missing\n");
            ok(HasOsGuid(Info, &Guid_Win7), "The Windows 7 identifier is missing\n");
            ok(!HasOsGuid(Info, &Guid_Win81), "An identifier that was not declared is reported\n");
        }
    }
    HeapFree(GetProcessHeap(), 0, Info);

    /* windowsSettings, in the namespace each element belongs to */
    CheckSetting(Context, NS_2005, L"dpiAware", L"true/pm");
    CheckSetting(Context, NS_2016, L"dpiAwareness", L"permonitorv2,permonitor");
    CheckSetting(Context, NS_2016, L"longPathAware", L"true");
    CheckSetting(Context, NS_2017, L"gdiScaling", L"true");
    CheckSetting(Context, NS_2019, L"activeCodePage", L"UTF-8");

    /* A setting that is absent, or asked in the wrong namespace, is not found */
    SetLastError(0xDEADBEEF);
    Result = QueryActCtxSettingsW(0, Context, NS_2005, L"autoElevate", Buffer, ARRAYSIZE(Buffer), &Needed);
    ok(!Result, "A setting the manifest does not declare was found\n");
    SetLastError(0xDEADBEEF);
    Result = QueryActCtxSettingsW(0, Context, NS_2005, L"gdiScaling", Buffer, ARRAYSIZE(Buffer), &Needed);
    ok(!Result, "gdiScaling was found in the 2005 namespace\n");

    ReleaseActCtx(Context);
    DeleteFileW(Path);
}

/*
 * Applications that do not declare support for Windows 8.1 or later get the Windows 8
 * version (6.2), from GetVersionEx and VerifyVersionInfo ("Targeting your application
 * for Windows"). The test program declares nothing.
 */
static
void
Test_NoManifestVersion(void)
{
    OSVERSIONINFOEXW Version = { sizeof(Version) };
    RTL_OSVERSIONINFOEXW Real = { sizeof(Real) };
    ACTIVATION_CONTEXT_COMPATIBILITY_INFORMATION Probe[8] = {{0}};
    SIZE_T Needed = 0;

    if (RtlGetVersion((PRTL_OSVERSIONINFOW)&Real) != 0)
    {
        skip("RtlGetVersion failed\n");
        return;
    }

    if (Real.dwMajorVersion < 10 && !(Real.dwMajorVersion == 6 && Real.dwMinorVersion >= 3))
    {
        skip("The system is older than Windows 8.1\n");
        return;
    }

    /* Only judge a program that really declares no supported OS */
    if (QueryActCtxW(QUERY_ACTCTX_FLAG_USE_ACTIVE_ACTCTX, NULL, NULL, CompatibilityInformationInActivationContext,
                     Probe, sizeof(Probe), &Needed) && Probe[0].ElementCount != 0)
    {
        skip("The test program declares supported systems\n");
        return;
    }

    /* Windows (measured on Windows 11) also trusts the OS version in the PE header of an unmanifested program:
     * a header that claims 6.3 or later is reported the real version, so the 6.2 rule only applies below that */
    {
        PIMAGE_NT_HEADERS Nt = RtlImageNtHeader(NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress);

        if (Nt && (Nt->OptionalHeader.MajorOperatingSystemVersion > 6 ||
                   (Nt->OptionalHeader.MajorOperatingSystemVersion == 6 &&
                    Nt->OptionalHeader.MinorOperatingSystemVersion >= 3)))
        {
            skip("The test program declares OS version %u.%u in its PE header\n",
                 Nt->OptionalHeader.MajorOperatingSystemVersion, Nt->OptionalHeader.MinorOperatingSystemVersion);
            return;
        }
    }

    ok(GetVersionExW((OSVERSIONINFOW *)&Version), "GetVersionExW failed with %lu\n", GetLastError());
    ok(Version.dwMajorVersion == 6 && Version.dwMinorVersion == 2,
       "GetVersionExW reports %lu.%lu for a program with no manifest, expected 6.2\n",
       Version.dwMajorVersion, Version.dwMinorVersion);

    /* RtlGetVersion is not subject to the manifest */
    ok(Real.dwMajorVersion > 6 || Real.dwMinorVersion >= 3,
       "RtlGetVersion reports %lu.%lu\n", Real.dwMajorVersion, Real.dwMinorVersion);
}

START_TEST(ManifestSettings)
{
    Test_Manifest();
    Test_NoManifestVersion();
}

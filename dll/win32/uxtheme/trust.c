/*
 * PROJECT:     LiberNT uxtheme
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Trust gate in front of every visual-style load (ThemeLoader4 I1a-T, THEME-TRUST-POLICY v5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "uxthemep.h"
#include <winreg.h>
#include <sddl.h>
#include <strsafe.h>
#include <tl4/trust/trust.h>
#include <tl4/loader/pe.h>
#include "trust.h"

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
#ifndef FILE_REMOVABLE_MEDIA
#define FILE_REMOVABLE_MEDIA 0x00000001
#endif
#ifndef FILE_REMOTE_DEVICE
#define FILE_REMOTE_DEVICE 0x00000010
#endif
#ifndef FILE_VIRTUAL_VOLUME
#define FILE_VIRTUAL_VOLUME 0x00000040
#endif

#define UXT_GATE_KEY        L"SOFTWARE\\Leeway\\UxTheme"
#define UXT_OWNER_FACT_KEY  L"SOFTWARE\\Leeway\\UxTheme\\ComponentOwner"
#define UXT_GUARD_USER      L"Software\\Leeway\\UxTheme\\Guard"
#define UXT_GUARD_MACHINE   L"SOFTWARE\\Leeway\\UxTheme\\Guard"
#define UXT_GATE_SDDL       L"O:BAG:SYD:PAI(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KR;;;BU)"
#define UXT_DEFAULT_STYLE   L"Aero"
#define UXT_NAME_MAX        32
#define UXT_GUARD_MAX       64
#define UXT_MAX_STYLE_SIZE  (32u * 1024u * 1024u)
#define UXT_RELOAD_MS       5000

static LONG g_classState;
static BOOL g_privileged = TRUE;    /* safe defaults until computed (T-DEF-4) */
static BOOL g_machineDefault = TRUE;
static BOOL g_userIsSystem;
static DWORD g_lastReload;
static BOOL g_reloaded;
static WCHAR g_windir[MAX_PATH];
static WCHAR g_windirNt[MAX_PATH];

static size_t
TrustWcsnlen(LPCWSTR s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

static BOOL
TrustWinDir(void)
{
    UINT n;

    if (g_windir[0])
        return TRUE;
    n = GetSystemWindowsDirectoryW(g_windir, ARRAY_SIZE(g_windir));
    if (!n || n >= ARRAY_SIZE(g_windir) - 64)
    {
        g_windir[0] = 0;
        return FALSE;
    }
    if (g_windir[n - 1] == L'\\')
        g_windir[n - 1] = 0;
    return TRUE;
}

static BOOL
TrustHandleName(HANDLE h, LPWSTR out, DWORD cch)
{
    BYTE buf[sizeof(OBJECT_NAME_INFORMATION) + MAX_PATH * 2 * sizeof(WCHAR)];
    POBJECT_NAME_INFORMATION oni = (POBJECT_NAME_INFORMATION)buf;
    ULONG ret = 0;
    DWORD chars;

    if (!NT_SUCCESS(NtQueryObject(h, ObjectNameInformation, oni, sizeof(buf), &ret)) || !oni->Name.Buffer)
        return FALSE;
    chars = oni->Name.Length / sizeof(WCHAR);
    if (!chars || chars >= cch)
        return FALSE;
    memcpy(out, oni->Name.Buffer, chars * sizeof(WCHAR));
    out[chars] = 0;
    return TRUE;
}

static BOOL
TrustWinDirNt(void)
{
    HANDLE h;
    BOOL ok;

    if (g_windirNt[0])
        return TRUE;
    if (!TrustWinDir())
        return FALSE;
    h = CreateFileW(g_windir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    ok = TrustHandleName(h, g_windirNt, ARRAY_SIZE(g_windirNt));
    CloseHandle(h);
    if (!ok)
        g_windirNt[0] = 0;
    return ok;
}

static PSYSTEM_PROCESS_INFORMATION
TrustProcessSnapshot(void)
{
    ULONG size = 256 * 1024, ret;
    PVOID buf;
    NTSTATUS st;

    for (;;)
    {
        buf = HeapAlloc(GetProcessHeap(), 0, size);
        if (!buf)
            return NULL;
        st = NtQuerySystemInformation(SystemProcessInformation, buf, size, &ret);
        if (NT_SUCCESS(st))
            return buf;
        HeapFree(GetProcessHeap(), 0, buf);
        if (st != STATUS_INFO_LENGTH_MISMATCH || size >= 16 * 1024 * 1024)
            return NULL;
        size *= 2;
    }
}

static PSYSTEM_PROCESS_INFORMATION
TrustFindProcess(PSYSTEM_PROCESS_INFORMATION snap, ULONG_PTR pid)
{
    PSYSTEM_PROCESS_INFORMATION p = snap;

    for (;;)
    {
        if ((ULONG_PTR)p->UniqueProcessId == pid)
            return p;
        if (!p->NextEntryOffset)
            return NULL;
        p = (PSYSTEM_PROCESS_INFORMATION)((PBYTE)p + p->NextEntryOffset);
    }
}

static int
TrustParentIsScm(PSYSTEM_PROCESS_INFORMATION snap)
{
    PROCESS_BASIC_INFORMATION pbi;
    PSYSTEM_PROCESS_INFORMATION self, parent;
    WCHAR expect[MAX_PATH], image[MAX_PATH];
    BYTE buf[sizeof(UNICODE_STRING) + MAX_PATH * 2 * sizeof(WCHAR)];
    PUNICODE_STRING us = (PUNICODE_STRING)buf;
    HANDLE h;
    ULONG ret;
    NTSTATUS st;

    if (!NT_SUCCESS(NtQueryInformationProcess(NtCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi), NULL)))
        return -1;
    self = TrustFindProcess(snap, (ULONG_PTR)GetCurrentProcessId());
    parent = TrustFindProcess(snap, pbi.InheritedFromUniqueProcessId);
    if (!self)
        return -1;
    if (!parent || parent->CreateTime.QuadPart >= self->CreateTime.QuadPart)
        return 0;
    if (!parent->ImageName.Buffer ||
        _wcsnicmp(parent->ImageName.Buffer, L"services.exe", parent->ImageName.Length / sizeof(WCHAR)) ||
        parent->ImageName.Length != 12 * sizeof(WCHAR))
        return 0;
    if (!TrustWinDirNt() || FAILED(StringCchPrintfW(expect, ARRAY_SIZE(expect), L"%s\\System32\\services.exe", g_windirNt)))
        return -1;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pbi.InheritedFromUniqueProcessId);
    if (!h)
        return -1;
    st = NtQueryInformationProcess(h, ProcessImageFileName, buf, sizeof(buf), &ret);
    CloseHandle(h);
    if (!NT_SUCCESS(st) || !us->Buffer || us->Length / sizeof(WCHAR) >= ARRAY_SIZE(image))
        return -1;
    memcpy(image, us->Buffer, us->Length);
    image[us->Length / sizeof(WCHAR)] = 0;
    return _wcsicmp(image, expect) == 0;
}

static BOOL
TrustIsLogonImage(void)
{
    WCHAR self[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, ARRAY_SIZE(self));

    if (!n || n >= ARRAY_SIZE(self) || !TrustWinDir())
        return FALSE;
    StringCchPrintfW(a, ARRAY_SIZE(a), L"%s\\System32\\winlogon.exe", g_windir);
    StringCchPrintfW(b, ARRAY_SIZE(b), L"%s\\System32\\logonui.exe", g_windir);
    return !_wcsicmp(self, a) || !_wcsicmp(self, b);
}

static PVOID
TrustTokenInfo(HANDLE tok, TOKEN_INFORMATION_CLASS cls)
{
    DWORD len = 0;
    PVOID p;

    GetTokenInformation(tok, cls, NULL, 0, &len);
    if (!len || len > 1024 * 1024 || !(p = HeapAlloc(GetProcessHeap(), 0, len)))
        return NULL;
    if (!GetTokenInformation(tok, cls, p, len, &len))
    {
        HeapFree(GetProcessHeap(), 0, p);
        return NULL;
    }
    return p;
}

static BOOL
TrustObjectName(HANDLE obj, WCHAR *out, DWORD cch, DWORD *chars)
{
    DWORD len = 0;

    if (!obj || !GetUserObjectInformationW(obj, UOI_NAME, out, cch * sizeof(WCHAR), &len) || len < sizeof(WCHAR))
        return FALSE;
    *chars = (DWORD)TrustWcsnlen(out, cch);
    return *chars > 0 && *chars < cch;
}

static void
TrustClassify(void)
{
    tl4_trust_facts f;
    tl4_trust_sid_ref *groups = NULL;
    uint64_t *privs = NULL;
    HANDLE tok = NULL;
    PTOKEN_USER user = NULL;
    PTOKEN_GROUPS tg = NULL;
    PTOKEN_PRIVILEGES tp = NULL;
    PTOKEN_MANDATORY_LABEL label = NULL;
    TOKEN_ELEVATION_TYPE etype;
    TOKEN_ELEVATION elev;
    DWORD len, i, session, dchars = 0, wchars = 0;
    WCHAR desk[TL4_TRUST_MAX_NAME + 1], winsta[TL4_TRUST_MAX_NAME + 1];
    PSYSTEM_PROCESS_INFORMATION snap = NULL;
    int scm;
    tl4_trust_class cls;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID system = NULL;

    ZeroMemory(&f, sizeof(f));
    f.complete = 1;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    {
        f.complete = 0;
        goto decide;
    }
    user = TrustTokenInfo(tok, TokenUser);
    tg = TrustTokenInfo(tok, TokenGroups);
    tp = TrustTokenInfo(tok, TokenPrivileges);
    label = TrustTokenInfo(tok, TokenIntegrityLevel);
    if (!user || !tg || !tp || !label ||
        !GetTokenInformation(tok, TokenElevationType, &etype, sizeof(etype), &len) ||
        !GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &len) ||
        !GetTokenInformation(tok, TokenSessionId, &session, sizeof(session), &len))
    {
        f.complete = 0;
        goto decide;
    }
    f.user.sid = (const uint8_t *)user->User.Sid;
    f.user.size = GetLengthSid(user->User.Sid);
    if (tg->GroupCount > TL4_TRUST_MAX_GROUPS)
        f.ngroups = TL4_TRUST_MAX_GROUPS + 1;   /* the classifier reports it as incomplete */
    else if (tg->GroupCount)
    {
        groups = HeapAlloc(GetProcessHeap(), 0, tg->GroupCount * sizeof(*groups));
        if (!groups)
        {
            f.complete = 0;
            goto decide;
        }
        for (i = 0; i < tg->GroupCount; i++)
        {
            groups[i].sid = (const uint8_t *)tg->Groups[i].Sid;
            groups[i].size = GetLengthSid(tg->Groups[i].Sid);
            groups[i].attributes = tg->Groups[i].Attributes;
        }
        f.groups = groups;
        f.ngroups = tg->GroupCount;
    }
    if (tp->PrivilegeCount > TL4_TRUST_MAX_PRIVILEGES)
        f.nprivileges = TL4_TRUST_MAX_PRIVILEGES + 1;
    else if (tp->PrivilegeCount)
    {
        privs = HeapAlloc(GetProcessHeap(), 0, tp->PrivilegeCount * sizeof(*privs));
        if (!privs)
        {
            f.complete = 0;
            goto decide;
        }
        for (i = 0; i < tp->PrivilegeCount; i++)
            privs[i] = ((uint64_t)(ULONG)tp->Privileges[i].Luid.HighPart << 32) | tp->Privileges[i].Luid.LowPart;
        f.privileges = privs;
        f.nprivileges = tp->PrivilegeCount;
    }
    f.elevation_type = (uint32_t)etype;
    f.is_elevated = elev.TokenIsElevated != 0;
    {
        PUCHAR count = GetSidSubAuthorityCount(label->Label.Sid);
        f.integrity_rid = (count && *count) ? *GetSidSubAuthority(label->Label.Sid, *count - 1) : 0;
    }
    f.session_id = session;
    f.interactive_in_session0 = 0;  /* LiberNT: interactive logons run in session 1 (I1-ACL-TOKEN-EVIDENCE) */
    snap = TrustProcessSnapshot();
    scm = snap ? TrustParentIsScm(snap) : -1;
    if (scm < 0)
        f.complete = 0;
    f.started_by_scm = scm > 0;
    f.is_logon_process = TrustIsLogonImage();
    if (!TrustObjectName(GetThreadDesktop(GetCurrentThreadId()), desk, ARRAY_SIZE(desk), &dchars) ||
        !TrustObjectName(GetProcessWindowStation(), winsta, ARRAY_SIZE(winsta), &wchars))
        f.complete = 0;
    f.desktop = (const uint16_t *)desk;
    f.desktop_chars = dchars;
    f.winstation = (const uint16_t *)winsta;
    f.winstation_chars = wchars;

decide:
    cls = tl4_trust_classify(&f);
    g_privileged = cls != TL4_TRUST_ORDINARY;
    /* T-PRIV-2 (the pure decision) or T-PRIV-6 (logon or secure desktop, kept here as Jim 2 asked) */
    g_machineDefault = tl4_trust_uses_machine_default(&f) || f.is_logon_process ||
                       (dchars == 8 && !_wcsnicmp(desk, L"Winlogon", 8)) ||
                       (dchars == 12 && !_wcsnicmp(desk, L"Screen-saver", 12)) ||
                       !(wchars == 7 && !_wcsnicmp(winsta, L"WinSta0", 7));
    if (user && AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &system))
    {
        g_userIsSystem = EqualSid(user->User.Sid, system);
        FreeSid(system);
    }
    TRACE("trust: class %d privileged %d machine default %d\n", cls, g_privileged, g_machineDefault);
    if (snap)
        HeapFree(GetProcessHeap(), 0, snap);
    if (groups)
        HeapFree(GetProcessHeap(), 0, groups);
    if (privs)
        HeapFree(GetProcessHeap(), 0, privs);
    if (user)
        HeapFree(GetProcessHeap(), 0, user);
    if (tg)
        HeapFree(GetProcessHeap(), 0, tg);
    if (tp)
        HeapFree(GetProcessHeap(), 0, tp);
    if (label)
        HeapFree(GetProcessHeap(), 0, label);
    if (tok)
        CloseHandle(tok);
}

static void
TrustEnsureClassified(void)
{
    if (InterlockedCompareExchange(&g_classState, 1, 0) == 0)
    {
        TrustClassify();
        InterlockedExchange(&g_classState, 2);
    }
    /* a concurrent first caller sees the safe defaults (privileged, machine default) until the result is in */
}

BOOL
UXTHEME_TrustIsPrivileged(void)
{
    TrustEnsureClassified();
    return g_classState != 2 || g_privileged;
}

BOOL
UXTHEME_TrustUsesMachineDefault(void)
{
    TrustEnsureClassified();
    return g_classState != 2 || g_machineDefault;
}

static void
TrustEvent(LPCWSTR text, LPCWSTR name)
{
    HANDLE src = RegisterEventSourceW(NULL, L"UxTheme");
    WCHAR msg[160];
    LPCWSTR strings[1] = { msg };

    if (!src)
        return;
    StringCchPrintfW(msg, ARRAY_SIZE(msg), L"%s: %s", text, name ? name : L"-");
    ReportEventW(src, EVENTLOG_WARNING_TYPE, 0, 0x1000, NULL, 1, 0, strings, NULL);
    DeregisterEventSource(src);
}

static BOOL
TrustKeyProtected(HKEY key)
{
    BYTE sd[4096];
    DWORD len = sizeof(sd);

    if (RegGetKeySecurity(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, sd, &len) != ERROR_SUCCESS)
        return FALSE;
    return tl4_trust_sd_writable(sd, len, TL4_TRUST_DIR, 0) == TL4_TRUST_SAFE;
}

static BOOL
TrustReadMachineValue(LPCWSTR subkey, LPCWSTR value, DWORD type, PVOID data, DWORD size)
{
    HKEY gate = NULL, key = NULL;
    DWORD t, n = size;
    BOOL ok = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UXT_GATE_KEY, 0, KEY_READ, &gate) != ERROR_SUCCESS)
        return FALSE;
    if (!TrustKeyProtected(gate))
        goto done;
    if (subkey)
    {
        if (RegOpenKeyExW(gate, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS || !TrustKeyProtected(key))
            goto done;
    }
    ok = RegQueryValueExW(key ? key : gate, value, NULL, &t, data, &n) == ERROR_SUCCESS && t == type &&
         (type != REG_DWORD || n == sizeof(DWORD));
done:
    if (key)
        RegCloseKey(key);
    RegCloseKey(gate);
    return ok;
}

static BOOL
TrustGateDword(LPCWSTR value)
{
    DWORD v = 0;
    return TrustReadMachineValue(NULL, value, REG_DWORD, &v, sizeof(v)) && v == 1;
}

BOOL
UXTHEME_TrustV3AnyPathOrdinary(void)
{
    /* T-GATE-4: never read by a privileged process */
    return !UXTHEME_TrustIsPrivileged() && TrustGateDword(L"V3AnyPathOrdinary");
}

BOOL
UXTHEME_TrustReloadAllowed(void)
{
    DWORD now = GetTickCount();

    if (!UXTHEME_TrustIsPrivileged())
        return TRUE;
    if (g_reloaded && now - g_lastReload < UXT_RELOAD_MS)   /* unsigned difference: wrap-safe */
        return FALSE;
    g_lastReload = now;
    g_reloaded = TRUE;
    return TRUE;
}

static BOOL
TrustNameValid(LPCWSTR s, size_t n)
{
    size_t i;

    if (n < 1 || n > UXT_NAME_MAX)
        return FALSE;
    for (i = 0; i < n; i++)
    {
        WCHAR c = s[i];
        BOOL alnum = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');

        if (!alnum && (i == 0 || (c != L'_' && c != L'-')))
            return FALSE;
    }
    return TRUE;
}

BOOL
UXTHEME_TrustBuildV3Path(LPCWSTR name, LPWSTR path, DWORD cchPath)
{
    if (!TrustNameValid(name, wcslen(name)) || !TrustWinDir())
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(path, cchPath, L"%s\\Resources\\Themes\\%s\\%s.msstyles", g_windir, name, name));
}

BOOL
UXTHEME_TrustNameFromStoredPath(LPCWSTR text, LPWSTR name, DWORD cchName)
{
    static const WCHAR mid[] = L"\\Resources\\Themes\\";
    size_t pre, n, total;
    LPCWSTR p, sep;

    if (!text || !TrustWinDir())
        return FALSE;
    total = TrustWcsnlen(text, MAX_PATH);
    if (total >= MAX_PATH)
        return FALSE;
    pre = wcslen(g_windir);
    if (!_wcsnicmp(text, g_windir, pre))
        p = text + pre;
    else if (!_wcsnicmp(text, L"%SystemRoot%", 12))   /* a fixed token, never expanded (T-LOAD-1 v4) */
        p = text + 12;
    else
        return FALSE;
    if (_wcsnicmp(p, mid, ARRAY_SIZE(mid) - 1))
        return FALSE;
    p += ARRAY_SIZE(mid) - 1;
    sep = wcschr(p, L'\\');
    if (!sep)
        return FALSE;
    n = (size_t)(sep - p);
    if (!TrustNameValid(p, n) || n >= cchName)
        return FALSE;
    if (_wcsnicmp(sep + 1, p, n) || _wcsicmp(sep + 1 + n, L".msstyles"))
        return FALSE;
    memcpy(name, p, n * sizeof(WCHAR));
    name[n] = 0;
    return TRUE;
}

void
UXTHEME_TrustMachineDefault(LPWSTR name, DWORD cchName)
{
    WCHAR v[UXT_NAME_MAX + 1];

    ZeroMemory(v, sizeof(v));
    if (TrustReadMachineValue(NULL, L"MachineDefault", REG_SZ, v, sizeof(v) - sizeof(WCHAR)) &&
        TrustNameValid(v, TrustWcsnlen(v, ARRAY_SIZE(v))))
        StringCchCopyW(name, cchName, v);
    else
        StringCchCopyW(name, cchName, UXT_DEFAULT_STYLE);
}

static BOOL
TrustTokenHasSid(HANDLE tok, PSID sid)
{
    PTOKEN_USER user = TrustTokenInfo(tok, TokenUser);
    PTOKEN_GROUPS tg = TrustTokenInfo(tok, TokenGroups);
    BOOL has = TRUE;   /* unknown counts as "carries it" */
    DWORD i;

    if (user && tg)
    {
        has = EqualSid(user->User.Sid, sid);
        for (i = 0; !has && i < tg->GroupCount; i++)
            has = EqualSid(tg->Groups[i].Sid, sid);
    }
    if (user)
        HeapFree(GetProcessHeap(), 0, user);
    if (tg)
        HeapFree(GetProcessHeap(), 0, tg);
    return has;
}

static BOOL
TrustComputeOwnerFact(void)
{
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID ti = NULL;
    PSYSTEM_PROCESS_INFORMATION snap, p;
    HKEY svc;
    BOOL ok = TRUE;

    if (!AllocateAndInitializeSid(&nt, 6, 80, 956008885, 3418522649UL, 1831038044, 1853292631, 2271478464UL, 0, 0, &ti))
        return FALSE;
    /* any service that would run under the SID is defined where ordinary principals cannot write */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\TrustedInstaller", 0, KEY_READ, &svc) ==
        ERROR_SUCCESS)
    {
        ok = TrustKeyProtected(svc);
        RegCloseKey(svc);
    }
    /* no process token on the system carries it */
    snap = ok ? TrustProcessSnapshot() : NULL;
    if (!snap)
        ok = FALSE;
    for (p = snap; ok && p; p = p->NextEntryOffset ? (PSYSTEM_PROCESS_INFORMATION)((PBYTE)p + p->NextEntryOffset) : NULL)
    {
        ULONG_PTR pid = (ULONG_PTR)p->UniqueProcessId;
        HANDLE h, tok;

        if (pid == 0 || pid == 4)   /* idle and System: kernel, no user token to inspect */
            continue;
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (!h)
        {
            ok = FALSE;             /* cannot check a process: the fact is not shown */
            break;
        }
        if (!OpenProcessToken(h, TOKEN_QUERY, &tok))
            ok = FALSE;
        else
        {
            if (TrustTokenHasSid(tok, ti))
                ok = FALSE;
            CloseHandle(tok);
        }
        CloseHandle(h);
    }
    if (snap)
        HeapFree(GetProcessHeap(), 0, snap);
    FreeSid(ti);
    return ok;
}

static BOOL
TrustOwnerTrusted(void)
{
    static LONG state;
    DWORD v = 0;

    if (state)
        return state == 1;
    if (!TrustReadMachineValue(L"ComponentOwner", L"Trusted", REG_DWORD, &v, sizeof(v)) && g_userIsSystem)
    {
        /* first SYSTEM process of the boot: compute and publish the fact (volatile, SYSTEM-only) */
        PSECURITY_DESCRIPTOR sd = NULL;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
        HKEY gate, fact;

        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(UXT_GATE_SDDL, SDDL_REVISION_1, &sd, NULL))
        {
            sa.lpSecurityDescriptor = sd;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, UXT_GATE_KEY, 0, NULL, 0, KEY_ALL_ACCESS, &sa, &gate, NULL) ==
                ERROR_SUCCESS)
            {
                v = TrustComputeOwnerFact() ? 1 : 0;
                if (RegCreateKeyExW(gate, L"ComponentOwner", 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, &sa, &fact,
                                    NULL) == ERROR_SUCCESS)
                {
                    RegSetValueExW(fact, L"Trusted", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
                    RegCloseKey(fact);
                }
                RegCloseKey(gate);
            }
            LocalFree(sd);
        }
        if (!TrustReadMachineValue(L"ComponentOwner", L"Trusted", REG_DWORD, &v, sizeof(v)))
            v = 0;
    }
    state = v == 1 ? 1 : 2;
    return v == 1;
}

static BOOL
TrustSdOk(HANDLE h, enum tl4_trust_object kind, unsigned flags)
{
    BYTE stackbuf[1024];
    PBYTE sd = stackbuf;
    ULONG len = sizeof(stackbuf), need = 0;
    NTSTATUS st;
    BOOL ok;

    st = NtQuerySecurityObject(h, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, sd, len, &need);
    if (st == STATUS_BUFFER_TOO_SMALL && need <= TL4_TRUST_MAX_SD)
    {
        sd = HeapAlloc(GetProcessHeap(), 0, need);
        if (!sd)
            return FALSE;
        len = need;
        st = NtQuerySecurityObject(h, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, sd, len, &need);
    }
    ok = NT_SUCCESS(st) && tl4_trust_sd_writable(sd, need ? need : len, kind, flags) == TL4_TRUST_SAFE;
    if (sd != stackbuf)
        HeapFree(GetProcessHeap(), 0, sd);
    return ok;
}

static BOOL
TrustNotReparse(HANDLE h, BOOL wantDir)
{
    IO_STATUS_BLOCK iosb;
    FILE_ATTRIBUTE_TAG_INFORMATION tag;

    if (!NT_SUCCESS(NtQueryInformationFile(h, &iosb, &tag, sizeof(tag), FileAttributeTagInformation)))
        return FALSE;
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return FALSE;
    return wantDir ? (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 : !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL
TrustVolumeOk(HANDLE h)
{
    IO_STATUS_BLOCK iosb;
    FILE_FS_DEVICE_INFORMATION dev;
    BYTE buf[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_FS_ATTRIBUTE_INFORMATION attr = (PFILE_FS_ATTRIBUTE_INFORMATION)buf;

    if (!NT_SUCCESS(NtQueryVolumeInformationFile(h, &iosb, &dev, sizeof(dev), FileFsDeviceInformation)) ||
        dev.DeviceType != FILE_DEVICE_DISK ||
        (dev.Characteristics & (FILE_REMOVABLE_MEDIA | FILE_REMOTE_DEVICE | FILE_VIRTUAL_VOLUME)))
        return FALSE;
    if (!NT_SUCCESS(NtQueryVolumeInformationFile(h, &iosb, attr, sizeof(buf), FileFsAttributeInformation)) ||
        !(attr->FileSystemAttributes & FILE_PERSISTENT_ACLS))
        return FALSE;
    return attr->FileSystemNameLength == 4 * sizeof(WCHAR) && !_wcsnicmp(attr->FileSystemName, L"NTFS", 4);
}

static BOOL
TrustExpectedNt(LPCWSTR dos, LPWSTR out, DWORD cch)
{
    size_t pre = wcslen(g_windir);

    if (!TrustWinDirNt() || _wcsnicmp(dos, g_windir, pre) || (dos[pre] && dos[pre] != L'\\'))
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s%s", g_windirNt, dos + pre));
}

static HRESULT
TrustOpenChecked(LPCWSTR path, LPCWSTR storeRoot, HANDLE *phFile)
{
    WCHAR nt[MAX_PATH], want[MAX_PATH], dir[MAX_PATH];
    unsigned flags = TrustOwnerTrusted() ? TL4_TRUST_COMPONENT_OWNER_TRUSTED : 0;
    size_t rootLen = wcslen(storeRoot);
    HANDLE h;
    HRESULT hr = E_ACCESSDENIED;
    LPWSTR cut;

    *phFile = INVALID_HANDLE_VALUE;
    h = CreateFileW(path, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());   /* a sharing violation is a clean refusal: no mark yet */
    if (!TrustNotReparse(h, FALSE) || GetFileType(h) != FILE_TYPE_DISK || !TrustVolumeOk(h) ||
        !TrustHandleName(h, nt, ARRAY_SIZE(nt)) || !TrustExpectedNt(path, want, ARRAY_SIZE(want)) ||
        _wcsicmp(nt, want) || !TrustSdOk(h, TL4_TRUST_FILE, flags))
        goto fail;

    if (FAILED(StringCchCopyW(dir, ARRAY_SIZE(dir), path)))
        goto fail;
    while ((cut = wcsrchr(dir, L'\\')) != NULL)
    {
        HANDLE hd;
        BOOL ok, isRoot = FALSE;
        enum tl4_trust_object kind;

        *cut = 0;
        if (wcslen(dir) == 2 && dir[1] == L':')
        {
            dir[2] = L'\\';
            dir[3] = 0;
            isRoot = TRUE;
        }
        kind = (wcslen(dir) >= rootLen && !_wcsnicmp(dir, storeRoot, rootLen)) ? TL4_TRUST_DIR : TL4_TRUST_DIR_ABOVE_ROOT;
        hd = CreateFileW(dir, READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (hd == INVALID_HANDLE_VALUE)
            goto fail;
        ok = TrustNotReparse(hd, TRUE) && TrustSdOk(hd, kind, flags) && TrustHandleName(hd, nt, ARRAY_SIZE(nt));
        if (ok)
        {
            size_t n = wcslen(nt);
            ok = !_wcsnicmp(nt, want, n) && (want[n] == L'\\' || (n && nt[n - 1] == L'\\'));
        }
        CloseHandle(hd);
        if (!ok)
            goto fail;
        if (isRoot)
            break;
    }
    *phFile = h;
    return S_OK;

fail:
    CloseHandle(h);
    return hr;
}

static HRESULT
TrustPeekVersion(HANDLE hFile, WORD *version)
{
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION std;
    HANDLE hs = NULL;
    PVOID base = NULL;
    SIZE_T view = 0;
    LARGE_INTEGER max;
    tl4_pe pe;
    tl4_res_name type = tl4_res_ascii("PACKTHEM_VERSION"), name = tl4_res_id(1);
    tl4_resource res;
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

    if (!NT_SUCCESS(NtQueryInformationFile(hFile, &iosb, &std, sizeof(std), FileStandardInformation)) ||
        std.EndOfFile.QuadPart <= 0 || std.EndOfFile.QuadPart > UXT_MAX_STYLE_SIZE)
        return hr;
    max = std.EndOfFile;
    if (!NT_SUCCESS(NtCreateSection(&hs, SECTION_MAP_READ | SECTION_QUERY, NULL, &max, PAGE_READONLY, SEC_COMMIT,
                                    hFile)))
        return hr;
    if (NT_SUCCESS(NtMapViewOfSection(hs, NtCurrentProcess(), &base, 0, 0, NULL, &view, ViewUnmap, 0, PAGE_READONLY)))
    {
        if (tl4_pe_open(base, (size_t)std.EndOfFile.QuadPart, &pe) == TL4_OK &&
            tl4_pe_find(&pe, &type, &name, &res) == TL4_OK && res.size == 2)
        {
            *version = (WORD)(res.data[0] | (res.data[1] << 8));
            hr = S_OK;
        }
        NtUnmapViewOfSection(NtCurrentProcess(), base);
    }
    NtClose(hs);
    return hr;
}

typedef struct _UXT_GUARD
{
    HKEY root;
    WCHAR subkey[80];
    WCHAR value[32];
    BOOL written;
} UXT_GUARD;

typedef struct _UXT_GUARD_DATA
{
    DWORD version;
    DWORD reserved;
    ULONGLONG writtenAt;
} UXT_GUARD_DATA;

static BOOL
TrustParseGuardName(LPCWSTR s, ULONG_PTR *pid, ULONGLONG *ctime)
{
    WCHAR *end;
    unsigned long p;
    unsigned long long t;

    if (wcslen(s) != 25 || s[8] != L'-')
        return FALSE;
    p = wcstoul(s, &end, 16);
    if (end != s + 8)
        return FALSE;
    t = _wcstoui64(s + 9, &end, 16);
    if (end != s + 25)
        return FALSE;
    *pid = p;
    *ctime = t;
    return TRUE;
}

static HRESULT
TrustGuardEnter(LPCWSTR part, LPCWSTR name, UXT_GUARD *g)
{
    PSYSTEM_PROCESS_INFORMATION snap, self;
    HKEY key;
    DWORD i, count = 0;
    UXT_GUARD_DATA data;
    FILETIME now;
    HRESULT hr = S_OK;

    ZeroMemory(g, sizeof(*g));
    g->root = g_userIsSystem ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    if (FAILED(StringCchPrintfW(g->subkey, ARRAY_SIZE(g->subkey), L"%s\\%s-%s",
                                g_userIsSystem ? UXT_GUARD_MACHINE : UXT_GUARD_USER, part, name)))
        return E_FAIL;
    snap = TrustProcessSnapshot();
    if (!snap)
        return E_FAIL;
    self = TrustFindProcess(snap, (ULONG_PTR)GetCurrentProcessId());
    if (!self)
    {
        HeapFree(GetProcessHeap(), 0, snap);
        return E_FAIL;
    }
    StringCchPrintfW(g->value, ARRAY_SIZE(g->value), L"%08lX-%016I64X", GetCurrentProcessId(),
                     (ULONGLONG)self->CreateTime.QuadPart);

    if (RegOpenKeyExW(g->root, g->subkey, 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        for (i = 0; hr == S_OK; i++)
        {
            WCHAR vname[64];
            DWORD vlen = ARRAY_SIZE(vname), type, size = sizeof(data);
            ULONG_PTR pid;
            ULONGLONG ctime;
            PSYSTEM_PROCESS_INFORMATION p;
            LONG r = RegEnumValueW(key, i, vname, &vlen, NULL, &type, (LPBYTE)&data, &size);

            if (r == ERROR_NO_MORE_ITEMS)
                break;
            if (++count > UXT_GUARD_MAX || r != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(data) ||
                data.version != 1 || !TrustParseGuardName(vname, &pid, &ctime))
            {
                hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);   /* malformed counts as stale: skip the style */
                break;
            }
            p = TrustFindProcess(snap, pid);
            if (!p || (ULONGLONG)p->CreateTime.QuadPart != ctime)
                hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);   /* T-GUARD-2: a load died */
        }
        RegCloseKey(key);
    }
    HeapFree(GetProcessHeap(), 0, snap);
    if (FAILED(hr))
    {
        TrustEvent(L"style skipped after a failed load (crash guard)", name);
        return hr;
    }

    GetSystemTimeAsFileTime(&now);
    data.version = 1;
    data.reserved = 0;
    data.writtenAt = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    if (RegCreateKeyExW(g->root, g->subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);         /* T-GUARD-4 */
    if (RegSetValueExW(key, g->value, 0, REG_BINARY, (const BYTE *)&data, sizeof(data)) != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    RegCloseKey(key);
    g->written = TRUE;
    return S_OK;
}

static void
TrustGuardLeave(UXT_GUARD *g)
{
    HKEY key;

    if (!g->written)
        return;
    if (RegOpenKeyExW(g->root, g->subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, g->value);
        RegCloseKey(key);
    }
    g->written = FALSE;
}

void
UXTHEME_TrustClearGuard(PTHEME_FILE tf)
{
    if (!tf || !tf->szTrustGuard[0])
        return;
    RegDeleteKeyW(tf->bTrustGuardMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, tf->szTrustGuard);
}

static BOOL
TrustImpersonating(void)
{
    HANDLE t;

    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t))
    {
        CloseHandle(t);
        return TRUE;
    }
    return GetLastError() != ERROR_NO_TOKEN;   /* cannot tell: treat as impersonating (T-FAIL-3) */
}

HRESULT
UXTHEME_TrustOpenThemeFile(LPCWSTR path, LPCWSTR pszColorName, LPCWSTR pszSizeName, PTHEME_FILE *tf)
{
    WCHAR name[UXT_NAME_MAX + 1], built[MAX_PATH], storeRoot[MAX_PATH];
    HANDLE hFile;
    UXT_GUARD guard;
    WORD version = 0;
    HRESULT hr;

    *tf = NULL;
    if (!path)
        return E_POINTER;
    TrustEnsureClassified();

    /* outside the store (5.9): v3 only in an ordinary process with the T-GATE-4 rollback on */
    if (!UXTHEME_TrustNameFromStoredPath(path, name, ARRAY_SIZE(name)) ||
        !UXTHEME_TrustBuildV3Path(name, built, ARRAY_SIZE(built)))
    {
        if (UXTHEME_TrustV3AnyPathOrdinary())
        {
            TrustEvent(L"style loaded outside the store (rollback V3AnyPathOrdinary)", NULL);
            return MSSTYLES_OpenThemeFileV3(path, pszColorName, pszSizeName, tf);
        }
        TrustEvent(L"style refused: outside the store", NULL);
        return E_ACCESSDENIED;
    }
    if (TrustImpersonating())                               /* T-PRIV-4 */
        return E_ACCESSDENIED;
    if (FAILED(StringCchPrintfW(storeRoot, ARRAY_SIZE(storeRoot), L"%s\\Resources\\Themes", g_windir)))
        return E_FAIL;

    hr = TrustOpenChecked(built, storeRoot, &hFile);
    if (FAILED(hr))
    {
        TrustEvent(L"style refused: location check", name);
        return hr;
    }
    hr = TrustGuardEnter(UXT_PART_THEMES, name, &guard);
    if (FAILED(hr))
    {
        CloseHandle(hFile);
        return hr;
    }
    hr = TrustPeekVersion(hFile, &version);
    if (SUCCEEDED(hr) && version == 3)
    {
        /* T-LOAD-2 exception: the v3 loader opens the checked path once more; our handle keeps writers out */
        hr = MSSTYLES_OpenThemeFileV3(built, pszColorName, pszSizeName, tf);
    }
    else if (SUCCEEDED(hr))
        hr = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);          /* version 4 comes with I1a-V */
    CloseHandle(hFile);
    TrustGuardLeave(&guard);                                /* a clean finish, success or refusal */
    if (SUCCEEDED(hr) && *tf)
    {
        StringCchCopyW((*tf)->szTrustGuard, ARRAY_SIZE((*tf)->szTrustGuard), guard.subkey);
        (*tf)->bTrustGuardMachine = guard.root == HKEY_LOCAL_MACHINE;
    }
    if (FAILED(hr))
        TrustEvent(L"style refused", name);
    return hr;
}

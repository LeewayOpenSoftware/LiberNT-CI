/*
 * PROJECT:     LiberNT hnetcfg
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Firewall policy store behind INetFwPolicy2: the profile settings and the rules
 *              the firewall service (MpsSvc) applies, where Windows keeps them
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * A rule is kept as its Windows rule string ("v2.10|Action=Allow|Dir=In|..."): the fields are
 * read and replaced in the string, and every string written is first checked with the parser
 * the service uses (mpssvc policy.c), so the service never meets a rule it cannot read.
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "ole2.h"
#include "netfw.h"
#include "netlistmgr.h"
#include "strsafe.h"

#include "wine/debug.h"
#include "hnetcfg_private.h"
#include "mpssvc_policy.h"


#define FW_POLICY_KEY L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy"
#define FW_RULES_KEY  FW_POLICY_KEY L"\\FirewallRules"
#define FW_NEW_RULE   L"v2.10|Action=Allow|Active=FALSE|Dir=In|"

static const CLSID fw_clsid_nlm = { 0xdcb00c01, 0x570f, 0x4a9b, { 0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b } };
static const IID fw_iid_nlm = { 0xdcb00000, 0x570f, 0x4a9b, { 0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b } };

/* ---- profiles ---------------------------------------------------------------------------- */

static const WCHAR *profile_subkey( LONG bit )
{
    switch (bit)
    {
    case NET_FW_PROFILE2_DOMAIN:  return L"DomainProfile";
    case NET_FW_PROFILE2_PRIVATE: return L"StandardProfile";
    case NET_FW_PROFILE2_PUBLIC:  return L"PublicProfile";
    }
    return NULL;
}

/* one profile; absent values read as the given default (EnableFirewall: off) */
HRESULT fwstore_get_profile_dword( NET_FW_PROFILE_TYPE2 profile, const WCHAR *name, DWORD def, DWORD *value )
{
    WCHAR path[MAX_PATH];
    const WCHAR *sub = profile_subkey( profile );
    DWORD size = sizeof(*value);

    if (!value) return E_POINTER;
    if (!sub) return E_INVALIDARG;
    StringCchPrintfW( path, ARRAY_SIZE(path), L"%s\\%s", FW_POLICY_KEY, sub );
    if (RegGetValueW( HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_DWORD, NULL, value, &size ))
        *value = def;
    return S_OK;
}

/* every profile of the mask (NET_FW_PROFILE2_ALL included) */
HRESULT fwstore_set_profile_dword( LONG profiles, const WCHAR *name, DWORD value )
{
    static const LONG bits[] = { NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC };
    unsigned int i, done = 0;

    for (i = 0; i < ARRAY_SIZE(bits); i++)
    {
        WCHAR path[MAX_PATH];
        HKEY key;
        LSTATUS err;

        if (!(profiles & bits[i])) continue;
        StringCchPrintfW( path, ARRAY_SIZE(path), L"%s\\%s", FW_POLICY_KEY, profile_subkey( bits[i] ) );
        err = RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL );
        if (!err)
        {
            err = RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
            RegCloseKey( key );
        }
        if (err) return HRESULT_FROM_WIN32( err );
        done++;
    }
    return done ? S_OK : E_INVALIDARG;
}

/* the profiles of the connected networks, from the Network List Manager */
LONG fwstore_current_profiles( void )
{
    INetworkListManager *list = NULL;
    IEnumNetworks *networks = NULL;
    INetwork *network;
    ULONG fetched;
    LONG mask = 0;

    if (FAILED(CoCreateInstance( &fw_clsid_nlm, NULL, CLSCTX_INPROC_SERVER, &fw_iid_nlm, (void **)&list )))
        return NET_FW_PROFILE2_PUBLIC;
    if (SUCCEEDED(INetworkListManager_GetNetworks( list, NLM_ENUM_NETWORK_CONNECTED, &networks )) && networks)
    {
        while (IEnumNetworks_Next( networks, 1, &network, &fetched ) == S_OK && fetched)
        {
            NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
            if (FAILED(INetwork_GetCategory( network, &category ))) category = NLM_NETWORK_CATEGORY_PUBLIC;
            if (category == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED) mask |= NET_FW_PROFILE2_DOMAIN;
            else if (category == NLM_NETWORK_CATEGORY_PRIVATE) mask |= NET_FW_PROFILE2_PRIVATE;
            else mask |= NET_FW_PROFILE2_PUBLIC;
            INetwork_Release( network );
        }
        IEnumNetworks_Release( networks );
    }
    INetworkListManager_Release( list );
    return mask ? mask : NET_FW_PROFILE2_PUBLIC;
}

/* No policy is the default here (the profiles off, no rules), so restoring it removes what the
   policy holds; a later Windows-default policy will write its profiles and rules instead. The key
   itself stays: its access control, and the firewall service watching it */
HRESULT fwstore_restore_defaults( void )
{
    LSTATUS err;
    HKEY key;

    err = RegOpenKeyExW( HKEY_LOCAL_MACHINE, FW_POLICY_KEY, 0,
                         DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE, &key );
    if (err == ERROR_FILE_NOT_FOUND) return S_OK;
    if (err) return HRESULT_FROM_WIN32( err );
    err = RegDeleteTreeW( key, NULL );
    RegCloseKey( key );
    return HRESULT_FROM_WIN32( err );
}

/* ---- rule strings -------------------------------------------------------------------------- */

/* the first field "key=" at or after *pos (a field starts after a '|'); its value and length */
static const WCHAR *find_field( const WCHAR *text, const WCHAR *key, const WCHAR **pos, size_t *len )
{
    size_t keylen = wcslen( key );
    const WCHAR *p = *pos ? *pos : text;

    while ((p = wcschr( p, '|' )))
    {
        p++;
        if (!wcsncmp( p, key, keylen ) && p[keylen] == '=')
        {
            const WCHAR *value = p + keylen + 1, *end = wcschr( value, '|' );
            *len = end ? (size_t)(end - value) : wcslen( value );
            *pos = value;
            return value;
        }
    }
    return NULL;
}

/* every value of the keys, joined with commas; S_FALSE when there is none */
HRESULT fwrule_get_values( const WCHAR *text, const WCHAR *const *keys, unsigned int count, WCHAR *out, size_t cch )
{
    unsigned int i;
    BOOL any = FALSE;

    if (!cch) return E_INVALIDARG;
    out[0] = 0;
    for (i = 0; i < count; i++)
    {
        const WCHAR *pos = NULL, *value;
        size_t len;

        while ((value = find_field( text, keys[i], &pos, &len )))
        {
            if (any && FAILED(StringCchCatW( out, cch, L"," ))) return E_OUTOFMEMORY;
            if (FAILED(StringCchCatNW( out, cch, value, len ))) return E_OUTOFMEMORY;
            any = TRUE;
        }
    }
    return any ? S_OK : S_FALSE;
}

HRESULT fwrule_get_value( const WCHAR *text, const WCHAR *key, WCHAR *out, size_t cch )
{
    const WCHAR *pos = NULL, *value;
    size_t len;

    if (!cch) return E_INVALIDARG;
    out[0] = 0;
    if (!(value = find_field( text, key, &pos, &len ))) return S_FALSE;
    return FAILED(StringCchCopyNW( out, cch, value, len )) ? E_OUTOFMEMORY : S_OK;
}

/* The text with every field of the keys removed, then "key=value" appended: one per value of
   the comma-separated list when list is TRUE, else the whole value (none when it is NULL or
   empty); a value may not hold '|' */
HRESULT fwrule_set_values( const WCHAR *text, const WCHAR *const *keys, unsigned int count,
                           const WCHAR *key, const WCHAR *values, BOOL list, WCHAR **result )
{
    /* each value may come with its key, '=' and '|' */
    size_t size = wcslen( text ) + (values && key ? (wcslen( values ) + 1) * (wcslen( key ) + 3) : 0) + 2;
    WCHAR *out;
    const WCHAR *p;

    *result = NULL;
    if (values && wcschr( values, '|' )) return E_INVALIDARG;
    if (!(out = malloc( size * sizeof(WCHAR) ))) return E_OUTOFMEMORY;

    /* the version, then the fields of other keys */
    p = wcschr( text, '|' );
    if (!p) { free( out ); return E_INVALIDARG; }
    memcpy( out, text, (p - text + 1) * sizeof(WCHAR) );
    out[p - text + 1] = 0;
    while (*++p)
    {
        const WCHAR *end = wcschr( p, '|' );
        size_t len = end ? (size_t)(end - p) : wcslen( p );
        unsigned int i;
        BOOL drop = FALSE;

        for (i = 0; i < count; i++)
        {
            size_t keylen = wcslen( keys[i] );
            if (len > keylen && !wcsncmp( p, keys[i], keylen ) && p[keylen] == '=') drop = TRUE;
        }
        if (!drop && len)
        {
            wcsncat( out, p, len );
            wcscat( out, L"|" );
        }
        if (!end) break;
        p = end;
    }

    /* the new values */
    if (key && values)
    {
        const WCHAR *v = values;
        while (*v)
        {
            const WCHAR *comma = list ? wcschr( v, ',' ) : NULL;
            size_t len = comma ? (size_t)(comma - v) : wcslen( v );
            while (len && *v == ' ') { v++; len--; }
            while (len && v[len - 1] == ' ') len--;
            if (len)
            {
                if (wcslen( out ) + wcslen( key ) + len + 3 > size) { free( out ); return E_OUTOFMEMORY; }
                wcscat( out, key );
                wcscat( out, L"=" );
                wcsncat( out, v, len );
                wcscat( out, L"|" );
            }
            if (!comma) break;
            v = comma + 1;
        }
    }
    *result = out;
    return S_OK;
}

/* a rule the service can read: the parser must accept it (Action= and Dir= set) */
BOOL fwrule_is_valid( const WCHAR *text )
{
    MPS_FIREWALL_RULE *rule = malloc( sizeof(*rule) );
    size_t len = wcslen( text );
    BOOL valid;

    if (!rule) return FALSE;
    valid = len <= MPS_MAX_RULE_CHARS && MpsParseFirewallRule( text, len, rule ) == ERROR_SUCCESS;
    free( rule );
    return valid;
}

const WCHAR *fwrule_default_text( void )
{
    return FW_NEW_RULE;
}

WCHAR *fw_strdup( const WCHAR *s )
{
    size_t size = (wcslen( s ) + 1) * sizeof(WCHAR);
    WCHAR *d = malloc( size );

    if (d) memcpy( d, s, size );
    return d;
}

/* ---- the rules in the registry ------------------------------------------------------------- */

static LSTATUS open_rules( REGSAM access, BOOL create, HKEY *key )
{
    if (create)
        return RegCreateKeyExW( HKEY_LOCAL_MACHINE, FW_RULES_KEY, 0, NULL, 0, access, NULL, key, NULL );
    return RegOpenKeyExW( HKEY_LOCAL_MACHINE, FW_RULES_KEY, 0, access, key );
}

/* the index-th rule (value) of the store: its id and its text, both malloc'ed */
HRESULT fwstore_rule_at( LONG index, WCHAR **id, WCHAR **text )
{
    WCHAR name[256];
    WCHAR *data;
    DWORD namelen = ARRAY_SIZE(name), size = (MPS_MAX_RULE_CHARS + 1) * sizeof(WCHAR), type;
    LSTATUS err;
    HKEY key;

    *id = *text = NULL;
    if ((err = open_rules( KEY_QUERY_VALUE, FALSE, &key ))) return HRESULT_FROM_WIN32( err );
    if (!(data = malloc( size + sizeof(WCHAR) ))) { RegCloseKey( key ); return E_OUTOFMEMORY; }
    err = RegEnumValueW( key, index, name, &namelen, NULL, &type, (BYTE *)data, &size );
    RegCloseKey( key );
    if (err || type != REG_SZ) { free( data ); return err ? HRESULT_FROM_WIN32( err ) : S_FALSE; }
    data[size / sizeof(WCHAR)] = 0;
    if (!(*id = fw_strdup( name ))) { free( data ); return E_OUTOFMEMORY; }
    *text = data;
    return S_OK;
}

/* the values of the store, rules or not: the bound of an index for fwstore_rule_at */
LONG fwstore_value_count( void )
{
    DWORD count = 0;
    HKEY key;

    if (open_rules( KEY_QUERY_VALUE, FALSE, &key )) return 0;
    RegQueryInfoKeyW( key, NULL, NULL, NULL, NULL, NULL, NULL, &count, NULL, NULL, NULL, NULL );
    RegCloseKey( key );
    return count;
}

/* the rules of the store: its string values, the ones fwstore_rule_at returns */
LONG fwstore_rule_count( void )
{
    WCHAR name[256];
    DWORD namelen, type, i;
    LONG count = 0;
    HKEY key;

    if (open_rules( KEY_QUERY_VALUE, FALSE, &key )) return 0;
    for (i = 0; ; i++)
    {
        LSTATUS err;

        namelen = ARRAY_SIZE(name);
        err = RegEnumValueW( key, i, name, &namelen, NULL, &type, NULL, NULL );
        if (err == ERROR_NO_MORE_ITEMS) break;
        if (!err && type == REG_SZ) count++;
    }
    RegCloseKey( key );
    return count;
}

/* the rule is still in the store (it may have been removed since it was read) */
BOOL fwstore_rule_exists( const WCHAR *id )
{
    DWORD type;
    LSTATUS err;
    HKEY key;

    if (open_rules( KEY_QUERY_VALUE, FALSE, &key )) return FALSE;
    err = RegQueryValueExW( key, id, NULL, &type, NULL, NULL );
    RegCloseKey( key );
    return !err && type == REG_SZ;
}

/* the first rule whose Name= is the name */
HRESULT fwstore_rule_find( const WCHAR *rulename, WCHAR **id, WCHAR **text )
{
    LONG i, count = fwstore_value_count();

    for (i = 0; i < count; i++)
    {
        WCHAR value[FW_MAX_NAME + 1];
        if (fwstore_rule_at( i, id, text ) != S_OK) continue;
        if (fwrule_get_value( *text, L"Name", value, ARRAY_SIZE(value) ) == S_OK && !lstrcmpiW( value, rulename ))
            return S_OK;
        free( *id );
        free( *text );
        *id = *text = NULL;
    }
    return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
}

HRESULT fwstore_rule_write( const WCHAR *id, const WCHAR *text )
{
    LSTATUS err;
    HKEY key;

    if (!fwrule_is_valid( text )) return E_INVALIDARG;
    if ((err = open_rules( KEY_SET_VALUE, TRUE, &key ))) return HRESULT_FROM_WIN32( err );
    err = RegSetValueExW( key, id, 0, REG_SZ, (const BYTE *)text, (wcslen( text ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
    return HRESULT_FROM_WIN32( err );
}

/* every rule with that Name= goes, as Windows removes them */
HRESULT fwstore_rule_remove( const WCHAR *rulename )
{
    WCHAR *id, *text;
    HKEY key;
    LSTATUS err;

    if ((err = open_rules( KEY_SET_VALUE | KEY_QUERY_VALUE, FALSE, &key )))
        return err == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32( err );
    while (fwstore_rule_find( rulename, &id, &text ) == S_OK)
    {
        err = RegDeleteValueW( key, id );
        free( id );
        free( text );
        if (err) break;
    }
    RegCloseKey( key );
    return HRESULT_FROM_WIN32( err );
}

/* a new rule id, a GUID in braces as Windows uses */
HRESULT fwstore_new_id( WCHAR **id )
{
    GUID guid;

    if (FAILED(CoCreateGuid( &guid ))) return E_FAIL;
    if (!(*id = malloc( 40 * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    StringFromGUID2( &guid, *id, 40 );
    return S_OK;
}

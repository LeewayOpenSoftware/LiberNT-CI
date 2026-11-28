/*
 * Copyright 2009 Hans Leidekker for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdio.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"
#include "netfw.h"

#include "wine/debug.h"
#include "hnetcfg_private.h"
#include "mpssvc_policy.h"

WINE_DEFAULT_DEBUG_CHANNEL(hnetcfg);

typedef struct fw_policy
{
    INetFwPolicy INetFwPolicy_iface;
    LONG refs;
} fw_policy;

static inline fw_policy *impl_from_INetFwPolicy( INetFwPolicy *iface )
{
    return CONTAINING_RECORD(iface, fw_policy, INetFwPolicy_iface);
}

typedef struct fw_policy2
{
    INetFwPolicy2 INetFwPolicy2_iface;
    INetFwRules   *fw_policy2_rules;
    LONG refs;
} fw_policy2;

static inline fw_policy2 *impl_from_INetFwPolicy2( INetFwPolicy2 *iface )
{
    return CONTAINING_RECORD(iface, fw_policy2, INetFwPolicy2_iface);
}

typedef struct fw_rule
{
    INetFwRule INetFwRule_iface;
    LONG refs;
    WCHAR *text;    /* the Windows rule string (fwstore.c) */
    WCHAR *id;      /* its value name in the store, NULL until the rule is added */
} fw_rule;

static inline fw_rule *impl_from_INetFwRule( INetFwRule *iface )
{
    return CONTAINING_RECORD(iface, fw_rule, INetFwRule_iface);
}

static HRESULT WINAPI netfw_rule_QueryInterface(
    INetFwRule *iface,
    REFIID riid,
    void **object)
{
    fw_rule *This = impl_from_INetFwRule( iface );

    TRACE("%p %s %p\n", This, debugstr_guid( riid ), object );

    if ( IsEqualGUID( riid, &IID_INetFwRule ) ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *object = iface;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    INetFwRule_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI netfw_rule_AddRef(
    INetFwRule *iface )
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return InterlockedIncrement( &This->refs );
}

static ULONG WINAPI netfw_rule_Release(
    INetFwRule *iface )
{
    fw_rule *This = impl_from_INetFwRule( iface );
    LONG refs = InterlockedDecrement( &This->refs );
    if (!refs)
    {
        TRACE("destroying %p\n", This);
        free( This->text );
        free( This->id );
        free( This );
    }
    return refs;
}

static HRESULT WINAPI netfw_rule_GetTypeInfoCount(
    INetFwRule *iface,
    UINT *pctinfo )
{
    fw_rule *This = impl_from_INetFwRule( iface );

    TRACE("%p %p\n", This, pctinfo);
    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI netfw_rule_GetTypeInfo(
    INetFwRule *iface,
    UINT iTInfo,
    LCID lcid,
    ITypeInfo **ppTInfo)
{
    fw_rule *This = impl_from_INetFwRule( iface );

    TRACE("%p %u %lu %p\n", This, iTInfo, lcid, ppTInfo);
    return get_typeinfo( INetFwRule_tid, ppTInfo );
}

static HRESULT WINAPI netfw_rule_GetIDsOfNames(
    INetFwRule *iface,
    REFIID riid,
    LPOLESTR *rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID *rgDispId)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("%p %s %p %u %lu %p\n", This, debugstr_guid(riid), rgszNames, cNames, lcid, rgDispId);

    hr = get_typeinfo( INetFwRule_tid, &typeinfo );
    if (SUCCEEDED(hr))
    {
        hr = ITypeInfo_GetIDsOfNames( typeinfo, rgszNames, cNames, rgDispId );
        ITypeInfo_Release( typeinfo );
    }
    return hr;
}

static HRESULT WINAPI netfw_rule_Invoke(
    INetFwRule *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS *pDispParams,
    VARIANT *pVarResult,
    EXCEPINFO *pExcepInfo,
    UINT *puArgErr)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("%p %ld %s %ld %d %p %p %p %p\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo( INetFwRule_tid, &typeinfo );
    if (SUCCEEDED(hr))
    {
        hr = ITypeInfo_Invoke( typeinfo, &This->INetFwRule_iface, dispIdMember,
                               wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr );
        ITypeInfo_Release( typeinfo );
    }
    return hr;
}

/* ---- the fields of the rule string (LiberNT: fwstore.c) ---- */

#define FW_PROTOCOL_ANY 256 /* NET_FW_IP_PROTOCOL_ANY */

static const WCHAR *const key_name[] = { L"Name" };
static const WCHAR *const key_desc[] = { L"Desc" };
static const WCHAR *const key_app[] = { L"App" };
static const WCHAR *const key_svc[] = { L"Svc" };
static const WCHAR *const key_protocol[] = { L"Protocol" };
static const WCHAR *const key_lport[] = { L"LPort", L"LPort2_10", L"LPort2_20" };
static const WCHAR *const key_rport[] = { L"RPort", L"RPort2_10" };
static const WCHAR *const key_laddr[] = { L"LA4", L"LA6" };
static const WCHAR *const key_raddr[] = { L"RA4", L"RA6" };
static const WCHAR *const key_icmp[] = { L"ICMP4", L"ICMP6" };
static const WCHAR *const key_dir[] = { L"Dir" };
static const WCHAR *const key_active[] = { L"Active" };
static const WCHAR *const key_group[] = { L"EmbedCtxt" };
static const WCHAR *const key_profile[] = { L"Profile" };
static const WCHAR *const key_action[] = { L"Action" };

/* the values of the keys as a BSTR; an absent field reads as dflt (NULL: an empty string) */
static HRESULT rule_get_bstr( fw_rule *rule, const WCHAR *const *keys, unsigned int count, const WCHAR *dflt, BSTR *out )
{
    WCHAR buffer[MPS_MAX_RULE_CHARS + 1];
    HRESULT hr;

    if (!out) return E_POINTER;
    hr = fwrule_get_values( rule->text, keys, count, buffer, ARRAY_SIZE(buffer) );
    if (FAILED(hr)) return hr;
    *out = SysAllocString( hr == S_FALSE ? (dflt ? dflt : L"") : buffer );
    return *out ? S_OK : E_OUTOFMEMORY;
}

/* The fields of the keys become the comma-separated values under key (none: removed). A rule
   of the store is written back at once, and only when the service can read it */
static HRESULT rule_update( fw_rule *rule, const WCHAR *const *keys, unsigned int count, const WCHAR *key, const WCHAR *values,
                            BOOL list )
{
    WCHAR *text;
    HRESULT hr = fwrule_set_values( rule->text, keys, count, key, values, list, &text );

    if (FAILED(hr)) return hr;
    /* a rule removed from the store meanwhile is not written back, as Windows */
    if (rule->id && !fwstore_rule_exists( rule->id ))
    {
        free( text );
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    }
    if (rule->id && FAILED(hr = fwstore_rule_write( rule->id, text )))
    {
        free( text );
        return hr;
    }
    free( rule->text );
    rule->text = text;
    return S_OK;
}

/* one value: a comma in it is part of it */
static HRESULT rule_put_bstr( fw_rule *rule, const WCHAR *const *keys, unsigned int count, BSTR value )
{
    return rule_update( rule, keys, count, keys[0], (value && *value) ? value : NULL, FALSE );
}

static HRESULT WINAPI netfw_rule_get_Name(
    INetFwRule *iface,
    BSTR *name)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_name, ARRAY_SIZE(key_name), NULL, name );
}

static HRESULT WINAPI netfw_rule_put_Name(
    INetFwRule *iface,
    BSTR name)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    if (!name || !*name || wcslen( name ) > FW_MAX_NAME) return E_INVALIDARG;
    return rule_put_bstr( This, key_name, ARRAY_SIZE(key_name), name );
}

static HRESULT WINAPI netfw_rule_get_Description(
    INetFwRule *iface,
    BSTR *desc)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_desc, ARRAY_SIZE(key_desc), NULL, desc );
}

static HRESULT WINAPI netfw_rule_put_Description(
    INetFwRule *iface,
    BSTR desc)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_put_bstr( This, key_desc, ARRAY_SIZE(key_desc), desc );
}

static HRESULT WINAPI netfw_rule_get_ApplicationName(
    INetFwRule *iface,
    BSTR *app)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_app, ARRAY_SIZE(key_app), NULL, app );
}

static HRESULT WINAPI netfw_rule_put_ApplicationName(
    INetFwRule *iface,
    BSTR app)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_put_bstr( This, key_app, ARRAY_SIZE(key_app), app );
}

static HRESULT WINAPI netfw_rule_get_ServiceName(
    INetFwRule *iface,
    BSTR *service)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_svc, ARRAY_SIZE(key_svc), NULL, service );
}

static HRESULT WINAPI netfw_rule_put_ServiceName(
    INetFwRule *iface,
    BSTR service)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_put_bstr( This, key_svc, ARRAY_SIZE(key_svc), service );
}

static HRESULT WINAPI netfw_rule_get_Protocol(
    INetFwRule *iface,
    LONG *protocol)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    WCHAR value[16];

    if (!protocol) return E_POINTER;
    *protocol = FW_PROTOCOL_ANY;
    if (fwrule_get_value( This->text, L"Protocol", value, ARRAY_SIZE(value) ) == S_OK)
        *protocol = wcstol( value, NULL, 10 );
    return S_OK;
}

static HRESULT WINAPI netfw_rule_put_Protocol(
    INetFwRule *iface,
    LONG protocol)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    WCHAR value[16];

    if (protocol < 0 || protocol > FW_PROTOCOL_ANY) return E_INVALIDARG;
    if (protocol == FW_PROTOCOL_ANY)
        return rule_update( This, key_protocol, ARRAY_SIZE(key_protocol), NULL, NULL, FALSE );
    swprintf( value, ARRAY_SIZE(value), L"%ld", protocol );
    return rule_update( This, key_protocol, ARRAY_SIZE(key_protocol), L"Protocol", value, FALSE );
}

/* ports and addresses: "*" (any) is no field at all */
static HRESULT rule_put_list( fw_rule *rule, const WCHAR *const *keys, unsigned int count, const WCHAR *key, BSTR list )
{
    if (!list || !*list || !wcscmp( list, L"*" ))
        return rule_update( rule, keys, count, NULL, NULL, FALSE );
    return rule_update( rule, keys, count, key, list, TRUE );
}

static HRESULT WINAPI netfw_rule_get_LocalPorts(
    INetFwRule *iface,
    BSTR *ports)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_lport, ARRAY_SIZE(key_lport), L"*", ports );
}

static HRESULT WINAPI netfw_rule_put_LocalPorts(
    INetFwRule *iface,
    BSTR ports)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_put_list( This, key_lport, ARRAY_SIZE(key_lport), L"LPort", ports );
}

static HRESULT WINAPI netfw_rule_get_RemotePorts(
    INetFwRule *iface,
    BSTR *ports)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_rport, ARRAY_SIZE(key_rport), L"*", ports );
}

static HRESULT WINAPI netfw_rule_put_RemotePorts(
    INetFwRule *iface,
    BSTR ports)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_put_list( This, key_rport, ARRAY_SIZE(key_rport), L"RPort", ports );
}

static HRESULT WINAPI netfw_rule_get_LocalAddresses(
    INetFwRule *iface,
    BSTR *addresses)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_laddr, ARRAY_SIZE(key_laddr), L"*", addresses );
}

static HRESULT WINAPI netfw_rule_put_LocalAddresses(
    INetFwRule *iface,
    BSTR addresses)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    if (addresses && wcschr( addresses, ':' )) return E_NOTIMPL; /* IPv4 only for now */
    return rule_put_list( This, key_laddr, ARRAY_SIZE(key_laddr), L"LA4", addresses );
}

static HRESULT WINAPI netfw_rule_get_RemoteAddresses(
    INetFwRule *iface,
    BSTR *addresses)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_raddr, ARRAY_SIZE(key_raddr), L"*", addresses );
}

static HRESULT WINAPI netfw_rule_put_RemoteAddresses(
    INetFwRule *iface,
    BSTR addresses)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    if (addresses && wcschr( addresses, ':' )) return E_NOTIMPL; /* IPv4 only for now */
    return rule_put_list( This, key_raddr, ARRAY_SIZE(key_raddr), L"RA4", addresses );
}

static HRESULT WINAPI netfw_rule_get_IcmpTypesAndCodes(
    INetFwRule *iface,
    BSTR *codes)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_icmp, ARRAY_SIZE(key_icmp), L"*", codes );
}

static HRESULT WINAPI netfw_rule_put_IcmpTypesAndCodes(
    INetFwRule *iface,
    BSTR codes)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_put_list( This, key_icmp, ARRAY_SIZE(key_icmp), L"ICMP4", codes );
}

static HRESULT WINAPI netfw_rule_get_Direction(
    INetFwRule *iface,
    NET_FW_RULE_DIRECTION *dir)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    WCHAR value[8];

    if (!dir) return E_POINTER;
    *dir = NET_FW_RULE_DIR_IN;
    if (fwrule_get_value( This->text, L"Dir", value, ARRAY_SIZE(value) ) == S_OK && !lstrcmpiW( value, L"Out" ))
        *dir = NET_FW_RULE_DIR_OUT;
    return S_OK;
}

static HRESULT WINAPI netfw_rule_put_Direction(
    INetFwRule *iface,
    NET_FW_RULE_DIRECTION dir)
{
    fw_rule *This = impl_from_INetFwRule( iface );

    if (dir != NET_FW_RULE_DIR_IN && dir != NET_FW_RULE_DIR_OUT) return E_INVALIDARG;
    return rule_update( This, key_dir, ARRAY_SIZE(key_dir), L"Dir", dir == NET_FW_RULE_DIR_IN ? L"In" : L"Out", FALSE );
}

static HRESULT WINAPI netfw_rule_get_Interfaces(
    INetFwRule *iface,
    VARIANT *interfaces)
{
    if (!interfaces) return E_POINTER;
    V_VT( interfaces ) = VT_EMPTY; /* every interface */
    return S_OK;
}

static HRESULT WINAPI netfw_rule_put_Interfaces(
    INetFwRule *iface,
    VARIANT interfaces)
{
    return V_VT( &interfaces ) == VT_EMPTY ? S_OK : E_NOTIMPL;
}

static HRESULT WINAPI netfw_rule_get_InterfaceTypes(
    INetFwRule *iface,
    BSTR *types)
{
    if (!types) return E_POINTER;
    *types = SysAllocString( L"All" );
    return *types ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI netfw_rule_put_InterfaceTypes(
    INetFwRule *iface,
    BSTR types)
{
    return (types && !lstrcmpiW( types, L"All" )) ? S_OK : E_NOTIMPL;
}

static HRESULT WINAPI netfw_rule_get_Enabled(
    INetFwRule *iface,
    VARIANT_BOOL *enabled)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    WCHAR value[8];

    if (!enabled) return E_POINTER;
    *enabled = (fwrule_get_value( This->text, L"Active", value, ARRAY_SIZE(value) ) == S_OK &&
                !lstrcmpiW( value, L"TRUE" )) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT WINAPI netfw_rule_put_Enabled(
    INetFwRule *iface,
    VARIANT_BOOL enabled)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_update( This, key_active, ARRAY_SIZE(key_active), L"Active", enabled ? L"TRUE" : L"FALSE", FALSE );
}

static HRESULT WINAPI netfw_rule_get_Grouping(
    INetFwRule *iface,
    BSTR *context)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_get_bstr( This, key_group, ARRAY_SIZE(key_group), NULL, context );
}

static HRESULT WINAPI netfw_rule_put_Grouping(
    INetFwRule *iface,
    BSTR context)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    return rule_put_bstr( This, key_group, ARRAY_SIZE(key_group), context );
}

static HRESULT WINAPI netfw_rule_get_Profiles(
    INetFwRule *iface,
    LONG *profiles)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    WCHAR value[64];

    if (!profiles) return E_POINTER;
    *profiles = 0;
    if (fwrule_get_values( This->text, key_profile, ARRAY_SIZE(key_profile), value, ARRAY_SIZE(value) ) != S_OK)
    {
        *profiles = NET_FW_PROFILE2_ALL;
        return S_OK;
    }
    if (wcsstr( value, L"Domain" )) *profiles |= NET_FW_PROFILE2_DOMAIN;
    if (wcsstr( value, L"Private" )) *profiles |= NET_FW_PROFILE2_PRIVATE;
    if (wcsstr( value, L"Public" )) *profiles |= NET_FW_PROFILE2_PUBLIC;
    return S_OK;
}

static HRESULT WINAPI netfw_rule_put_Profiles(
    INetFwRule *iface,
    LONG profiles)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    WCHAR value[64] = L"";

    if (!(profiles & (NET_FW_PROFILE2_DOMAIN | NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_PUBLIC)))
        return E_INVALIDARG;
    if ((profiles & NET_FW_PROFILE2_ALL) == NET_FW_PROFILE2_ALL ||
        (profiles & 7) == 7)
        return rule_update( This, key_profile, ARRAY_SIZE(key_profile), NULL, NULL, FALSE );
    if (profiles & NET_FW_PROFILE2_DOMAIN) wcscat( value, L"Domain," );
    if (profiles & NET_FW_PROFILE2_PRIVATE) wcscat( value, L"Private," );
    if (profiles & NET_FW_PROFILE2_PUBLIC) wcscat( value, L"Public," );
    return rule_update( This, key_profile, ARRAY_SIZE(key_profile), L"Profile", value, TRUE );
}

static HRESULT WINAPI netfw_rule_get_EdgeTraversal(
    INetFwRule *iface,
    VARIANT_BOOL *enabled)
{
    if (!enabled) return E_POINTER;
    *enabled = VARIANT_FALSE;
    return S_OK;
}

static HRESULT WINAPI netfw_rule_put_EdgeTraversal(
    INetFwRule *iface,
    VARIANT_BOOL enabled)
{
    return enabled ? E_NOTIMPL : S_OK;
}

static HRESULT WINAPI netfw_rule_get_Action(
    INetFwRule *iface,
    NET_FW_ACTION *action)
{
    fw_rule *This = impl_from_INetFwRule( iface );
    WCHAR value[16];

    if (!action) return E_POINTER;
    *action = NET_FW_ACTION_ALLOW;
    if (fwrule_get_value( This->text, L"Action", value, ARRAY_SIZE(value) ) == S_OK && !lstrcmpiW( value, L"Block" ))
        *action = NET_FW_ACTION_BLOCK;
    return S_OK;
}

static HRESULT WINAPI netfw_rule_put_Action(
    INetFwRule *iface,
    NET_FW_ACTION action)
{
    fw_rule *This = impl_from_INetFwRule( iface );

    if (action != NET_FW_ACTION_ALLOW && action != NET_FW_ACTION_BLOCK) return E_INVALIDARG;
    return rule_update( This, key_action, ARRAY_SIZE(key_action), L"Action",
                        action == NET_FW_ACTION_ALLOW ? L"Allow" : L"Block", FALSE );
}

static const struct INetFwRuleVtbl fw_rule_vtbl =
{
    netfw_rule_QueryInterface,
    netfw_rule_AddRef,
    netfw_rule_Release,
    netfw_rule_GetTypeInfoCount,
    netfw_rule_GetTypeInfo,
    netfw_rule_GetIDsOfNames,
    netfw_rule_Invoke,
    netfw_rule_get_Name,
    netfw_rule_put_Name,
    netfw_rule_get_Description,
    netfw_rule_put_Description,
    netfw_rule_get_ApplicationName,
    netfw_rule_put_ApplicationName,
    netfw_rule_get_ServiceName,
    netfw_rule_put_ServiceName,
    netfw_rule_get_Protocol,
    netfw_rule_put_Protocol,
    netfw_rule_get_LocalPorts,
    netfw_rule_put_LocalPorts,
    netfw_rule_get_RemotePorts,
    netfw_rule_put_RemotePorts,
    netfw_rule_get_LocalAddresses,
    netfw_rule_put_LocalAddresses,
    netfw_rule_get_RemoteAddresses,
    netfw_rule_put_RemoteAddresses,
    netfw_rule_get_IcmpTypesAndCodes,
    netfw_rule_put_IcmpTypesAndCodes,
    netfw_rule_get_Direction,
    netfw_rule_put_Direction,
    netfw_rule_get_Interfaces,
    netfw_rule_put_Interfaces,
    netfw_rule_get_InterfaceTypes,
    netfw_rule_put_InterfaceTypes,
    netfw_rule_get_Enabled,
    netfw_rule_put_Enabled,
    netfw_rule_get_Grouping,
    netfw_rule_put_Grouping,
    netfw_rule_get_Profiles,
    netfw_rule_put_Profiles,
    netfw_rule_get_EdgeTraversal,
    netfw_rule_put_EdgeTraversal,
    netfw_rule_get_Action,
    netfw_rule_put_Action,
};

HRESULT NetFwRule_create( IUnknown *outer, void **obj )
{
    fw_rule *rule;

    TRACE("(%p)\n", obj);

    rule = malloc( sizeof(*rule) );
    if (!rule) return E_OUTOFMEMORY;

    rule->INetFwRule_iface.lpVtbl = &fw_rule_vtbl;
    rule->refs = 1;
    rule->id = NULL;
    if (!(rule->text = fw_strdup( fwrule_default_text() )))
    {
        free( rule );
        return E_OUTOFMEMORY;
    }

    *obj = &rule->INetFwRule_iface;

    TRACE("returning iface %p\n", *obj);
    return S_OK;
}

/* a rule of the store; takes id and text */
static HRESULT stored_rule_create( WCHAR *id, WCHAR *text, INetFwRule **out )
{
    fw_rule *rule;
    HRESULT hr = NetFwRule_create( NULL, (void **)out );

    if (FAILED(hr))
    {
        free( id );
        free( text );
        return hr;
    }
    rule = impl_from_INetFwRule( *out );
    free( rule->text );
    rule->text = text;
    rule->id = id;
    return S_OK;
}

typedef struct fw_rules
{
    INetFwRules INetFwRules_iface;
    LONG refs;
} fw_rules;

static inline fw_rules *impl_from_INetFwRules( INetFwRules *iface )
{
    return CONTAINING_RECORD(iface, fw_rules, INetFwRules_iface);
}

static HRESULT WINAPI netfw_rules_QueryInterface(
    INetFwRules *iface,
    REFIID riid,
    void **object)
{
    fw_rules *This = impl_from_INetFwRules( iface );

    TRACE("%p %s %p\n", This, debugstr_guid( riid ), object );

    if ( IsEqualGUID( riid, &IID_INetFwRules ) ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *object = iface;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    INetFwRules_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI netfw_rules_AddRef(
    INetFwRules *iface )
{
    fw_rules *This = impl_from_INetFwRules( iface );
    return InterlockedIncrement( &This->refs );
}

static ULONG WINAPI netfw_rules_Release(
    INetFwRules *iface )
{
    fw_rules *This = impl_from_INetFwRules( iface );
    LONG refs = InterlockedDecrement( &This->refs );
    if (!refs)
    {
        TRACE("destroying %p\n", This);
        free( This );
    }
    return refs;
}

static HRESULT WINAPI netfw_rules_GetTypeInfoCount(
    INetFwRules *iface,
    UINT *pctinfo )
{
    fw_rules *This = impl_from_INetFwRules( iface );

    TRACE("%p %p\n", This, pctinfo);
    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI netfw_rules_GetTypeInfo(
    INetFwRules *iface,
    UINT iTInfo,
    LCID lcid,
    ITypeInfo **ppTInfo)
{
    fw_rules *This = impl_from_INetFwRules( iface );

    TRACE("%p %u %lu %p\n", This, iTInfo, lcid, ppTInfo);
    return get_typeinfo( INetFwRules_tid, ppTInfo );
}

static HRESULT WINAPI netfw_rules_GetIDsOfNames(
    INetFwRules *iface,
    REFIID riid,
    LPOLESTR *rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID *rgDispId)
{
    fw_rules *This = impl_from_INetFwRules( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("%p %s %p %u %lu %p\n", This, debugstr_guid(riid), rgszNames, cNames, lcid, rgDispId);

    hr = get_typeinfo( INetFwRules_tid, &typeinfo );
    if (SUCCEEDED(hr))
    {
        hr = ITypeInfo_GetIDsOfNames( typeinfo, rgszNames, cNames, rgDispId );
        ITypeInfo_Release( typeinfo );
    }
    return hr;
}

static HRESULT WINAPI netfw_rules_Invoke(
    INetFwRules *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS *pDispParams,
    VARIANT *pVarResult,
    EXCEPINFO *pExcepInfo,
    UINT *puArgErr)
{
    fw_rules *This = impl_from_INetFwRules( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("%p %ld %s %ld %d %p %p %p %p\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo( INetFwRules_tid, &typeinfo );
    if (SUCCEEDED(hr))
    {
        hr = ITypeInfo_Invoke( typeinfo, &This->INetFwRules_iface, dispIdMember,
                               wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr );
        ITypeInfo_Release( typeinfo );
    }
    return hr;
}

static HRESULT WINAPI netfw_rules_get_Count(
    INetFwRules *iface,
    LONG *count)
{
    fw_rules *This = impl_from_INetFwRules( iface );

    TRACE("%p, %p\n", This, count);

    if (!count) return E_POINTER;
    *count = fwstore_rule_count();
    return S_OK;
}

static HRESULT WINAPI netfw_rules_Add(
    INetFwRules *iface,
    INetFwRule *rule)
{
    fw_rules *This = impl_from_INetFwRules( iface );
    fw_rule *data;
    WCHAR *id;
    HRESULT hr;
    BSTR name;

    TRACE("%p, %p\n", This, rule);

    if (!rule) return E_POINTER;
    /* a rule needs a name, as in Windows */
    if (FAILED(INetFwRule_get_Name( rule, &name ))) return E_INVALIDARG;
    hr = SysStringLen( name ) ? S_OK : E_INVALIDARG;
    SysFreeString( name );
    if (FAILED(hr)) return hr;

    data = impl_from_INetFwRule( rule );
    if (data->INetFwRule_iface.lpVtbl != &fw_rule_vtbl) return E_INVALIDARG;
    if (data->id) return HRESULT_FROM_WIN32( ERROR_ALREADY_EXISTS );
    if (FAILED(hr = fwstore_new_id( &id ))) return hr;
    if (FAILED(hr = fwstore_rule_write( id, data->text )))
    {
        free( id );
        return hr;
    }
    data->id = id; /* from now on, every change to the rule is written */
    return S_OK;
}

static HRESULT WINAPI netfw_rules_Remove(
    INetFwRules *iface,
    BSTR name)
{
    fw_rules *This = impl_from_INetFwRules( iface );

    TRACE("%p, %s\n", This, debugstr_w(name));

    if (!name) return E_INVALIDARG;
    return fwstore_rule_remove( name );
}

static HRESULT WINAPI netfw_rules_Item(
    INetFwRules *iface,
    BSTR name,
    INetFwRule **rule)
{
    fw_rules *This = impl_from_INetFwRules( iface );
    WCHAR *id, *text;
    HRESULT hr;

    TRACE("%p, %s, %p\n", This, debugstr_w(name), rule);

    if (!rule) return E_POINTER;
    *rule = NULL;
    if (!name) return E_INVALIDARG;
    if (FAILED(hr = fwstore_rule_find( name, &id, &text ))) return hr;
    return stored_rule_create( id, text, rule );
}

static HRESULT NewEnum_create(fw_rules *rules, IUnknown **obj);

typedef struct
{
    IEnumVARIANT IEnumVARIANT_iface;
    LONG ref, pos;
    fw_rules *rules;
} EnumVARIANT;

static inline EnumVARIANT *impl_from_IEnumVARIANT(IEnumVARIANT *iface)
{
    return CONTAINING_RECORD(iface, EnumVARIANT, IEnumVARIANT_iface);
}

static HRESULT WINAPI enumvar_QueryInterface(IEnumVARIANT *iface, REFIID riid, void **obj)
{
    if (!riid || !obj) return E_INVALIDARG;

    TRACE("%p,%s,%p\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IEnumVARIANT) ||
        IsEqualGUID(riid, &IID_IUnknown))
    {
        IEnumVARIANT_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI enumvar_AddRef(IEnumVARIANT *iface)
{
    EnumVARIANT *enumvar = impl_from_IEnumVARIANT(iface);
    return InterlockedIncrement(&enumvar->ref);
}

static ULONG WINAPI enumvar_Release(IEnumVARIANT *iface)
{
    EnumVARIANT *enumvar = impl_from_IEnumVARIANT(iface);
    LONG ref = InterlockedDecrement(&enumvar->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);
        INetFwRules_Release(&enumvar->rules->INetFwRules_iface);
        free(enumvar);
    }

    return ref;
}

/* the rules of the store, in their order there */
static HRESULT WINAPI enumvar_Next(IEnumVARIANT *iface, ULONG celt, VARIANT *var, ULONG *fetched)
{
    EnumVARIANT *enumvar = impl_from_IEnumVARIANT(iface);
    ULONG got = 0;

    TRACE("%p,%lu,%p,%p\n", iface, celt, var, fetched);

    if (!var) return E_POINTER;
    while (got < celt && enumvar->pos < fwstore_value_count())
    {
        WCHAR *id, *text;
        INetFwRule *rule;
        HRESULT hr = fwstore_rule_at( enumvar->pos++, &id, &text );

        if (hr != S_OK) continue;
        if (FAILED(hr = stored_rule_create( id, text, &rule ))) break;
        V_VT( &var[got] ) = VT_DISPATCH;
        V_DISPATCH( &var[got] ) = (IDispatch *)rule;
        got++;
    }
    if (fetched) *fetched = got;
    return got == celt ? S_OK : S_FALSE;
}

static HRESULT WINAPI enumvar_Skip(IEnumVARIANT *iface, ULONG celt)
{
    EnumVARIANT *enumvar = impl_from_IEnumVARIANT(iface);
    LONG count = fwstore_value_count();

    TRACE("%p,%lu\n", iface, celt);

    /* rules are skipped, not the other values of the store */
    while (celt && enumvar->pos < count)
    {
        WCHAR *id, *text;

        if (fwstore_rule_at( enumvar->pos++, &id, &text ) != S_OK) continue;
        free( id );
        free( text );
        celt--;
    }
    return celt ? S_FALSE : S_OK;
}

static HRESULT WINAPI enumvar_Reset(IEnumVARIANT *iface)
{
    EnumVARIANT *enumvar = impl_from_IEnumVARIANT(iface);

    TRACE("%p\n", iface);

    enumvar->pos = 0;

    return S_OK;
}

static HRESULT WINAPI enumvar_Clone(IEnumVARIANT *iface, IEnumVARIANT **penum)
{
    EnumVARIANT *enumvar = impl_from_IEnumVARIANT(iface);

    FIXME("%p,%p\n", iface, penum);

    return NewEnum_create(enumvar->rules, (IUnknown **)penum);
}

static const struct IEnumVARIANTVtbl EnumVARIANT_vtbl =
{
    enumvar_QueryInterface,
    enumvar_AddRef,
    enumvar_Release,
    enumvar_Next,
    enumvar_Skip,
    enumvar_Reset,
    enumvar_Clone
};

static HRESULT NewEnum_create(fw_rules *rules, IUnknown **obj)
{
    EnumVARIANT *enumvar;

    enumvar = malloc(sizeof(*enumvar));
    if (!enumvar) return E_OUTOFMEMORY;

    enumvar->IEnumVARIANT_iface.lpVtbl = &EnumVARIANT_vtbl;
    enumvar->ref = 1;
    enumvar->pos = 0;
    enumvar->rules = rules;
    INetFwRules_AddRef(&rules->INetFwRules_iface);

    *obj = (IUnknown *)&enumvar->IEnumVARIANT_iface;

    TRACE("created %p\n", *obj);

    return S_OK;
}

static HRESULT WINAPI netfw_rules_get__NewEnum(
    INetFwRules *iface,
    IUnknown **newEnum)
{
    fw_rules *This = impl_from_INetFwRules( iface );

    TRACE("%p, %p\n", This, newEnum);

    if (!newEnum) return E_POINTER;

    return NewEnum_create(This, newEnum);
}

static const struct INetFwRulesVtbl fw_rules_vtbl =
{
    netfw_rules_QueryInterface,
    netfw_rules_AddRef,
    netfw_rules_Release,
    netfw_rules_GetTypeInfoCount,
    netfw_rules_GetTypeInfo,
    netfw_rules_GetIDsOfNames,
    netfw_rules_Invoke,
    netfw_rules_get_Count,
    netfw_rules_Add,
    netfw_rules_Remove,
    netfw_rules_Item,
    netfw_rules_get__NewEnum
};

static HRESULT create_INetFwRules(INetFwRules **object)
{
    fw_rules *rules;

    TRACE("(%p)\n", object);

    rules = malloc( sizeof(*rules) );
    if (!rules) return E_OUTOFMEMORY;

    rules->INetFwRules_iface.lpVtbl = &fw_rules_vtbl;
    rules->refs = 1;

    *object = &rules->INetFwRules_iface;

    TRACE("returning iface %p\n", *object);
    return S_OK;
}

static ULONG WINAPI fw_policy_AddRef(
    INetFwPolicy *iface )
{
    fw_policy *fw_policy = impl_from_INetFwPolicy( iface );
    return InterlockedIncrement( &fw_policy->refs );
}

static ULONG WINAPI fw_policy_Release(
    INetFwPolicy *iface )
{
    fw_policy *fw_policy = impl_from_INetFwPolicy( iface );
    LONG refs = InterlockedDecrement( &fw_policy->refs );
    if (!refs)
    {
        TRACE("destroying %p\n", fw_policy);
        free( fw_policy );
    }
    return refs;
}

static HRESULT WINAPI fw_policy_QueryInterface(
    INetFwPolicy *iface,
    REFIID riid,
    void **ppvObject )
{
    fw_policy *This = impl_from_INetFwPolicy( iface );

    TRACE("%p %s %p\n", This, debugstr_guid( riid ), ppvObject );

    if ( IsEqualGUID( riid, &IID_INetFwPolicy ) ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *ppvObject = iface;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    INetFwPolicy_AddRef( iface );
    return S_OK;
}

static HRESULT WINAPI fw_policy_GetTypeInfoCount(
    INetFwPolicy *iface,
    UINT *pctinfo )
{
    fw_policy *This = impl_from_INetFwPolicy( iface );

    TRACE("%p %p\n", This, pctinfo);
    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI fw_policy_GetTypeInfo(
    INetFwPolicy *iface,
    UINT iTInfo,
    LCID lcid,
    ITypeInfo **ppTInfo )
{
    fw_policy *This = impl_from_INetFwPolicy( iface );

    TRACE("%p %u %lu %p\n", This, iTInfo, lcid, ppTInfo);
    return get_typeinfo( INetFwPolicy_tid, ppTInfo );
}

static HRESULT WINAPI fw_policy_GetIDsOfNames(
    INetFwPolicy *iface,
    REFIID riid,
    LPOLESTR *rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID *rgDispId )
{
    fw_policy *This = impl_from_INetFwPolicy( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("%p %s %p %u %lu %p\n", This, debugstr_guid(riid), rgszNames, cNames, lcid, rgDispId);

    hr = get_typeinfo( INetFwPolicy_tid, &typeinfo );
    if (SUCCEEDED(hr))
    {
        hr = ITypeInfo_GetIDsOfNames( typeinfo, rgszNames, cNames, rgDispId );
        ITypeInfo_Release( typeinfo );
    }
    return hr;
}

static HRESULT WINAPI fw_policy_Invoke(
    INetFwPolicy *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS *pDispParams,
    VARIANT *pVarResult,
    EXCEPINFO *pExcepInfo,
    UINT *puArgErr )
{
    fw_policy *This = impl_from_INetFwPolicy( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("%p %ld %s %ld %d %p %p %p %p\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo( INetFwPolicy_tid, &typeinfo );
    if (SUCCEEDED(hr))
    {
        hr = ITypeInfo_Invoke( typeinfo, &This->INetFwPolicy_iface, dispIdMember,
                               wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr );
        ITypeInfo_Release( typeinfo );
    }
    return hr;
}

static HRESULT WINAPI fw_policy_get_CurrentProfile(
    INetFwPolicy *iface,
    INetFwProfile **profile )
{
    fw_policy *This = impl_from_INetFwPolicy( iface );

    TRACE("%p, %p\n", This, profile);
    return NetFwProfile_create( NULL, (void **)profile );
}

static HRESULT WINAPI fw_policy_GetProfileByType(
    INetFwPolicy *iface,
    NET_FW_PROFILE_TYPE profileType,
    INetFwProfile **profile )
{
    fw_policy *This = impl_from_INetFwPolicy( iface );

    FIXME("%p, %u, %p\n", This, profileType, profile);
    return E_NOTIMPL;
}

static const struct INetFwPolicyVtbl fw_policy_vtbl =
{
    fw_policy_QueryInterface,
    fw_policy_AddRef,
    fw_policy_Release,
    fw_policy_GetTypeInfoCount,
    fw_policy_GetTypeInfo,
    fw_policy_GetIDsOfNames,
    fw_policy_Invoke,
    fw_policy_get_CurrentProfile,
    fw_policy_GetProfileByType
};

HRESULT NetFwPolicy_create( IUnknown *pUnkOuter, LPVOID *ppObj )
{
    fw_policy *fp;

    TRACE("(%p,%p)\n", pUnkOuter, ppObj);

    fp = malloc( sizeof(*fp) );
    if (!fp) return E_OUTOFMEMORY;

    fp->INetFwPolicy_iface.lpVtbl = &fw_policy_vtbl;
    fp->refs = 1;

    *ppObj = &fp->INetFwPolicy_iface;

    TRACE("returning iface %p\n", *ppObj);
    return S_OK;
}

static HRESULT WINAPI fwpolicy2_QueryInterface(INetFwPolicy2 *iface, REFIID riid, void **out)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p %s %p\n", This, debugstr_guid( riid ), out );

    if ( IsEqualGUID( riid, &IID_INetFwPolicy2 ) ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *out = iface;
    }
    else if( IsEqualGUID( riid, &IID_INetFwRules ) )
    {
        TRACE("INetFwRules not supported\n");
        return E_NOINTERFACE;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    INetFwPolicy2_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI fwpolicy2_AddRef(INetFwPolicy2 *iface)
{
    fw_policy2 *fw_policy = impl_from_INetFwPolicy2( iface );
    return InterlockedIncrement( &fw_policy->refs );
}

static ULONG WINAPI fwpolicy2_Release(INetFwPolicy2 *iface)
{
    fw_policy2 *fw_policy = impl_from_INetFwPolicy2( iface );
    LONG refs = InterlockedDecrement( &fw_policy->refs );
    if (!refs)
    {
        INetFwRules_Release(fw_policy->fw_policy2_rules);
        TRACE("destroying %p\n", fw_policy);
        free( fw_policy );
    }
    return refs;
}

static HRESULT WINAPI fwpolicy2_GetTypeInfoCount(INetFwPolicy2 *iface, UINT *pctinfo)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p %p\n", This, pctinfo);
    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI fwpolicy2_GetTypeInfo(INetFwPolicy2 *iface, UINT iTInfo, LCID lcid, ITypeInfo **info)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p %u %lu %p\n", This, iTInfo, lcid, info);
    return get_typeinfo( INetFwPolicy2_tid, info );
}

static HRESULT WINAPI fwpolicy2_GetIDsOfNames(INetFwPolicy2 *iface, REFIID riid, LPOLESTR *rgszNames,
                        UINT cNames, LCID lcid, DISPID *rgDispId)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("%p %s %p %u %lu %p\n", This, debugstr_guid(riid), rgszNames, cNames, lcid, rgDispId);

    hr = get_typeinfo( INetFwPolicy2_tid, &typeinfo );
    if (SUCCEEDED(hr))
    {
        hr = ITypeInfo_GetIDsOfNames( typeinfo, rgszNames, cNames, rgDispId );
        ITypeInfo_Release( typeinfo );
    }
    return hr;
}

static HRESULT WINAPI fwpolicy2_Invoke(INetFwPolicy2 *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
                WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("%p %ld %s %ld %d %p %p %p %p\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo( INetFwPolicy2_tid, &typeinfo );
    if (SUCCEEDED(hr))
    {
        hr = ITypeInfo_Invoke( typeinfo, &This->INetFwPolicy2_iface, dispIdMember,
                               wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr );
        ITypeInfo_Release( typeinfo );
    }
    return hr;
}

static HRESULT WINAPI fwpolicy2_get_CurrentProfileTypes(INetFwPolicy2 *iface, LONG *profile)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p %p\n", This, profile);

    if (!profile) return E_POINTER;
    *profile = fwstore_current_profiles();
    return S_OK;
}

static HRESULT WINAPI fwpolicy2_get_FirewallEnabled(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT_BOOL *enabled)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );
    DWORD value;
    HRESULT hr;

    TRACE("%p %d %p\n", This, profileType, enabled);

    if (!enabled) return E_POINTER;
    /* no policy: off (the firewall service then filters nothing) */
    if (FAILED(hr = fwstore_get_profile_dword( profileType, L"EnableFirewall", 0, &value ))) return hr;
    *enabled = value ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT WINAPI fwpolicy2_put_FirewallEnabled(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT_BOOL enabled)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p %d %d\n", This, profileType, enabled);

    return fwstore_set_profile_dword( profileType, L"EnableFirewall", enabled ? 1 : 0 );
}

static HRESULT WINAPI fwpolicy2_get_ExcludedInterfaces(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT *interfaces)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %d %p\n", This, profileType, interfaces);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_put_ExcludedInterfaces(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT interfaces)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );
    FIXME("%p %d\n", This, profileType);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_get_BlockAllInboundTraffic(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT_BOOL *block)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );
    DWORD value;
    HRESULT hr;

    TRACE("%p %d %p\n", This, profileType, block);

    if (!block) return E_POINTER;
    if (FAILED(hr = fwstore_get_profile_dword( profileType, L"DoNotAllowExceptions", 0, &value ))) return hr;
    *block = value ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT WINAPI fwpolicy2_put_BlockAllInboundTraffic(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT_BOOL block)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p %d %d\n", This, profileType, block);

    return fwstore_set_profile_dword( profileType, L"DoNotAllowExceptions", block ? 1 : 0 );
}

static HRESULT WINAPI fwpolicy2_get_NotificationsDisabled(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT_BOOL *disabled)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %d %p\n", This, profileType, disabled);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_put_NotificationsDisabled(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT_BOOL disabled)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %d %d\n", This, profileType, disabled);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_get_UnicastResponsesToMulticastBroadcastDisabled(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT_BOOL *disabled)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %d %p\n", This, profileType, disabled);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_put_UnicastResponsesToMulticastBroadcastDisabled(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, VARIANT_BOOL disabled)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %d %d\n", This, profileType, disabled);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_get_Rules(INetFwPolicy2 *iface, INetFwRules **rules)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p %p\n", This, rules);

    if(!rules)
        return E_POINTER;

    *rules = This->fw_policy2_rules;
    INetFwRules_AddRef(This->fw_policy2_rules);

    return S_OK;
}

#ifdef __REACTOS__
typedef struct fw_service_restriction
{
    INetFwServiceRestriction INetFwServiceRestriction_iface;
    INetFwRules *rules;
    LONG refs;
} fw_service_restriction;

static inline fw_service_restriction *impl_from_INetFwServiceRestriction( INetFwServiceRestriction *iface )
{
    return CONTAINING_RECORD(iface, fw_service_restriction, INetFwServiceRestriction_iface);
}

static HRESULT WINAPI fw_service_restriction_QueryInterface( INetFwServiceRestriction *iface, REFIID riid, void **object )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );

    TRACE("%p %s %p\n", This, debugstr_guid( riid ), object );

    if ( IsEqualGUID( riid, &IID_INetFwServiceRestriction ) ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *object = iface;
        INetFwServiceRestriction_AddRef( iface );
        return S_OK;
    }

    FIXME("interface %s not implemented\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI fw_service_restriction_AddRef( INetFwServiceRestriction *iface )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );
    return InterlockedIncrement( &This->refs );
}

static ULONG WINAPI fw_service_restriction_Release( INetFwServiceRestriction *iface )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );
    LONG refs = InterlockedDecrement( &This->refs );

    if (!refs)
    {
        TRACE("destroying %p\n", This);
        INetFwRules_Release( This->rules );
        free( This );
    }
    return refs;
}

static HRESULT WINAPI fw_service_restriction_GetTypeInfoCount( INetFwServiceRestriction *iface, UINT *pctinfo )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );

    FIXME("%p %p\n", This, pctinfo);
    return E_NOTIMPL;
}

static HRESULT WINAPI fw_service_restriction_GetTypeInfo( INetFwServiceRestriction *iface, UINT iTInfo, LCID lcid,
                                                          ITypeInfo **ppTInfo )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );

    FIXME("%p %u %lu %p\n", This, iTInfo, lcid, ppTInfo);
    return E_NOTIMPL;
}

static HRESULT WINAPI fw_service_restriction_GetIDsOfNames( INetFwServiceRestriction *iface, REFIID riid,
                                                            LPOLESTR *rgszNames, UINT cNames, LCID lcid,
                                                            DISPID *rgDispId )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );

    FIXME("%p %s %p %u %lu %p\n", This, debugstr_guid(riid), rgszNames, cNames, lcid, rgDispId);
    return E_NOTIMPL;
}

static HRESULT WINAPI fw_service_restriction_Invoke( INetFwServiceRestriction *iface, DISPID dispIdMember,
                                                     REFIID riid, LCID lcid, WORD wFlags,
                                                     DISPPARAMS *pDispParams, VARIANT *pVarResult,
                                                     EXCEPINFO *pExcepInfo, UINT *puArgErr )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );

    FIXME("%p %ld %s %ld %d %p %p %p %p\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
    return E_NOTIMPL;
}

static HRESULT WINAPI fw_service_restriction_RestrictService( INetFwServiceRestriction *iface, BSTR serviceName,
                                                              BSTR appName, VARIANT_BOOL restrictService,
                                                              VARIANT_BOOL serviceSidRestricted )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );

    FIXME("%p %s %s %d %d\n", This, debugstr_w(serviceName), debugstr_w(appName), restrictService,
          serviceSidRestricted);
    return E_NOTIMPL;
}

static HRESULT WINAPI fw_service_restriction_ServiceRestricted( INetFwServiceRestriction *iface, BSTR serviceName,
                                                                BSTR appName, VARIANT_BOOL *serviceRestricted )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );

    FIXME("%p %s %s %p\n", This, debugstr_w(serviceName), debugstr_w(appName), serviceRestricted);
    return E_NOTIMPL;
}

static HRESULT WINAPI fw_service_restriction_get_Rules( INetFwServiceRestriction *iface, INetFwRules **rules )
{
    fw_service_restriction *This = impl_from_INetFwServiceRestriction( iface );

    TRACE("%p %p\n", This, rules);

    if (!rules)
        return E_POINTER;

    *rules = This->rules;
    INetFwRules_AddRef( This->rules );
    return S_OK;
}

static const struct INetFwServiceRestrictionVtbl fw_service_restriction_vtbl =
{
    fw_service_restriction_QueryInterface,
    fw_service_restriction_AddRef,
    fw_service_restriction_Release,
    fw_service_restriction_GetTypeInfoCount,
    fw_service_restriction_GetTypeInfo,
    fw_service_restriction_GetIDsOfNames,
    fw_service_restriction_Invoke,
    fw_service_restriction_RestrictService,
    fw_service_restriction_ServiceRestricted,
    fw_service_restriction_get_Rules
};
#endif

static HRESULT WINAPI fwpolicy2_get_ServiceRestriction(INetFwPolicy2 *iface, INetFwServiceRestriction **ServiceRestriction)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );
#ifdef __REACTOS__
    fw_service_restriction *restriction;

    TRACE("%p %p\n", This, ServiceRestriction);

    if (!ServiceRestriction)
        return E_POINTER;

    if (!(restriction = malloc( sizeof(*restriction) )))
        return E_OUTOFMEMORY;

    if (FAILED(create_INetFwRules( &restriction->rules )))
    {
        free( restriction );
        return E_OUTOFMEMORY;
    }

    restriction->INetFwServiceRestriction_iface.lpVtbl = &fw_service_restriction_vtbl;
    restriction->refs = 1;
    *ServiceRestriction = &restriction->INetFwServiceRestriction_iface;
    return S_OK;
#else

    FIXME("%p %p\n", This, ServiceRestriction);
    return E_NOTIMPL;
#endif
}

static HRESULT WINAPI fwpolicy2_EnableRuleGroup(INetFwPolicy2 *iface, LONG bitmask, BSTR group, VARIANT_BOOL enable)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %ld %s %d\n", This, bitmask, debugstr_w(group), enable);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_IsRuleGroupEnabled(INetFwPolicy2 *iface, LONG bitmask, BSTR group, VARIANT_BOOL *enabled)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %ld %s %p\n", This, bitmask, debugstr_w(group), enabled);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_RestoreLocalFirewallDefaults(INetFwPolicy2 *iface)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p\n", This);

    return fwstore_restore_defaults();
}

static HRESULT WINAPI fwpolicy2_get_DefaultInboundAction(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, NET_FW_ACTION *action)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );
    DWORD value;
    HRESULT hr;

    TRACE("%p %d %p\n", This, profileType, action);

    if (!action) return E_POINTER;
    /* the policy value is 1 for block, as Windows keeps it; absent: block */
    if (FAILED(hr = fwstore_get_profile_dword( profileType, L"DefaultInboundAction", 1, &value ))) return hr;
    *action = value ? NET_FW_ACTION_BLOCK : NET_FW_ACTION_ALLOW;
    return S_OK;
}

static HRESULT WINAPI fwpolicy2_put_DefaultInboundAction(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, NET_FW_ACTION action)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    TRACE("%p %d %d\n", This, profileType, action);

    if (action != NET_FW_ACTION_ALLOW && action != NET_FW_ACTION_BLOCK) return E_INVALIDARG;
    return fwstore_set_profile_dword( profileType, L"DefaultInboundAction", action == NET_FW_ACTION_BLOCK ? 1 : 0 );
}

static HRESULT WINAPI fwpolicy2_get_DefaultOutboundAction(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, NET_FW_ACTION *action)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %d %p\n", This, profileType, action);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_put_DefaultOutboundAction(INetFwPolicy2 *iface, NET_FW_PROFILE_TYPE2 profileType, NET_FW_ACTION action)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %d %d\n", This, profileType, action);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_get_IsRuleGroupCurrentlyEnabled(INetFwPolicy2 *iface, BSTR group, VARIANT_BOOL *enabled)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %s %p\n", This, debugstr_w(group), enabled);
    return E_NOTIMPL;
}

static HRESULT WINAPI fwpolicy2_get_LocalPolicyModifyState(INetFwPolicy2 *iface, NET_FW_MODIFY_STATE *modifyState)
{
    fw_policy2 *This = impl_from_INetFwPolicy2( iface );

    FIXME("%p %p\n", This, modifyState);
    return E_NOTIMPL;
}

static const struct INetFwPolicy2Vtbl fw_policy2_vtbl =
{
    fwpolicy2_QueryInterface,
    fwpolicy2_AddRef,
    fwpolicy2_Release,
    fwpolicy2_GetTypeInfoCount,
    fwpolicy2_GetTypeInfo,
    fwpolicy2_GetIDsOfNames,
    fwpolicy2_Invoke,
    fwpolicy2_get_CurrentProfileTypes,
    fwpolicy2_get_FirewallEnabled,
    fwpolicy2_put_FirewallEnabled,
    fwpolicy2_get_ExcludedInterfaces,
    fwpolicy2_put_ExcludedInterfaces,
    fwpolicy2_get_BlockAllInboundTraffic,
    fwpolicy2_put_BlockAllInboundTraffic,
    fwpolicy2_get_NotificationsDisabled,
    fwpolicy2_put_NotificationsDisabled,
    fwpolicy2_get_UnicastResponsesToMulticastBroadcastDisabled,
    fwpolicy2_put_UnicastResponsesToMulticastBroadcastDisabled,
    fwpolicy2_get_Rules,
    fwpolicy2_get_ServiceRestriction,
    fwpolicy2_EnableRuleGroup,
    fwpolicy2_IsRuleGroupEnabled,
    fwpolicy2_RestoreLocalFirewallDefaults,
    fwpolicy2_get_DefaultInboundAction,
    fwpolicy2_put_DefaultInboundAction,
    fwpolicy2_get_DefaultOutboundAction,
    fwpolicy2_put_DefaultOutboundAction,
    fwpolicy2_get_IsRuleGroupCurrentlyEnabled,
    fwpolicy2_get_LocalPolicyModifyState
};

HRESULT NetFwPolicy2_create( IUnknown *outer, void **obj )
{
    fw_policy2 *fp;

    TRACE("(%p,%p)\n", outer, obj);

    fp = malloc( sizeof(*fp) );
    if (!fp) return E_OUTOFMEMORY;

    fp->INetFwPolicy2_iface.lpVtbl = &fw_policy2_vtbl;
    fp->refs = 1;

    *obj = &fp->INetFwPolicy2_iface;

    if (FAILED(create_INetFwRules(&fp->fw_policy2_rules)))
    {
        free( fp );
        return E_OUTOFMEMORY;
    }

    TRACE("returning iface %p\n", *obj);
    return S_OK;
}

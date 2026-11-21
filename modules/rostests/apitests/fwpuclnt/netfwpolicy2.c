/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     INetFwPolicy2 (hnetcfg): the profiles and rules the firewall service applies
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * Writes the machine firewall policy: run as an administrator. The settings the test changes
 * are put back as they were.
 */

#define COBJMACROS
#include <apitest.h>
#include <ole2.h>
#include <netfw.h>

#define TEST_RULE L"LiberNT apitest rule"

static void test_profiles(INetFwPolicy2 *policy)
{
    VARIANT_BOOL enabled, block, saved_enabled, saved_block;
    NET_FW_ACTION action, saved_action;
    LONG current = 0;
    HRESULT hr;

    hr = INetFwPolicy2_get_CurrentProfileTypes(policy, &current);
    ok(hr == S_OK, "get_CurrentProfileTypes returned 0x%08lx\n", hr);
    ok(current != 0 && !(current & ~NET_FW_PROFILE2_ALL), "Unexpected current profiles 0x%lx\n", current);

    hr = INetFwPolicy2_get_FirewallEnabled(policy, NET_FW_PROFILE2_PUBLIC, &saved_enabled);
    ok(hr == S_OK, "get_FirewallEnabled returned 0x%08lx\n", hr);
    hr = INetFwPolicy2_get_BlockAllInboundTraffic(policy, NET_FW_PROFILE2_PUBLIC, &saved_block);
    ok(hr == S_OK, "get_BlockAllInboundTraffic returned 0x%08lx\n", hr);
    hr = INetFwPolicy2_get_DefaultInboundAction(policy, NET_FW_PROFILE2_PUBLIC, &saved_action);
    ok(hr == S_OK, "get_DefaultInboundAction returned 0x%08lx\n", hr);

    hr = INetFwPolicy2_put_FirewallEnabled(policy, NET_FW_PROFILE2_PUBLIC, VARIANT_TRUE);
    ok(hr == S_OK, "put_FirewallEnabled returned 0x%08lx\n", hr);
    hr = INetFwPolicy2_get_FirewallEnabled(policy, NET_FW_PROFILE2_PUBLIC, &enabled);
    ok(hr == S_OK && enabled == VARIANT_TRUE, "FirewallEnabled did not read back on (0x%08lx, %d)\n", hr, enabled);
    hr = INetFwPolicy2_put_FirewallEnabled(policy, NET_FW_PROFILE2_PUBLIC, VARIANT_FALSE);
    hr = INetFwPolicy2_get_FirewallEnabled(policy, NET_FW_PROFILE2_PUBLIC, &enabled);
    ok(hr == S_OK && enabled == VARIANT_FALSE, "FirewallEnabled did not read back off\n");

    hr = INetFwPolicy2_put_DefaultInboundAction(policy, NET_FW_PROFILE2_PUBLIC, NET_FW_ACTION_ALLOW);
    ok(hr == S_OK, "put_DefaultInboundAction returned 0x%08lx\n", hr);
    hr = INetFwPolicy2_get_DefaultInboundAction(policy, NET_FW_PROFILE2_PUBLIC, &action);
    ok(hr == S_OK && action == NET_FW_ACTION_ALLOW, "DefaultInboundAction did not read back allow\n");
    hr = INetFwPolicy2_put_DefaultInboundAction(policy, NET_FW_PROFILE2_PUBLIC, (NET_FW_ACTION)7);
    ok(hr == E_INVALIDARG, "A bad action was accepted: 0x%08lx\n", hr);

    hr = INetFwPolicy2_put_BlockAllInboundTraffic(policy, NET_FW_PROFILE2_PUBLIC, VARIANT_TRUE);
    ok(hr == S_OK, "put_BlockAllInboundTraffic returned 0x%08lx\n", hr);
    hr = INetFwPolicy2_get_BlockAllInboundTraffic(policy, NET_FW_PROFILE2_PUBLIC, &block);
    ok(hr == S_OK && block == VARIANT_TRUE, "BlockAllInboundTraffic did not read back on\n");

    /* a profile mask sets every profile in it */
    hr = INetFwPolicy2_put_FirewallEnabled(policy, NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN, VARIANT_FALSE);
    ok(hr == S_OK, "put_FirewallEnabled on two profiles returned 0x%08lx\n", hr);

    INetFwPolicy2_put_FirewallEnabled(policy, NET_FW_PROFILE2_PUBLIC, saved_enabled);
    INetFwPolicy2_put_BlockAllInboundTraffic(policy, NET_FW_PROFILE2_PUBLIC, saved_block);
    INetFwPolicy2_put_DefaultInboundAction(policy, NET_FW_PROFILE2_PUBLIC, saved_action);
}

static BOOL rule_listed(INetFwRules *rules, const WCHAR *name)
{
    IUnknown *unk = NULL;
    IEnumVARIANT *enumvar = NULL;
    VARIANT var;
    ULONG fetched;
    BOOL found = FALSE;

    if (FAILED(INetFwRules_get__NewEnum(rules, &unk)) || !unk)
        return FALSE;
    if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IEnumVARIANT, (void **)&enumvar)))
    {
        while (IEnumVARIANT_Next(enumvar, 1, &var, &fetched) == S_OK && fetched)
        {
            INetFwRule *rule = NULL;
            BSTR rulename = NULL;

            if (V_VT(&var) == VT_DISPATCH &&
                SUCCEEDED(IDispatch_QueryInterface(V_DISPATCH(&var), &IID_INetFwRule, (void **)&rule)))
            {
                if (SUCCEEDED(INetFwRule_get_Name(rule, &rulename)) && rulename && !wcscmp(rulename, name))
                    found = TRUE;
                SysFreeString(rulename);
                INetFwRule_Release(rule);
            }
            VariantClear(&var);
        }
        IEnumVARIANT_Release(enumvar);
    }
    IUnknown_Release(unk);
    return found;
}

static void test_rules(INetFwPolicy2 *policy)
{
    INetFwRules *rules = NULL;
    INetFwRule *rule = NULL, *item = NULL;
    LONG count = -1, count2 = -1, protocol = 0, profiles = 0;
    VARIANT_BOOL enabled;
    NET_FW_ACTION action;
    NET_FW_RULE_DIRECTION dir;
    BSTR value = NULL;
    HRESULT hr;

    hr = INetFwPolicy2_get_Rules(policy, &rules);
    ok(hr == S_OK, "get_Rules returned 0x%08lx\n", hr);
    if (FAILED(hr))
        return;

    /* a rule left by an earlier run */
    INetFwRules_Remove(rules, (BSTR)TEST_RULE);
    hr = INetFwRules_get_Count(rules, &count);
    ok(hr == S_OK && count >= 0, "get_Count returned 0x%08lx, %ld\n", hr, count);

    hr = CoCreateInstance(&CLSID_NetFwRule, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwRule, (void **)&rule);
    ok(hr == S_OK, "CoCreateInstance(NetFwRule) returned 0x%08lx\n", hr);
    if (FAILED(hr))
    {
        INetFwRules_Release(rules);
        return;
    }

    /* a new rule has no name and cannot be added */
    hr = INetFwRules_Add(rules, rule);
    ok(hr == E_INVALIDARG, "A rule without a name was added: 0x%08lx\n", hr);

    ok(INetFwRule_put_Name(rule, (BSTR)L"bad|name") == E_INVALIDARG, "A name with '|' was accepted\n");
    ok(INetFwRule_put_Name(rule, (BSTR)TEST_RULE) == S_OK, "put_Name failed\n");
    ok(INetFwRule_put_Description(rule, (BSTR)L"Opens 7778, 7779") == S_OK, "put_Description failed\n");
    ok(INetFwRule_put_Protocol(rule, 6) == S_OK, "put_Protocol failed\n");
    ok(INetFwRule_put_LocalPorts(rule, (BSTR)L"7778,7779") == S_OK, "put_LocalPorts failed\n");
    ok(INetFwRule_put_Direction(rule, NET_FW_RULE_DIR_IN) == S_OK, "put_Direction failed\n");
    ok(INetFwRule_put_Action(rule, NET_FW_ACTION_ALLOW) == S_OK, "put_Action failed\n");
    ok(INetFwRule_put_Profiles(rule, NET_FW_PROFILE2_PUBLIC) == S_OK, "put_Profiles failed\n");
    ok(INetFwRule_put_Enabled(rule, VARIANT_TRUE) == S_OK, "put_Enabled failed\n");

    hr = INetFwRules_Add(rules, rule);
    ok(hr == S_OK, "Add returned 0x%08lx\n", hr);
    hr = INetFwRules_get_Count(rules, &count2);
    ok(hr == S_OK && count2 == count + 1, "Count went from %ld to %ld\n", count, count2);
    ok(rule_listed(rules, TEST_RULE), "The added rule is not enumerated\n");

    hr = INetFwRules_Item(rules, (BSTR)TEST_RULE, &item);
    ok(hr == S_OK, "Item returned 0x%08lx\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(INetFwRule_get_LocalPorts(item, &value) == S_OK && value && !wcscmp(value, L"7778,7779"),
           "LocalPorts read back %ls\n", value);
        SysFreeString(value);
        value = NULL;
        ok(INetFwRule_get_Description(item, &value) == S_OK && value && !wcscmp(value, L"Opens 7778, 7779"),
           "Description read back %ls\n", value);
        SysFreeString(value);
        value = NULL;
        ok(INetFwRule_get_RemotePorts(item, &value) == S_OK && value && !wcscmp(value, L"*"),
           "RemotePorts read back %ls\n", value);
        SysFreeString(value);
        ok(INetFwRule_get_Protocol(item, &protocol) == S_OK && protocol == 6, "Protocol read back %ld\n", protocol);
        ok(INetFwRule_get_Profiles(item, &profiles) == S_OK && profiles == NET_FW_PROFILE2_PUBLIC,
           "Profiles read back 0x%lx\n", profiles);
        ok(INetFwRule_get_Direction(item, &dir) == S_OK && dir == NET_FW_RULE_DIR_IN, "Direction read back %d\n", dir);
        ok(INetFwRule_get_Action(item, &action) == S_OK && action == NET_FW_ACTION_ALLOW, "Action read back %d\n", action);
        ok(INetFwRule_get_Enabled(item, &enabled) == S_OK && enabled == VARIANT_TRUE, "Enabled read back %d\n", enabled);

        /* a rule of the store is written as it changes */
        ok(INetFwRule_put_Enabled(item, VARIANT_FALSE) == S_OK, "put_Enabled on a stored rule failed\n");
        INetFwRule_Release(item);
        item = NULL;
        if (SUCCEEDED(INetFwRules_Item(rules, (BSTR)TEST_RULE, &item)))
        {
            ok(INetFwRule_get_Enabled(item, &enabled) == S_OK && enabled == VARIANT_FALSE,
               "The change to a stored rule was not kept\n");
            INetFwRule_Release(item);
        }
    }

    /* the rule already in the store cannot be added twice */
    ok(INetFwRules_Add(rules, rule) != S_OK, "A stored rule was added again\n");

    hr = INetFwRules_Remove(rules, (BSTR)TEST_RULE);
    ok(hr == S_OK, "Remove returned 0x%08lx\n", hr);
    hr = INetFwRules_get_Count(rules, &count2);
    ok(hr == S_OK && count2 == count, "Count after Remove is %ld, was %ld\n", count2, count);
    item = NULL;
    ok(INetFwRules_Item(rules, (BSTR)TEST_RULE, &item) != S_OK && !item, "A removed rule was found\n");

    INetFwRule_Release(rule);
    INetFwRules_Release(rules);
}

START_TEST(netfwpolicy2)
{
    INetFwPolicy2 *policy = NULL;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2, (void **)&policy);
    ok(hr == S_OK, "CoCreateInstance(NetFwPolicy2) returned 0x%08lx\n", hr);
    if (SUCCEEDED(hr))
    {
        test_profiles(policy);
        test_rules(policy);
        INetFwPolicy2_Release(policy);
    }
    CoUninitialize();
}

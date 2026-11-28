/*
 * PROJECT:     LiberNT reg.exe
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     REG COMPARE
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "reg.h"

enum compare_output { OUTPUT_DIFFERENT, OUTPUT_ALL, OUTPUT_SAME, OUTPUT_NONE };

struct compare_state
{
    enum compare_output output;
    BOOL recurse;
    BOOL different;
    REGSAM sam;
};

struct reg_value
{
    DWORD type;
    DWORD size;
    BYTE *data;
};

static void output_value_line(struct compare_state *state, WCHAR mark, const WCHAR *key_name,
                              const WCHAR *value_name, const struct reg_value *value)
{
    WCHAR mark_str[2] = { mark, 0 };
    const WCHAR *type;
    WCHAR defval[32];
    WCHAR *data;

    if (mark == '=' ? (state->output != OUTPUT_ALL && state->output != OUTPUT_SAME)
                    : (state->output != OUTPUT_ALL && state->output != OUTPUT_DIFFERENT))
        return;

    if (!*value_name)
    {
        LoadStringW(GetModuleHandleW(NULL), STRING_DEFAULT_VALUE, defval, ARRAY_SIZE(defval));
        value_name = defval;
    }

    type = reg_type_to_wchar(value->type);
    data = reg_data_to_wchar(value->type, value->data, value->size);
    output_message(STRING_COMPARE_VALUE, mark_str, key_name, value_name,
                   type ? type : L"", data ? data : L"");
    free(data);
}

static void output_key_line(struct compare_state *state, WCHAR mark, const WCHAR *key_name)
{
    WCHAR mark_str[2] = { mark, 0 };

    if (mark == '=' ? (state->output != OUTPUT_ALL && state->output != OUTPUT_SAME)
                    : (state->output != OUTPUT_ALL && state->output != OUTPUT_DIFFERENT))
        return;

    output_message(STRING_COMPARE_KEY, mark_str, key_name);
}

static LONG read_value(HKEY hkey, const WCHAR *name, struct reg_value *value)
{
    LONG error;

    value->data = NULL;
    value->size = 0;
    error = RegQueryValueExW(hkey, name, NULL, &value->type, NULL, &value->size);
    if (error)
        return error;

    value->data = malloc(value->size + sizeof(WCHAR) * 2);
    if (!value->data)
        return ERROR_NOT_ENOUGH_MEMORY;

    error = RegQueryValueExW(hkey, name, NULL, &value->type, value->data, &value->size);
    if (error)
    {
        free(value->data);
        value->data = NULL;
        return error;
    }

    ZeroMemory(value->data + value->size, sizeof(WCHAR) * 2);
    return ERROR_SUCCESS;
}

static BOOL values_equal(const struct reg_value *a, const struct reg_value *b)
{
    return a->type == b->type && a->size == b->size && !memcmp(a->data, b->data, a->size);
}

/* Compares one value; returns FALSE when it exists in neither key */
static BOOL compare_value(struct compare_state *state, HKEY key1, const WCHAR *name1,
                          HKEY key2, const WCHAR *name2, const WCHAR *value_name)
{
    struct reg_value v1, v2;
    BOOL have1, have2;

    have1 = !read_value(key1, value_name, &v1);
    have2 = !read_value(key2, value_name, &v2);

    if (have1 && have2 && values_equal(&v1, &v2))
    {
        output_value_line(state, '=', name1, value_name, &v1);
    }
    else
    {
        if (have1 || have2)
            state->different = TRUE;
        if (have1)
            output_value_line(state, '<', name1, value_name, &v1);
        if (have2)
            output_value_line(state, '>', name2, value_name, &v2);
    }

    if (have1) free(v1.data);
    if (have2) free(v2.data);
    return have1 || have2;
}

static WCHAR *enum_name(HKEY hkey, BOOL subkeys, DWORD index, WCHAR *buffer, DWORD buffer_len)
{
    DWORD len = buffer_len;
    LONG error;

    if (subkeys)
        error = RegEnumKeyExW(hkey, index, buffer, &len, NULL, NULL, NULL, NULL);
    else
        error = RegEnumValueW(hkey, index, buffer, &len, NULL, NULL, NULL, NULL);

    return error ? NULL : buffer;
}

static BOOL has_value(HKEY hkey, const WCHAR *name)
{
    return !RegQueryValueExW(hkey, name, NULL, NULL, NULL, NULL);
}

static BOOL open_subkey(struct compare_state *state, HKEY hkey, const WCHAR *name, HKEY *subkey)
{
    return !RegOpenKeyExW(hkey, name, 0, KEY_READ | state->sam, subkey);
}

static void compare_keys(struct compare_state *state, HKEY key1, WCHAR *name1, HKEY key2, WCHAR *name2)
{
    DWORD max1 = 0, max2 = 0, maxkey1 = 0, maxkey2 = 0, len, i;
    WCHAR *buffer, *name;
    HKEY sub1, sub2;

    RegQueryInfoKeyW(key1, NULL, NULL, NULL, NULL, &maxkey1, NULL, NULL, &max1, NULL, NULL, NULL);
    RegQueryInfoKeyW(key2, NULL, NULL, NULL, NULL, &maxkey2, NULL, NULL, &max2, NULL, NULL, NULL);
    len = max(max(max1, max2), max(maxkey1, maxkey2)) + 1;
    buffer = malloc(len * sizeof(WCHAR));
    if (!buffer)
        return;

    for (i = 0; (name = enum_name(key1, FALSE, i, buffer, len)); i++)
        compare_value(state, key1, name1, key2, name2, name);

    for (i = 0; (name = enum_name(key2, FALSE, i, buffer, len)); i++)
    {
        if (!has_value(key1, name))
            compare_value(state, key1, name1, key2, name2, name);
    }

    if (state->recurse)
    {
        for (i = 0; (name = enum_name(key1, TRUE, i, buffer, len)); i++)
        {
            WCHAR *path1 = build_subkey_path(name1, lstrlenW(name1), name, lstrlenW(name));

            if (!open_subkey(state, key1, name, &sub1))
            {
                free(path1);
                continue;
            }

            if (open_subkey(state, key2, name, &sub2))
            {
                WCHAR *path2 = build_subkey_path(name2, lstrlenW(name2), name, lstrlenW(name));

                output_key_line(state, '=', path1);
                compare_keys(state, sub1, path1, sub2, path2);
                RegCloseKey(sub2);
                free(path2);
            }
            else
            {
                state->different = TRUE;
                output_key_line(state, '<', path1);
            }

            RegCloseKey(sub1);
            free(path1);
        }

        for (i = 0; (name = enum_name(key2, TRUE, i, buffer, len)); i++)
        {
            if (open_subkey(state, key1, name, &sub1))
            {
                RegCloseKey(sub1);
                continue;
            }

            state->different = TRUE;
            if (state->output == OUTPUT_ALL || state->output == OUTPUT_DIFFERENT)
            {
                WCHAR *path2 = build_subkey_path(name2, lstrlenW(name2), name, lstrlenW(name));
                output_key_line(state, '>', path2);
                free(path2);
            }
        }
    }

    free(buffer);
}

int reg_compare(int argc, WCHAR *argvW[])
{
    struct compare_state state = { OUTPUT_DIFFERENT, FALSE, FALSE, 0 };
    WCHAR *value_name = NULL, *path1, *path2, *name1, *name2;
    BOOL value_empty = FALSE, output_set = FALSE;
    HKEY root1, root2, key1, key2;
    LONG error;
    int i;

    if (argc < 4)
        goto invalid;

    for (i = 4; i < argc; i++)
    {
        WCHAR *str;

        if (argvW[i][0] != '/' && argvW[i][0] != '-')
            goto invalid;

        str = &argvW[i][1];

        if (!lstrcmpiW(str, L"ve"))
        {
            if (value_empty) goto invalid;
            value_empty = TRUE;
        }
        else if (!lstrcmpiW(str, L"v"))
        {
            if (value_name || !(value_name = argvW[++i])) goto invalid;
        }
        else if (!lstrcmpiW(str, L"s"))
        {
            if (state.recurse) goto invalid;
            state.recurse = TRUE;
        }
        else if (!lstrcmpiW(str, L"oa") || !lstrcmpiW(str, L"od") ||
                 !lstrcmpiW(str, L"os") || !lstrcmpiW(str, L"on"))
        {
            if (output_set) goto invalid;
            output_set = TRUE;
            switch (towlower(str[1]))
            {
                case 'a': state.output = OUTPUT_ALL; break;
                case 's': state.output = OUTPUT_SAME; break;
                case 'n': state.output = OUTPUT_NONE; break;
                default:  state.output = OUTPUT_DIFFERENT; break;
            }
        }
        else if (!lstrcmpiW(str, L"reg:32"))
        {
            if (state.sam & KEY_WOW64_32KEY) goto invalid;
            state.sam |= KEY_WOW64_32KEY;
        }
        else if (!lstrcmpiW(str, L"reg:64"))
        {
            if (state.sam & KEY_WOW64_64KEY) goto invalid;
            state.sam |= KEY_WOW64_64KEY;
        }
        else
            goto invalid;
    }

    if ((value_name && value_empty) || state.sam == (KEY_WOW64_32KEY | KEY_WOW64_64KEY))
        goto invalid;

    if (!parse_registry_key(argvW[2], &root1, &path1))
        return 1;

    /* "\\." names the first key on the local computer */
    if (!lstrcmpW(argvW[3], L"\\\\."))
    {
        output_message(STRING_COMPARE_SELF);
        return 1;
    }

    if (!parse_registry_key(argvW[3], &root2, &path2))
        return 1;

    if (root1 == root2 && !lstrcmpiW(path1 ? path1 : L"", path2 ? path2 : L""))
    {
        output_message(STRING_COMPARE_SELF);
        return 1;
    }

    if ((error = RegOpenKeyExW(root1, path1, 0, KEY_READ | state.sam, &key1)))
    {
        output_message(STRING_REG_NOT_FOUND);
        return 1;
    }

    if ((error = RegOpenKeyExW(root2, path2, 0, KEY_READ | state.sam, &key2)))
    {
        RegCloseKey(key1);
        output_message(STRING_REG_NOT_FOUND);
        return 1;
    }

    name1 = get_long_key(root1, path1);
    name2 = get_long_key(root2, path2);

    if (value_name || value_empty)
    {
        if (!compare_value(&state, key1, name1, key2, name2, value_empty ? L"" : value_name))
        {
            output_message(STRING_REG_NOT_FOUND);
            RegCloseKey(key1);
            RegCloseKey(key2);
            free(name1);
            free(name2);
            return 1;
        }
    }
    else
        compare_keys(&state, key1, name1, key2, name2);

    RegCloseKey(key1);
    RegCloseKey(key2);
    free(name1);
    free(name2);

    output_message(state.different ? STRING_COMPARE_DIFFERENT : STRING_COMPARE_IDENTICAL);
    output_message(STRING_SUCCESS);
    return state.different ? 2 : 0;

invalid:
    output_message(STRING_INVALID_SYNTAX);
    output_message(STRING_FUNC_HELP, wcsupr(argvW[1]));
    return 1;
}

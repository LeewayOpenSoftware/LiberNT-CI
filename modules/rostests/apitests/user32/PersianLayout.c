/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the characters of the Persian keyboard layout
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static void TestKey(HKL hkl, UINT vk, BOOL shift, BOOL ctrl, LPCWSTR expect)
{
    BYTE state[256] = { 0 };
    WCHAR buffer[8] = { 0 };
    int ret, len = lstrlenW(expect);

    if (shift)
        state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (ctrl)
        state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;

    ret = ToUnicodeEx(vk, MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl), state, buffer, ARRAYSIZE(buffer), 0, hkl);
    ok(ret == len, "vk %02x shift %d ctrl %d: got %d characters, expected %d\n", vk, shift, ctrl, ret, len);
    ok(!memcmp(buffer, expect, len * sizeof(WCHAR)), "vk %02x shift %d ctrl %d: got %04x %04x, expected %04x\n",
       vk, shift, ctrl, buffer[0], buffer[1], expect[0]);
}

START_TEST(PersianLayout)
{
    HKL hkl, *layouts;
    int i, count;
    BOOL loaded = FALSE;

    count = GetKeyboardLayoutList(0, NULL);
    layouts = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*layouts));
    if (layouts)
    {
        count = GetKeyboardLayoutList(count, layouts);
        for (i = 0; i < count; i++)
        {
            if (layouts[i] == (HKL)0x04290429)
                loaded = TRUE;
        }
        HeapFree(GetProcessHeap(), 0, layouts);
    }

    hkl = LoadKeyboardLayoutW(L"00000429", KLF_NOTELLSHELL);
    ok(hkl == (HKL)0x04290429, "LoadKeyboardLayoutW returned %p, error %lu\n", hkl, GetLastError());
    if (!hkl)
        return;

    TestKey(hkl, 'D', FALSE, FALSE, L"\x06cc");
    TestKey(hkl, 'C', TRUE, FALSE, L"\x0698");
    TestKey(hkl, VK_OEM_3, FALSE, FALSE, L"\x00f7");
    TestKey(hkl, VK_OEM_102, FALSE, FALSE, L"\x067e");
    TestKey(hkl, VK_OEM_6, FALSE, FALSE, L"\x0686");
    TestKey(hkl, 'R', TRUE, FALSE, L"\x0631\x064a\x0627\x0644");
    TestKey(hkl, '0', FALSE, TRUE, L"\x06c1");
    TestKey(hkl, '2', TRUE, TRUE, L"\x200c");

    if (!loaded)
        UnloadKeyboardLayout(hkl);
}

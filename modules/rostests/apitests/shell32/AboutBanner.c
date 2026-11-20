/* ReactOS API tests. LGPL-2.1-or-later. */
#include <apitest.h>
#include <windows.h>

#define TEST_IDB_ABOUT_BANNER 131
#define TEST_IDB_ABOUT_LINE   138

static void check_bitmap(HMODULE module, int id, int cx, int cy)
{
    HBITMAP bitmap = LoadBitmapW(module, MAKEINTRESOURCEW(id));
    BITMAP info;

    ok(bitmap != NULL, "bitmap %d missing: %lu\n", id, GetLastError());
    if (!bitmap)
        return;
    ok(GetObjectW(bitmap, sizeof(info), &info) == sizeof(info), "GetObject failed for %d\n", id);
    ok(info.bmWidth == cx, "bitmap %d width %ld, expected %d\n", id, info.bmWidth, cx);
    ok(info.bmHeight == cy, "bitmap %d height %ld, expected %d\n", id, info.bmHeight, cy);
    DeleteObject(bitmap);
}

START_TEST(AboutBanner)
{
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");

    ok(shell32 != NULL, "shell32 not loaded\n");
    if (!shell32)
        return;
    check_bitmap(shell32, TEST_IDB_ABOUT_BANNER, 413, 72);
    check_bitmap(shell32, 132, 516, 90);
    check_bitmap(shell32, 133, 619, 108);
    check_bitmap(shell32, 134, 722, 126);
    check_bitmap(shell32, 135, 826, 144);
    check_bitmap(shell32, TEST_IDB_ABOUT_LINE, 413, 5);
}

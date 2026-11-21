/* ReactOS API tests. LGPL-2.1-or-later. */
#include <apitest.h>
#include <windows.h>

/* The About header wordmark: 32-bit art at 96, 144 and 192 dpi (ids 131, 133, 135) */
#define TEST_IDB_OROSI_WORDMARK     131
#define TEST_IDB_OROSI_WORDMARK_150 133
#define TEST_IDB_OROSI_WORDMARK_200 135

/* Gone with the old banner */
#define TEST_IDB_OLD_BANNER_125 132
#define TEST_IDB_OLD_BANNER_175 134
#define TEST_IDB_OLD_LINE       138

static void check_bitmap(HMODULE module, int id, int cx, int cy)
{
    HBITMAP bitmap = (HBITMAP)LoadImageW(module, MAKEINTRESOURCEW(id), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    DIBSECTION info;

    ok(bitmap != NULL, "bitmap %d missing: %lu\n", id, GetLastError());
    if (!bitmap)
        return;
    ok(GetObjectW(bitmap, sizeof(info), &info) == sizeof(info), "GetObject failed for %d\n", id);
    ok(info.dsBm.bmWidth == cx, "bitmap %d width %ld, expected %d\n", id, info.dsBm.bmWidth, cx);
    ok(info.dsBm.bmHeight == cy, "bitmap %d height %ld, expected %d\n", id, info.dsBm.bmHeight, cy);
    ok(info.dsBm.bmBitsPixel == 32, "bitmap %d is %u bpp, expected 32 (alpha)\n", id, info.dsBm.bmBitsPixel);
    ok(info.dsBm.bmBits != NULL, "bitmap %d has no DIB bits\n", id);
    DeleteObject(bitmap);
}

static void check_gone(HMODULE module, int id)
{
    HBITMAP bitmap = LoadBitmapW(module, MAKEINTRESOURCEW(id));
    ok(bitmap == NULL, "old banner bitmap %d still present\n", id);
    if (bitmap)
        DeleteObject(bitmap);
}

START_TEST(AboutBanner)
{
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");

    ok(shell32 != NULL, "shell32 not loaded\n");
    if (!shell32)
        return;
    check_bitmap(shell32, TEST_IDB_OROSI_WORDMARK, 261, 48);
    check_bitmap(shell32, TEST_IDB_OROSI_WORDMARK_150, 391, 72);
    check_bitmap(shell32, TEST_IDB_OROSI_WORDMARK_200, 522, 96);
    check_gone(shell32, TEST_IDB_OLD_BANNER_125);
    check_gone(shell32, TEST_IDB_OLD_BANNER_175);
    check_gone(shell32, TEST_IDB_OLD_LINE);
}

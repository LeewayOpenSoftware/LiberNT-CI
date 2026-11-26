/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Synthetic-topology tests for the wallpaper Span geometry helper
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <apitest.h>
#include <windef.h>
#include "wallpaperspan.h"

static void Test_OneMonitorEqualsFill(void)
{
    WALLPAPER_SPAN_LAYOUT Layout;
    RECTL Virtual = { 0, 0, 1280, 800 };
    RECTL Monitor = { 0, 0, 1280, 800 };
    RECTL Dest, Source;
    BOOL Ret;

    Ret = WallpaperSpanLayout(4000, 3000, &Virtual, &Layout);
    ok(Ret, "layout failed\n");
    if (!Ret)
        return;
    ok(Layout.ScaleNum == 1280 && Layout.ScaleDen == 4000,
       "scale %ld/%ld, expected 1280/4000\n", Layout.ScaleNum, Layout.ScaleDen);
    ok(Layout.Source.left == 0 && Layout.Source.top == 250 &&
       Layout.Source.right == 4000 && Layout.Source.bottom == 2750,
       "crop (%ld,%ld)-(%ld,%ld), expected (0,250)-(4000,2750)\n",
       Layout.Source.left, Layout.Source.top, Layout.Source.right, Layout.Source.bottom);

    Ret = WallpaperSpanSample(&Layout, 4000, 3000, &Virtual, &Monitor, &Dest, &Source);
    ok(Ret, "sample failed\n");
    if (!Ret)
        return;
    ok(Dest.left == 0 && Dest.top == 0 && Dest.right == 1280 && Dest.bottom == 800,
       "dest (%ld,%ld)-(%ld,%ld)\n", Dest.left, Dest.top, Dest.right, Dest.bottom);
    ok(Source.left == Layout.Source.left && Source.top == Layout.Source.top &&
       Source.right == Layout.Source.right && Source.bottom == Layout.Source.bottom,
       "one monitor must sample the whole crop\n");
}

static void Test_SideBySideSeam(void)
{
    WALLPAPER_SPAN_LAYOUT Layout;
    RECTL Virtual = { 0, 0, 3840, 1080 };
    RECTL Left = { 0, 0, 1920, 1080 };
    RECTL Right = { 1920, 0, 3840, 1080 };
    RECTL DestL, SrcL, DestR, SrcR;
    BOOL Ret;

    Ret = WallpaperSpanLayout(4000, 3000, &Virtual, &Layout);
    ok(Ret, "layout failed\n");
    if (!Ret)
        return;
    ok(Layout.Source.left == 0 && Layout.Source.top == 937 &&
       Layout.Source.right == 4000 && Layout.Source.bottom == 2062,
       "crop (%ld,%ld)-(%ld,%ld), expected (0,937)-(4000,2062)\n",
       Layout.Source.left, Layout.Source.top, Layout.Source.right, Layout.Source.bottom);

    Ret = WallpaperSpanSample(&Layout, 4000, 3000, &Virtual, &Left, &DestL, &SrcL);
    ok(Ret, "left sample failed\n");
    Ret = WallpaperSpanSample(&Layout, 4000, 3000, &Virtual, &Right, &DestR, &SrcR);
    ok(Ret, "right sample failed\n");
    if (!Ret)
        return;
    ok(DestL.right == 1920 && DestR.left == 1920, "dest clips at the seam\n");
    ok(SrcL.left == Layout.Source.left && SrcR.right == Layout.Source.right,
       "outer source edges must match the crop\n");
    ok(SrcL.right == SrcR.left, "seam reset: left ends %ld, right starts %ld\n",
       SrcL.right, SrcR.left);
    ok(SrcL.top == SrcR.top && SrcL.bottom == SrcR.bottom,
       "rows must stay continuous across the seam\n");
    ok(SrcL.right == 2000, "seam source column %ld, expected 2000\n", SrcL.right);
}

static void Test_NegativeX(void)
{
    WALLPAPER_SPAN_LAYOUT Layout;
    RECTL Virtual = { -1280, 0, 640, 1024 };
    RECTL Left = { -1280, 0, 0, 1024 };
    RECTL Right = { 0, 0, 640, 1024 };
    RECTL DestL, SrcL, DestR, SrcR;
    BOOL Ret;

    Ret = WallpaperSpanLayout(4000, 3000, &Virtual, &Layout);
    ok(Ret, "layout failed\n");
    if (!Ret)
        return;
    Ret = WallpaperSpanSample(&Layout, 4000, 3000, &Virtual, &Left, &DestL, &SrcL);
    ok(Ret, "left sample failed\n");
    Ret = WallpaperSpanSample(&Layout, 4000, 3000, &Virtual, &Right, &DestR, &SrcR);
    ok(Ret, "right sample failed\n");
    if (!Ret)
        return;
    ok(DestL.left == -1280 && DestL.right == 0, "signed dest (%ld,%ld)\n", DestL.left, DestL.right);
    ok(SrcL.left == Layout.Source.left, "the monitor left of the primary must sample the left edge\n");
    ok(SrcL.right == SrcR.left, "seam reset: left ends %ld, right starts %ld\n", SrcL.right, SrcR.left);
    ok(SrcR.right == Layout.Source.right, "right edge of the crop\n");
}

static void Test_Stacked(void)
{
    WALLPAPER_SPAN_LAYOUT Layout;
    RECTL Virtual = { 0, -1024, 1280, 800 };
    RECTL Top = { 0, -1024, 1280, 0 };
    RECTL Bottom = { 0, 0, 1280, 800 };
    RECTL DestT, SrcT, DestB, SrcB;
    BOOL Ret;

    Ret = WallpaperSpanLayout(1280, 1824, &Virtual, &Layout);
    ok(Ret, "layout failed\n");
    if (!Ret)
        return;
    ok(Layout.Source.left == 0 && Layout.Source.top == 0 &&
       Layout.Source.right == 1280 && Layout.Source.bottom == 1824,
       "same-aspect image must not crop: (%ld,%ld)-(%ld,%ld)\n",
       Layout.Source.left, Layout.Source.top, Layout.Source.right, Layout.Source.bottom);

    Ret = WallpaperSpanSample(&Layout, 1280, 1824, &Virtual, &Top, &DestT, &SrcT);
    ok(Ret, "top sample failed\n");
    Ret = WallpaperSpanSample(&Layout, 1280, 1824, &Virtual, &Bottom, &DestB, &SrcB);
    ok(Ret, "bottom sample failed\n");
    if (!Ret)
        return;
    ok(SrcT.bottom == SrcB.top, "seam reset: top ends %ld, bottom starts %ld\n", SrcT.bottom, SrcB.top);
    ok(SrcT.top == 0 && SrcB.bottom == 1824, "column edges must match the crop\n");
    ok(SrcB.top == 1024, "bottom monitor starts at source row %ld, expected 1024\n", SrcB.top);
}

static void Test_GapsAndClips(void)
{
    WALLPAPER_SPAN_LAYOUT Layout;
    RECTL Virtual = { 0, 0, 3200, 1080 };
    RECTL Short = { 1920, 0, 3200, 720 };
    RECTL Outside = { 4000, 0, 5000, 1000 };
    RECTL Dest, Source;
    BOOL Ret;

    Ret = WallpaperSpanLayout(4000, 3000, &Virtual, &Layout);
    ok(Ret, "layout failed\n");
    if (!Ret)
        return;

    Ret = WallpaperSpanSample(&Layout, 4000, 3000, &Virtual, &Short, &Dest, &Source);
    ok(Ret, "short monitor sample failed\n");
    if (Ret)
    {
        ok(Dest.bottom == 720, "dest must clip to the monitor, got bottom %ld\n", Dest.bottom);
        ok(Source.bottom == 825 + 720 * (Layout.Source.bottom - Layout.Source.top) / 1080,
           "source rows must follow the monitor, got %ld\n", Source.bottom);
    }

    Ret = WallpaperSpanSample(&Layout, 4000, 3000, &Virtual, &Outside, &Dest, &Source);
    ok(!Ret, "a monitor outside the virtual bounds must not sample\n");
}

static void Test_Guards(void)
{
    WALLPAPER_SPAN_LAYOUT Layout;
    RECTL Virtual = { 0, 0, 1280, 800 };
    RECTL Empty = { 0, 0, 0, 800 };
    RECTL Inverted = { 0, 0, -5, 800 };
    RECTL Monitor = { 0, 0, 1280, 800 };
    RECTL Dest, Source;
    BOOL Ret;

    ok(!WallpaperSpanLayout(0, 3000, &Virtual, &Layout), "zero image width accepted\n");
    ok(!WallpaperSpanLayout(4000, 0, &Virtual, &Layout), "zero image height accepted\n");
    ok(!WallpaperSpanLayout(4000, 3000, &Empty, &Layout), "empty canvas accepted\n");
    ok(!WallpaperSpanLayout(4000, 3000, &Inverted, &Layout), "inverted canvas accepted\n");
    ok(!WallpaperSpanLayout(4000, 3000, NULL, &Layout), "NULL bounds accepted\n");
    ok(!WallpaperSpanLayout(4000, 3000, &Virtual, NULL), "NULL layout accepted\n");

    Ret = WallpaperSpanLayout(4000, 3000, &Virtual, &Layout);
    ok(Ret, "layout failed\n");
    if (!Ret)
        return;
    ok(!WallpaperSpanSample(NULL, 4000, 3000, &Virtual, &Monitor, &Dest, &Source),
       "NULL layout sampled\n");
    ok(!WallpaperSpanSample(&Layout, 4000, 3000, &Virtual, &Monitor, NULL, &Source),
       "NULL dest accepted\n");

    Ret = WallpaperSpanLayout(100000, 100000, &Virtual, &Layout);
    ok(Ret, "large image layout failed\n");
    if (Ret)
        ok(Layout.Source.left >= 0 && Layout.Source.right <= 100000,
           "crop escaped the image: (%ld,%ld)\n", Layout.Source.left, Layout.Source.right);
}

static void Test_OverflowGuards(void)
{
    WALLPAPER_SPAN_LAYOUT Layout;
    RECTL WideCanvas = { 0, 0, 1000, 10000 };
    RECTL ExactMax = { 0, 0, LONG_MAX, 1 };
    RECTL HugeSignedX = { LONG_MIN, 0, LONG_MAX, 100 };
    RECTL HugeSignedY = { 0, LONG_MIN, 100, LONG_MAX };
    RECTL Monitor = { 0, 0, 1000, 10 };
    RECTL Dest, Source;
    BOOL Ret;

    /* The exact scaled width is 4,295,000,000: unrepresentable, must fail. */
    ok(!WallpaperSpanLayout(429500, 1, &WideCanvas, &Layout),
       "overflowing scaled width accepted\n");

    Ret = WallpaperSpanLayout(4294, 10, &WideCanvas, &Layout);
    ok(Ret, "representable high-aspect layout failed\n");
    if (Ret)
        ok(Layout.Source.left >= 0 && Layout.Source.right > Layout.Source.left &&
           Layout.Source.right <= 4294 && Layout.Source.bottom <= 10,
           "crop escaped the image: (%ld,%ld)-(%ld,%ld)\n",
           Layout.Source.left, Layout.Source.top,
           Layout.Source.right, Layout.Source.bottom);

    /* Dwight M1: the exact-scaled crop is degenerate here (top == bottom), so reject. */
    ok(!WallpaperSpanLayout(10, 10, &ExactMax, &Layout),
       "degenerate LONG_MAX canvas crop accepted\n");

    /* Dwight M1: high-aspect 4294x1 scales to a zero-width crop, so reject. */
    ok(!WallpaperSpanLayout(4294, 1, &WideCanvas, &Layout),
       "zero-width high-aspect crop accepted\n");

    ok(!WallpaperSpanLayout(10, 10, &HugeSignedX, &Layout),
       "canvas wider than LONG_MAX accepted\n");
    ok(!WallpaperSpanLayout(10, 10, &HugeSignedY, &Layout),
       "canvas taller than LONG_MAX accepted\n");

    Layout.ScaleNum = 1;
    Layout.ScaleDen = 1;
    Layout.Source.left = LONG_MIN;
    Layout.Source.top = 0;
    Layout.Source.right = LONG_MAX;
    Layout.Source.bottom = 10;
    ok(!WallpaperSpanSample(&Layout, 10, 10, &WideCanvas, &Monitor, &Dest, &Source),
       "overflowing source extent sampled\n");
}

START_TEST(SpanGeometry)
{
    Test_OneMonitorEqualsFill();
    Test_SideBySideSeam();
    Test_NegativeX();
    Test_Stacked();
    Test_GapsAndClips();
    Test_Guards();
    Test_OverflowGuards();
}

/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the x86 msvcrt _libm_sse2_*_precise exports
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "../crt/math_helpers.h"
#include <emmintrin.h>
#include <errno.h>

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_acos_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_asin_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_atan_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_cos_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_exp_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_log_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_log10_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_pow_precise(__m128d Xmm0, __m128d Xmm1);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_sin_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_sqrt_precise(__m128d Xmm0);
__ATTRIBUTE_SSE2__ __m128d _libm_sse2_tan_precise(__m128d Xmm0);

typedef __m128d (*PLIBM_SSE2_UNARY)(__m128d);

static const struct
{
    const char *Name;
    PLIBM_SSE2_UNARY Function;
    double X;
    UINT64 Expected;
    int Errno;
} s_unary_tests[] =
{
    { "acos", _libm_sse2_acos_precise, 0.5, 0x3ff0c152382d7366, 0 },
    { "acos", _libm_sse2_acos_precise, 100.0, 0x7ff8000000000000, EDOM },
    { "acos", _libm_sse2_acos_precise, -1.0, 0x400921fb54442d18, 0 },
    { "acos", _libm_sse2_acos_precise, 0.0, 0x3ff921fb54442d18, 0 },
    { "acos", _libm_sse2_acos_precise, 1000.0, 0x7ff8000000000000, EDOM },
    { "asin", _libm_sse2_asin_precise, 0.5, 0x3fe0c152382d7366, 0 },
    { "asin", _libm_sse2_asin_precise, 100.0, 0x7ff8000000000000, EDOM },
    { "asin", _libm_sse2_asin_precise, -1.0, 0xbff921fb54442d18, 0 },
    { "asin", _libm_sse2_asin_precise, 0.0, 0x0000000000000000, 0 },
    { "asin", _libm_sse2_asin_precise, 1000.0, 0x7ff8000000000000, EDOM },
    { "atan", _libm_sse2_atan_precise, 0.5, 0x3fddac670561bb4f, 0 },
    { "atan", _libm_sse2_atan_precise, 100.0, 0x3ff8f905eb2def22, 0 },
    { "atan", _libm_sse2_atan_precise, -1.0, 0xbfe921fb54442d18, 0 },
    { "atan", _libm_sse2_atan_precise, 0.0, 0x0000000000000000, 0 },
    { "atan", _libm_sse2_atan_precise, 1000.0, 0x3ff91de2c0e658bd, 0 },
    { "cos", _libm_sse2_cos_precise, 0.5, 0x3fec1528065b7d50, 0 },
    { "cos", _libm_sse2_cos_precise, 100.0, 0x3feb981dbf665fdf, 0 },
    { "cos", _libm_sse2_cos_precise, -1.0, 0x3fe14a280fb5068c, 0 },
    { "cos", _libm_sse2_cos_precise, 0.0, 0x3ff0000000000000, 0 },
    { "cos", _libm_sse2_cos_precise, 1000.0, 0x3fe1ff026793f1bb, 0 },
    { "exp", _libm_sse2_exp_precise, 0.5, 0x3ffa61298e1e069c, 0 },
    { "exp", _libm_sse2_exp_precise, 100.0, 0x48f3494a9b171bf5, 0 },
    { "exp", _libm_sse2_exp_precise, -1.0, 0x3fd78b56362cef38, 0 },
    { "exp", _libm_sse2_exp_precise, 0.0, 0x3ff0000000000000, 0 },
    { "exp", _libm_sse2_exp_precise, 1000.0, 0x7ff0000000000000, ERANGE },
    { "log10", _libm_sse2_log10_precise, 0.5, 0xbfd34413509f79ff, 0 },
    { "log10", _libm_sse2_log10_precise, 100.0, 0x4000000000000000, 0 },
    { "log10", _libm_sse2_log10_precise, -1.0, 0x7ff8000000000000, EDOM },
    { "log10", _libm_sse2_log10_precise, 0.0, 0xfff0000000000000, ERANGE },
    { "log10", _libm_sse2_log10_precise, 1000.0, 0x4008000000000000, 0 },
    { "log", _libm_sse2_log_precise, 0.5, 0xbfe62e42fefa39ef, 0 },
    { "log", _libm_sse2_log_precise, 100.0, 0x40126bb1bbb55516, 0 },
    { "log", _libm_sse2_log_precise, -1.0, 0x7ff8000000000000, EDOM },
    { "log", _libm_sse2_log_precise, 0.0, 0xfff0000000000000, ERANGE },
    { "log", _libm_sse2_log_precise, 1000.0, 0x401ba18a998fffa0, 0 },
    { "sin", _libm_sse2_sin_precise, 0.5, 0x3fdeaee8744b05f0, 0 },
    { "sin", _libm_sse2_sin_precise, 100.0, 0xbfe03425b78c4db8, 0 },
    { "sin", _libm_sse2_sin_precise, -1.0, 0xbfeaed548f090cee, 0 },
    { "sin", _libm_sse2_sin_precise, 0.0, 0x0000000000000000, 0 },
    { "sin", _libm_sse2_sin_precise, 1000.0, 0x3fea75cc150a206b, 0 },
    { "sqrt", _libm_sse2_sqrt_precise, 0.5, 0x3fe6a09e667f3bcd, 0 },
    { "sqrt", _libm_sse2_sqrt_precise, 100.0, 0x4024000000000000, 0 },
    { "sqrt", _libm_sse2_sqrt_precise, -1.0, 0xfff8000000000000, 0 },
    { "sqrt", _libm_sse2_sqrt_precise, 0.0, 0x0000000000000000, 0 },
    { "sqrt", _libm_sse2_sqrt_precise, 1000.0, 0x403f9f6e4990f227, 0 },
    { "tan", _libm_sse2_tan_precise, 0.5, 0x3fe17b4f5bf3474a, 0 },
    { "tan", _libm_sse2_tan_precise, 100.0, 0xbfe2ca74d62b5d38, 0 },
    { "tan", _libm_sse2_tan_precise, -1.0, 0xbff8eb245cbee3a6, 0 },
    { "tan", _libm_sse2_tan_precise, 0.0, 0x0000000000000000, 0 },
    { "tan", _libm_sse2_tan_precise, 1000.0, 0x3ff786729f34311a, 0 },
    { "acos", _libm_sse2_acos_precise, INFINITY, 0x7ff8000000000000, EDOM },
    { "asin", _libm_sse2_asin_precise, -INFINITY, 0x7ff8000000000000, EDOM },
    { "atan", _libm_sse2_atan_precise, INFINITY, 0x3ff921fb54442d18, 0 },
    { "atan", _libm_sse2_atan_precise, -INFINITY, 0xbff921fb54442d18, 0 },
    { "cos", _libm_sse2_cos_precise, INFINITY, 0xfff8000000000000, EDOM },
    { "exp", _libm_sse2_exp_precise, -1000.0, 0x0000000000000000, 0 },
    { "exp", _libm_sse2_exp_precise, INFINITY, 0x7ff0000000000000, 0 },
    { "exp", _libm_sse2_exp_precise, -INFINITY, 0x0000000000000000, 0 },
    { "exp", _libm_sse2_exp_precise, 709.0, 0x7fdd422d2be5dc9b, 0 },
    { "exp", _libm_sse2_exp_precise, 710.0, 0x7ff0000000000000, ERANGE },
    { "exp", _libm_sse2_exp_precise, -745.0, 0x0000000000000001, 0 },
    { "exp", _libm_sse2_exp_precise, -746.0, 0x0000000000000000, 0 },
    { "log10", _libm_sse2_log10_precise, INFINITY, 0x7ff0000000000000, 0 },
    { "log10", _libm_sse2_log10_precise, -INFINITY, 0x7ff8000000000000, EDOM },
    { "log10", _libm_sse2_log10_precise, -0.0, 0xfff0000000000000, ERANGE },
    { "log", _libm_sse2_log_precise, INFINITY, 0x7ff0000000000000, 0 },
    { "log", _libm_sse2_log_precise, -0.0, 0xfff0000000000000, ERANGE },
    { "sin", _libm_sse2_sin_precise, INFINITY, 0xfff8000000000000, EDOM },
    { "sqrt", _libm_sse2_sqrt_precise, INFINITY, 0x7ff0000000000000, 0 },
    { "sqrt", _libm_sse2_sqrt_precise, -INFINITY, 0xfff8000000000000, 0 },
    { "sqrt", _libm_sse2_sqrt_precise, -0.0, 0x8000000000000000, 0 },
    { "tan", _libm_sse2_tan_precise, -INFINITY, 0xfff8000000000000, EDOM },
};

static const struct
{
    double X;
    double Y;
    UINT64 Expected;
    int Errno;
} s_pow_tests[] =
{
    { 2.0, 3.0, 0x4020000000000000, 0 },
    { -2.0, 0.5, 0xfff8000000000000, EDOM },
    { 0.0, -1.0, 0x7ff0000000000000, ERANGE },
    { 2.0, 2000.0, 0x7ff0000000000000, ERANGE },
    { 2.0, -2000.0, 0x0000000000000000, 0 },
    { -8.0, 3.0, 0xc080000000000000, 0 },
    { 0.0, 0.0, 0x3ff0000000000000, 0 },
    { -INFINITY, 3.0, 0xfff0000000000000, 0 },
    { INFINITY, -1.0, 0x0000000000000000, 0 },
    { -0.0, -1.0, 0xfff0000000000000, ERANGE },
    { 1.0, INFINITY, 0x3ff0000000000000, EDOM },
    { -2.0, INFINITY, 0x7ff0000000000000, 0 },
    { 0.5, -INFINITY, 0x7ff0000000000000, 0 },
    { 0.0, -INFINITY, 0x7ff0000000000000, 0 },
};

__ATTRIBUTE_SSE2__
static void Test_unary(void)
{
    int i;

    for (i = 0; i < _countof(s_unary_tests); i++)
    {
        __m128d Result;
        UINT64 Bits;

        errno = 0;
        Result = s_unary_tests[i].Function(_mm_set_sd(s_unary_tests[i].X));
        Bits = dbl_to_u64(_mm_cvtsd_f64(Result));
        ok(Bits == s_unary_tests[i].Expected,
           "_libm_sse2_%s_precise(%g) = 0x%I64x, expected 0x%I64x\n",
           s_unary_tests[i].Name, s_unary_tests[i].X, Bits, s_unary_tests[i].Expected);
        ok(errno == s_unary_tests[i].Errno, "_libm_sse2_%s_precise(%g) errno = %d, expected %d\n",
           s_unary_tests[i].Name, s_unary_tests[i].X, errno, s_unary_tests[i].Errno);
    }
}

__ATTRIBUTE_SSE2__
static void Test_pow(void)
{
    int i;

    for (i = 0; i < _countof(s_pow_tests); i++)
    {
        __m128d Result;
        UINT64 Bits;

        errno = 0;
        Result = _libm_sse2_pow_precise(_mm_set_sd(s_pow_tests[i].X), _mm_set_sd(s_pow_tests[i].Y));
        Bits = dbl_to_u64(_mm_cvtsd_f64(Result));
        ok(Bits == s_pow_tests[i].Expected,
           "_libm_sse2_pow_precise(%g, %g) = 0x%I64x, expected 0x%I64x\n",
           s_pow_tests[i].X, s_pow_tests[i].Y, Bits, s_pow_tests[i].Expected);
        ok(errno == s_pow_tests[i].Errno, "_libm_sse2_pow_precise(%g, %g) errno = %d, expected %d\n",
           s_pow_tests[i].X, s_pow_tests[i].Y, errno, s_pow_tests[i].Errno);
    }
}

START_TEST(libm_sse2_precise)
{
    Test_unary();
    Test_pow();
}

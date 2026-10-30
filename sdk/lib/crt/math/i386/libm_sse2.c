/*
 * PROJECT:     ReactOS CRT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Simplified implementation of __libm_sse2_*
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <emmintrin.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(acos,asin,atan,atan2,cos)
#pragma function(exp,log,log10,pow,sin,tan)
#define __ATTRIBUTE_SSE2__
#else
#define __ATTRIBUTE_SSE2__ __attribute__((__target__("sse2")))
#endif

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wuninitialized"
#endif

double __cdecl __acrt_report_math_error(int type, char const* name, double arg1, double arg2, double retval, int error);

static __inline double libm_sse2_from_bits(uint64_t bits)
{
    union { uint64_t i; double f; } u = { bits };
    return u.f;
}

double __cdecl __libm_sse2_core_acos(double x);
double __cdecl __libm_sse2_core_asin(double x);
double __cdecl __libm_sse2_core_atan(double x);
double __cdecl __libm_sse2_core_cos(double x);
double __cdecl __libm_sse2_core_exp(double x);
double __cdecl __libm_sse2_core_log(double x);
double __cdecl __libm_sse2_core_log10(double x);
double __cdecl __libm_sse2_core_pow(double x, double y);
double __cdecl __libm_sse2_core_sin(double x);
double __cdecl __libm_sse2_core_tan(double x);

#define LIBM_SSE2_QNAN libm_sse2_from_bits(0x7ff8000000000000ULL)
#define LIBM_SSE2_INDEFINITE libm_sse2_from_bits(0xfff8000000000000ULL)

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_acos(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = acos(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_acosf(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = acos(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_asin(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = asin(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_asinf(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = asin(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_atan(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = atan(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_atanf(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = atan(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_atan2(__m128d Xmm0, __m128d Xmm1)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double y = _mm_cvtsd_f64(Xmm1);
    double result = atan2(x, y);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_cos(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = cos(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_cosf(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = cos(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_exp(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = exp(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_expf(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = exp(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_log(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = log(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_logf(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = log(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_log10(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = log10(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_log10f(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = log10(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_pow(__m128d Xmm0, __m128d Xmm1)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double y = _mm_cvtsd_f64(Xmm1);
    double result = pow(x, y);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_powf(__m128 Xmm0, __m128 Xmm1)
{
    float x = _mm_cvtss_f32(Xmm0);
    float y = _mm_cvtss_f32(Xmm1);
    float result = powf(x, y);
    return _mm_set_ss(result);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_sin(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = sin(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_sinf(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = sin(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d __libm_sse2_tan(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double result = tan(x);
    return _mm_set_sd(result);
}

__ATTRIBUTE_SSE2__ __m128 __libm_sse2_tanf(__m128 Xmm0)
{
    __m128d Xmm0d = _mm_cvtss_sd(Xmm0d, Xmm0);
    double x = _mm_cvtsd_f64(Xmm0d);
    double result = tan(x);
    __m128d result128 = _mm_set_sd(result);
    return _mm_cvtpd_ps(result128);
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_acos_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    if (fabs(x) > 1.0)
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "acos", x, 0.0, LIBM_SSE2_QNAN, EDOM));
    return _mm_set_sd(__libm_sse2_core_acos(x));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_asin_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    if (fabs(x) > 1.0)
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "asin", x, 0.0, LIBM_SSE2_QNAN, EDOM));
    return _mm_set_sd(__libm_sse2_core_asin(x));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_atan_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    return _mm_set_sd(__libm_sse2_core_atan(x));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_cos_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    if (fabs(x) == INFINITY)
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "cos", x, 0.0, LIBM_SSE2_INDEFINITE, EDOM));
    return _mm_set_sd(__libm_sse2_core_cos(x));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_exp_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    return _mm_set_sd(__libm_sse2_core_exp(x));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_log_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    if (x < 0.0)
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "log", x, 0.0, LIBM_SSE2_QNAN, EDOM));
    if (x == 0.0)
        return _mm_set_sd(__acrt_report_math_error(_SING, "log", x, 0.0, -INFINITY, ERANGE));
    return _mm_set_sd(__libm_sse2_core_log(x));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_log10_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    if (x < 0.0)
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "log10", x, 0.0, LIBM_SSE2_QNAN, EDOM));
    if (x == 0.0)
        return _mm_set_sd(__acrt_report_math_error(_SING, "log10", x, 0.0, -INFINITY, ERANGE));
    return _mm_set_sd(__libm_sse2_core_log10(x));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_pow_precise(__m128d Xmm0, __m128d Xmm1)
{
    double x = _mm_cvtsd_f64(Xmm0);
    double y = _mm_cvtsd_f64(Xmm1);
    double result;

    if (x == 1.0 && fabs(y) == INFINITY)
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "pow", x, y, 1.0, EDOM));
    if (x < 0.0 && x != -INFINITY && fabs(y) != INFINITY && y != floor(y))
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "pow", x, y, LIBM_SSE2_INDEFINITE, EDOM));
    if (x == 0.0 && y < 0.0 && y != -INFINITY)
    {
        int odd = (y == floor(y) && floor(y * 0.5) != y * 0.5);
        result = (odd && 1.0 / x < 0.0) ? -INFINITY : INFINITY;
        return _mm_set_sd(__acrt_report_math_error(_SING, "pow", x, y, result, ERANGE));
    }
    return _mm_set_sd(__libm_sse2_core_pow(x, y));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_sin_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    if (fabs(x) == INFINITY)
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "sin", x, 0.0, LIBM_SSE2_INDEFINITE, EDOM));
    return _mm_set_sd(__libm_sse2_core_sin(x));
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_sqrt_precise(__m128d Xmm0)
{
    return _mm_sqrt_sd(Xmm0, Xmm0);
}

__ATTRIBUTE_SSE2__ __m128d _libm_sse2_tan_precise(__m128d Xmm0)
{
    double x = _mm_cvtsd_f64(Xmm0);
    if (fabs(x) == INFINITY)
        return _mm_set_sd(__acrt_report_math_error(_DOMAIN, "tan", x, 0.0, LIBM_SSE2_INDEFINITE, EDOM));
    return _mm_set_sd(__libm_sse2_core_tan(x));
}

#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(pow)
#endif

/*
 * @implemented
 */
double	CDECL	_CIpow(void)
{
	FPU_DOUBLES(x, y);
	return pow(x, y);
}

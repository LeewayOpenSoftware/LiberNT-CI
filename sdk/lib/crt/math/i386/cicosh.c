#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(cosh)
#endif

/*
 * @implemented
 */
double	CDECL	_CIcosh(void)
{
	FPU_DOUBLE(x);
	return cosh(x);
}

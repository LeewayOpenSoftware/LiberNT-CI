#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(exp)
#endif

/*
 * @implemented
 */
double	CDECL	_CIexp(void)
{
	FPU_DOUBLE(x);
	return exp(x);
}

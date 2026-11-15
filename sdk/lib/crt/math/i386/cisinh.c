#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(sinh)
#endif

/*
 * @implemented
 */
double	CDECL	_CIsinh(void)
{
	FPU_DOUBLE(x);
	return sinh(x);
}

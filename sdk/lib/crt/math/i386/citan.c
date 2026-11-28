#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(tan)
#endif

/*
 * @implemented
 */
double	CDECL	_CItan(void)
{
	FPU_DOUBLE(x);
	return tan(x);
}

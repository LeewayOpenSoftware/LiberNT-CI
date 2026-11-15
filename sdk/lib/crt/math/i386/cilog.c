#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(log)
#endif

/*
 * @implemented
 */
double	CDECL	_CIlog(void)
{
	FPU_DOUBLE(x);
	return log(x);
}

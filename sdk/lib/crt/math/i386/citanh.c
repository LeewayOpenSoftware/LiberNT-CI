#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(tanh)
#endif

/*
 * @implemented
 */
double	CDECL	_CItanh(void)
{
	FPU_DOUBLE(x);
	return tanh(x);
}

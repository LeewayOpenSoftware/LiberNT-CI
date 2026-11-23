#ifndef _SECURITY_H
#define _SECURITY_H

#include <sspi.h>

#define NEGOSSP_NAME_W L"Negotiate"
#define NEGOSSP_NAME_A "Negotiate"
#ifdef UNICODE
#define NEGOSSP_NAME NEGOSSP_NAME_W
#else
#define NEGOSSP_NAME NEGOSSP_NAME_A
#endif

#if defined(SECURITY_WIN32) || defined(SECURITY_KERNEL)
#include <secext.h>
#endif

#endif /* _SECURITY_H */

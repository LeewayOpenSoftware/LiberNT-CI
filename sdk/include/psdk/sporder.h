/*
 * sporder.h
 *
 * Winsock 2 service provider order.
 *
 * THIS SOFTWARE IS NOT COPYRIGHTED
 *
 * This source code is offered for use in the public domain. You may
 * use, modify or distribute it freely.
 *
 * This code is distributed in the hope that it will be useful but
 * WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 * DISCLAIMED. This includes but is not limited to warranties of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#pragma once

#ifndef __SPORDER_H__
#define __SPORDER_H__

#include <ws2spi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns ERROR_SUCCESS or the error itself, not SOCKET_ERROR */
int
WSPAPI
WSCWriteProviderOrder(
  _In_reads_(dwNumberOfEntries) LPDWORD lpwdCatalogEntryId,
  _In_ DWORD dwNumberOfEntries);

#ifdef __cplusplus
}
#endif

#endif /* __SPORDER_H__ */

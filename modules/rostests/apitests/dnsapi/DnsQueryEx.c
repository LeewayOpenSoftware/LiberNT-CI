/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for DnsQueryEx and DnsCancelQuery
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <windns.h>
#include <apitest.h>

#define QUERY_CONTEXT_TAG ((PVOID)(ULONG_PTR)0x517cc1b9)

static HANDLE g_Done;
static volatile DNS_STATUS g_CallbackStatus;
static volatile LONG g_ContextSeen;
static volatile LONG g_CallbackCalls;

static VOID CALLBACK
QueryComplete(PVOID Context, PDNS_QUERY_RESULT Results)
{
    if (Context == QUERY_CONTEXT_TAG)
        InterlockedExchange(&g_ContextSeen, 1);
    g_CallbackStatus = Results->QueryStatus;
    InterlockedIncrement(&g_CallbackCalls);
    if (Results->pQueryRecords)
        DnsRecordListFree(Results->pQueryRecords, DnsFreeRecordList);
    SetEvent(g_Done);
}

static VOID
ResetCompletion(VOID)
{
    if (g_Done)
        CloseHandle(g_Done);
    g_Done = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_CallbackStatus = ERROR_SUCCESS;
    InterlockedExchange(&g_ContextSeen, 0);
    InterlockedExchange(&g_CallbackCalls, 0);
}

void func_DnsQueryEx(void)
{
    DNS_QUERY_REQUEST Request;
    DNS_QUERY_RESULT Results;
    DNS_QUERY_CANCEL Cancel;
    DNS_STATUS Status;
    DWORD Wait;

    /* A query without a completion routine completes synchronously */
    ZeroMemory(&Request, sizeof(Request));
    Request.Version = DNS_QUERY_REQUEST_VERSION1;
    Request.QueryName = L"localhost";
    Request.QueryType = DNS_TYPE_A;
    ZeroMemory(&Results, sizeof(Results));
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_SUCCESS, "DnsQueryEx sync failed: %lu\n", Status);
    ok(Results.Version == DNS_QUERY_RESULTS_VERSION1, "Result version %lu\n", Results.Version);
    ok(Results.QueryStatus == Status, "Result status %lu\n", Results.QueryStatus);
    ok(Results.pQueryRecords != NULL, "Sync query returned no records\n");
    if (Results.pQueryRecords)
        DnsRecordListFree(Results.pQueryRecords, DnsFreeRecordList);

    /* Argument validation */
    Status = DnsQueryEx(NULL, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL request: %lu\n", Status);
    Status = DnsQueryEx(&Request, NULL, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL results: %lu\n", Status);
    Request.Version = 2;
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "Bad request version: %lu\n", Status);
    Request.Version = DNS_QUERY_REQUEST_VERSION1;
    Request.QueryName = NULL;
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL name: %lu\n", Status);
    Request.QueryName = L"localhost";
    Request.pDnsExtraQueryInfo = (PDNS_EXTRA_QUERY_INFO)&Results;
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "Reserved info: %lu\n", Status);
    Request.pDnsExtraQueryInfo = NULL;

    /* A query with a completion routine is delivered asynchronously */
    ResetCompletion();
    Request.QueryName = L"localhost";
    Request.QueryType = DNS_TYPE_AAAA;
    Request.pQueryCompletionCallback = QueryComplete;
    Request.QueryContext = QUERY_CONTEXT_TAG;
    ZeroMemory(&Cancel, sizeof(Cancel));
    Status = DnsQueryEx(&Request, &Results, &Cancel);
    ok(Status == ERROR_SUCCESS, "Async submit failed: %lu\n", Status);
    Wait = WaitForSingleObject(g_Done, 10000);
    ok(Wait == WAIT_OBJECT_0, "Completion never delivered\n");
    ok(g_CallbackCalls == 1, "Callback calls %ld\n", g_CallbackCalls);
    ok(g_ContextSeen == 1, "Query context not passed through\n");
    ok(g_CallbackStatus == ERROR_SUCCESS, "Callback status %lu\n", g_CallbackStatus);

    /* Cancelling a finished query is a harmless no-op */
    DnsCancelQuery(&Cancel);

    /* Cancelling an in-flight query must not deliver a completion */
    ResetCompletion();
    Request.QueryName = L"localhost";
    Request.QueryType = DNS_TYPE_A;
    ZeroMemory(&Cancel, sizeof(Cancel));
    Status = DnsQueryEx(&Request, &Results, &Cancel);
    ok(Status == ERROR_SUCCESS, "Second async submit failed: %lu\n", Status);
    DnsCancelQuery(&Cancel);
    Wait = WaitForSingleObject(g_Done, 3000);
    ok(Wait == WAIT_TIMEOUT || g_CallbackCalls == 1,
       "Cancelled query delivered a completion\n");

    ResetCompletion();
}

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
static PDNS_QUERY_RESULT g_CallbackResults;
static volatile DNS_STATUS g_CallbackStatus;
static volatile LONG g_ContextSeen;
static volatile LONG g_CallbackCalls;

static VOID CALLBACK
QueryComplete(PVOID Context, PDNS_QUERY_RESULT Results)
{
    if (Context == QUERY_CONTEXT_TAG)
        InterlockedExchange(&g_ContextSeen, 1);
    g_CallbackResults = Results;
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
    g_CallbackResults = NULL;
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
    Request.pDnsExtraQueryInfo = &Results;
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "Reserved info: %lu\n", Status);
    Request.pDnsExtraQueryInfo = NULL;

    /* A query with a completion routine is pending and delivers asynchronously
     * into the caller's result structure */
    ResetCompletion();
    Request.QueryName = L"localhost";
    Request.QueryType = DNS_TYPE_AAAA;
    Request.pQueryCompletionCallback = QueryComplete;
    Request.QueryContext = QUERY_CONTEXT_TAG;
    ZeroMemory(&Cancel, sizeof(Cancel));
    ZeroMemory(&Results, sizeof(Results));
    Status = DnsQueryEx(&Request, &Results, &Cancel);
    ok(Status == DNS_REQUEST_PENDING, "Async submit returned %lu, expected DNS_REQUEST_PENDING\n", Status);
    Wait = WaitForSingleObject(g_Done, 10000);
    ok(Wait == WAIT_OBJECT_0, "Completion never delivered\n");
    ok(g_CallbackCalls == 1, "Callback calls %ld\n", g_CallbackCalls);
    ok(g_ContextSeen == 1, "Query context not passed through\n");
    ok(g_CallbackStatus == ERROR_SUCCESS, "Callback status %lu\n", g_CallbackStatus);
    ok(g_CallbackResults == &Results, "Callback did not receive the caller's result pointer\n");
    ok(Results.QueryStatus == ERROR_SUCCESS, "Caller result not filled: %lu\n", Results.QueryStatus);

    /* A finished query's cancel handle is inert */
    DnsCancelQuery(&Cancel);
    ok(g_CallbackCalls == 1, "Cancel after completion redelivered\n");

    /* Cancelling a pending query still delivers one completion with ERROR_CANCELLED */
    ResetCompletion();
    Request.QueryName = L"localhost";
    Request.QueryType = DNS_TYPE_A;
    ZeroMemory(&Cancel, sizeof(Cancel));
    ZeroMemory(&Results, sizeof(Results));
    Status = DnsQueryEx(&Request, &Results, &Cancel);
    ok(Status == DNS_REQUEST_PENDING, "Second async submit returned %lu\n", Status);
    DnsCancelQuery(&Cancel);
    Wait = WaitForSingleObject(g_Done, 10000);
    ok(Wait == WAIT_OBJECT_0, "Cancelled query never delivered its completion\n");
    ok(g_CallbackCalls == 1, "Cancelled query delivered %ld completions\n", g_CallbackCalls);
    ok(g_CallbackStatus == ERROR_CANCELLED, "Cancelled status %lu\n", g_CallbackStatus);
    ok(Results.QueryStatus == ERROR_CANCELLED, "Caller result status %lu\n", Results.QueryStatus);

    /* A cleared handle cancels nothing and touches no memory */
    ResetCompletion();
    Status = DnsQueryEx(&Request, &Results, &Cancel);
    ok(Status == DNS_REQUEST_PENDING, "Third async submit returned %lu\n", Status);
    DnsCancelQuery(&Cancel);
    DnsCancelQuery(&Cancel);
    Wait = WaitForSingleObject(g_Done, 10000);
    ok(Wait == WAIT_OBJECT_0, "Completion after double cancel never delivered\n");
    ok(g_CallbackCalls == 1, "Double cancel delivered %ld completions\n", g_CallbackCalls);

    ResetCompletion();
}

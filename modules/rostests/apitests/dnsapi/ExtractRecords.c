/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for DnsExtractRecordsFromMessage_W/UTF8
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <windns.h>
#include <apitest.h>

/* www.example.com A IN, answered with a CNAME to example.com and an A record */
static const UCHAR TestMessage[] =
{
    0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01, 0x00, 0x01,
    0xC0, 0x0C,
    0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x0D,
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x04,
    0x01, 0x02, 0x03, 0x04
};

/* Same question, no answers */
static const UCHAR EmptyMessage[] =
{
    0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01, 0x00, 0x01
};

void func_ExtractRecords(void)
{
    PDNS_RECORD Records, Record;
    DNS_STATUS Status;

    Status = DnsExtractRecordsFromMessage_W(NULL, sizeof(TestMessage), &Records);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL buffer: %lu\n", Status);
    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)TestMessage, sizeof(TestMessage), NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL out param: %lu\n", Status);
    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)TestMessage, 10, &Records);
    ok(Status == ERROR_INVALID_DATA, "Short message: %lu\n", Status);
    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)TestMessage, sizeof(TestMessage) - 1, &Records);
    ok(Status == ERROR_INVALID_DATA, "Truncated message: %lu\n", Status);

    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)EmptyMessage, sizeof(EmptyMessage), &Records);
    ok(Status == DNS_INFO_NO_RECORDS, "Empty message: %lu\n", Status);

    Records = NULL;
    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)TestMessage, sizeof(TestMessage), &Records);
    ok(Status == ERROR_SUCCESS, "Extract failed: %lu\n", Status);
    ok(Records != NULL, "No records extracted\n");
    if (!Records)
        return;

    Record = Records;
    ok(Record->wType == DNS_TYPE_CNAME, "First type %u\n", Record->wType);
    ok(Record->Flags.S.Section == DnsSectionAnswer, "First section %lu\n", Record->Flags.S.Section);
    ok(Record->dwTtl == 300, "First TTL %lu\n", Record->dwTtl);
    ok(Record->pName && !wcscmp(Record->pName, L"www.example.com"), "First name %S\n",
       Record->pName ? Record->pName : L"(null)");
    ok(Record->Data.CNAME.pNameHost && !wcscmp(Record->Data.CNAME.pNameHost, L"example.com"),
       "CNAME target %S\n", Record->Data.CNAME.pNameHost ? Record->Data.CNAME.pNameHost : L"(null)");

    Record = Records->pNext;
    ok(Record != NULL, "Second record missing\n");
    if (Record)
    {
        ok(Record->wType == DNS_TYPE_A, "Second type %u\n", Record->wType);
        ok(Record->pName && !wcscmp(Record->pName, L"example.com"), "Second name %S\n",
           Record->pName ? Record->pName : L"(null)");
        ok(!memcmp(&Record->Data.A.IpAddress, "\x01\x02\x03\x04", 4), "A data mismatch\n");
        ok(Record->pNext == NULL, "Unexpected third record\n");
    }
    DnsRecordListFree(Records, DnsFreeRecordList);

    Records = NULL;
    Status = DnsExtractRecordsFromMessage_UTF8((PDNS_MESSAGE_BUFFER)TestMessage, sizeof(TestMessage), &Records);
    ok(Status == ERROR_SUCCESS, "UTF8 extract failed: %lu\n", Status);
    ok(Records != NULL, "No UTF8 records\n");
    if (Records)
    {
        Record = Records;
        ok(Record->pName && !strcmp(Record->pName, "www.example.com"), "UTF8 name %s\n",
           Record->pName ? Record->pName : "(null)");
        ok(Record->Data.CNAME.pNameHost && !strcmp(Record->Data.CNAME.pNameHost, "example.com"),
           "UTF8 CNAME target %s\n", Record->Data.CNAME.pNameHost ? Record->Data.CNAME.pNameHost : "(null)");
        Record = Records->pNext;
        ok(Record != NULL && Record->wType == DNS_TYPE_A, "UTF8 A record missing\n");
        if (Record)
            ok(!memcmp(&Record->Data.A.IpAddress, "\x01\x02\x03\x04", 4), "UTF8 A data mismatch\n");
    }
    DnsRecordListFree(Records, DnsFreeRecordList);
}

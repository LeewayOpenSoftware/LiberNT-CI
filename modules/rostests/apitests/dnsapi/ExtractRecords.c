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

/* www.example.com A IN, answered with a CNAME to example.com and an A record.
 * The caller applies DNS_BYTE_FLIP_HEADER_COUNTS, so the Xid and the header
 * counts are stored in host byte order; everything past the header stays
 * in network byte order. */
static const UCHAR TestMessage[] =
{
    0x34, 0x12, 0x81, 0x80, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
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
    0x34, 0x12, 0x81, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01, 0x00, 0x01
};

void func_ExtractRecords(void)
{
    PDNS_RECORDW RecordsW, RecordW;
    PDNS_RECORDA RecordsA, RecordA;
    DNS_STATUS Status;

    Status = DnsExtractRecordsFromMessage_W(NULL, sizeof(TestMessage), (PDNS_RECORD *)&RecordsW);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL buffer: %lu\n", Status);
    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)TestMessage, sizeof(TestMessage), NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL out param: %lu\n", Status);
    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)TestMessage, 10, (PDNS_RECORD *)&RecordsW);
    ok(Status == ERROR_INVALID_DATA, "Short message: %lu\n", Status);
    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)TestMessage, sizeof(TestMessage) - 1, (PDNS_RECORD *)&RecordsW);
    ok(Status == ERROR_INVALID_DATA, "Truncated message: %lu\n", Status);

    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)EmptyMessage, sizeof(EmptyMessage), (PDNS_RECORD *)&RecordsW);
    ok(Status == DNS_INFO_NO_RECORDS, "Empty message: %lu\n", Status);

    RecordsW = NULL;
    Status = DnsExtractRecordsFromMessage_W((PDNS_MESSAGE_BUFFER)TestMessage, sizeof(TestMessage), (PDNS_RECORD *)&RecordsW);
    ok(Status == ERROR_SUCCESS, "Extract failed: %lu\n", Status);
    ok(RecordsW != NULL, "No records extracted\n");
    if (!RecordsW)
        return;

    RecordW = RecordsW;
    ok(RecordW->wType == DNS_TYPE_CNAME, "First type %u\n", RecordW->wType);
    ok(RecordW->Flags.S.Section == DnsSectionAnswer, "First section %lu\n",
       (unsigned long)RecordW->Flags.S.Section);
    ok(RecordW->dwTtl == 300, "First TTL %lu\n", RecordW->dwTtl);
    ok(RecordW->pName && !wcscmp(RecordW->pName, L"www.example.com"), "First name %S\n",
       RecordW->pName ? RecordW->pName : L"(null)");
    ok(RecordW->Data.CNAME.pNameHost && !wcscmp(RecordW->Data.CNAME.pNameHost, L"example.com"),
       "CNAME target %S\n", RecordW->Data.CNAME.pNameHost ? RecordW->Data.CNAME.pNameHost : L"(null)");

    RecordW = RecordsW->pNext;
    ok(RecordW != NULL, "Second record missing\n");
    if (RecordW)
    {
        ok(RecordW->wType == DNS_TYPE_A, "Second type %u\n", RecordW->wType);
        ok(RecordW->pName && !wcscmp(RecordW->pName, L"example.com"), "Second name %S\n",
           RecordW->pName ? RecordW->pName : L"(null)");
        ok(!memcmp(&RecordW->Data.A.IpAddress, "\x01\x02\x03\x04", 4), "A data mismatch\n");
        ok(RecordW->pNext == NULL, "Unexpected third record\n");
    }
    DnsRecordListFree((PDNS_RECORD)RecordsW, DnsFreeRecordList);

    RecordsA = NULL;
    Status = DnsExtractRecordsFromMessage_UTF8((PDNS_MESSAGE_BUFFER)TestMessage, sizeof(TestMessage), (PDNS_RECORD *)&RecordsA);
    ok(Status == ERROR_SUCCESS, "UTF8 extract failed: %lu\n", Status);
    ok(RecordsA != NULL, "No UTF8 records\n");
    if (RecordsA)
    {
        RecordA = RecordsA;
        ok(RecordA->pName && !strcmp(RecordA->pName, "www.example.com"), "UTF8 name %s\n",
           RecordA->pName ? RecordA->pName : "(null)");
        ok(RecordA->Data.CNAME.pNameHost && !strcmp(RecordA->Data.CNAME.pNameHost, "example.com"),
           "UTF8 CNAME target %s\n", RecordA->Data.CNAME.pNameHost ? RecordA->Data.CNAME.pNameHost : "(null)");
        RecordA = RecordsA->pNext;
        ok(RecordA != NULL && RecordA->wType == DNS_TYPE_A, "UTF8 A record missing\n");
        if (RecordA)
            ok(!memcmp(&RecordA->Data.A.IpAddress, "\x01\x02\x03\x04", 4), "UTF8 A data mismatch\n");
    }
    DnsRecordListFree((PDNS_RECORD)RecordsA, DnsFreeRecordList);
}

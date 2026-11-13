/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     IopValidateMountPointBuffer: malformed mount-point reparse buffers fail cleanly
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ntoskrnl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_OFFSET 16u
#define FIELDS_LENGTH 8u

static int Failures;
static int Checks;

typedef struct _CASE
{
    const char *Name;
    ULONG Tag;
    ULONG DataLength;
    ULONG SubOffset, SubLength, PrintOffset, PrintLength;
    ULONG BufferLength;
    WCHAR FirstChar;
    NTSTATUS Expected;
} CASE;

static PREPARSE_DATA_BUFFER
Build(const CASE *c)
{
    PREPARSE_DATA_BUFFER Buffer;
    PUCHAR Bytes;
    ULONG i;

    Bytes = malloc(c->BufferLength ? c->BufferLength : 1);
    if (!Bytes)
        abort();
    for (i = 0; i < c->BufferLength; i++)
        Bytes[i] = (UCHAR)('a' + i % 26);

    Buffer = (PREPARSE_DATA_BUFFER)Bytes;
    if (c->BufferLength >= PATH_OFFSET)
    {
        Buffer->ReparseTag = c->Tag;
        Buffer->ReparseDataLength = (USHORT)c->DataLength;
        Buffer->Reserved = 0;
        Buffer->MountPointReparseBuffer.SubstituteNameOffset = (USHORT)c->SubOffset;
        Buffer->MountPointReparseBuffer.SubstituteNameLength = (USHORT)c->SubLength;
        Buffer->MountPointReparseBuffer.PrintNameOffset = (USHORT)c->PrintOffset;
        Buffer->MountPointReparseBuffer.PrintNameLength = (USHORT)c->PrintLength;
        if ((ULONG)PATH_OFFSET + c->SubOffset + sizeof(WCHAR) <= c->BufferLength)
            memcpy(Bytes + PATH_OFFSET + c->SubOffset, &c->FirstChar, sizeof(WCHAR));
    }
    return Buffer;
}

static void
Check(const CASE *c)
{
    PREPARSE_DATA_BUFFER Buffer = Build(c);
    NTSTATUS Status = IopValidateMountPointBuffer(Buffer, c->BufferLength);

    Checks++;
    if (Status != c->Expected)
    {
        printf("FAIL %s: status 0x%08X, expected 0x%08X\n", c->Name, (unsigned)Status, (unsigned)c->Expected);
        Failures++;
    }
    free(Buffer);
}

#define OK STATUS_SUCCESS
#define BAD STATUS_IO_REPARSE_DATA_INVALID
#define MP IO_REPARSE_TAG_MOUNT_POINT
#define BS 0x5C

static const CASE Cases[] =
{
    /* \??\C:\target (26 bytes), NUL, C:\target (18 bytes), NUL: the layout mklink writes */
    { "valid junction",                MP, 8 + 48,   0, 26, 28, 18, 16 + 48, BS, OK },
    { "valid, names back to back",     MP, 8 + 44,   0, 26, 26, 18, 16 + 44, BS, OK },
    { "valid, print name first",       MP, 8 + 48,  20, 26,  0, 18, 16 + 48, BS, OK },
    { "valid, empty print name",       MP, 8 + 26,   0, 26,  0,  0, 16 + 26, BS, OK },
    { "valid, empty print name at end",MP, 8 + 26,   0, 26, 26,  0, 16 + 26, BS, OK },
    { "valid, buffer larger than data",MP, 8 + 48,   0, 26, 28, 18, 16 + 200, BS, OK },

    { "substitute offset > body",      MP, 8 + 48,  60,  2,  0,  2, 16 + 48, BS, BAD },
    { "substitute offset+len > body",  MP, 8 + 48,  24, 26,  0,  2, 16 + 48, BS, BAD },
    { "substitute ends 2 past body",   MP, 8 + 26,   2, 26,  0,  0, 16 + 26, BS, BAD },
    { "substitute offset+len wraps",   MP, 8 + 48, 0xFFFE, 4, 0, 2, 16 + 48, BS, BAD },
    { "print offset > body",           MP, 8 + 48,   0, 26, 50,  2, 16 + 48, BS, BAD },
    { "print offset+len > body",       MP, 8 + 48,   0, 26, 40, 10, 16 + 48, BS, BAD },
    { "print length wraps",            MP, 8 + 48,   0, 26, 28, 0xFFFE, 16 + 48, BS, BAD },

    { "overlap, print inside sub",     MP, 8 + 48,   0, 26,  4, 10, 16 + 48, BS, BAD },
    { "overlap, same name",            MP, 8 + 48,   0, 26,  0, 26, 16 + 48, BS, BAD },
    { "overlap, print ends in sub",    MP, 8 + 48,  10, 26,  0, 12, 16 + 48, BS, BAD },
    { "overlap, print starts in sub",  MP, 8 + 48,   0, 26, 24, 18, 16 + 48, BS, BAD },

    { "odd substitute length",         MP, 8 + 48,   0, 25, 28, 18, 16 + 48, BS, BAD },
    { "odd substitute offset",         MP, 8 + 48,   1, 26, 28, 18, 16 + 48, BS, BAD },
    { "odd print length",              MP, 8 + 48,   0, 26, 28, 17, 16 + 48, BS, BAD },
    { "odd print offset",              MP, 8 + 48,   0, 26, 29, 18, 16 + 48, BS, BAD },

    { "empty substitute name",         MP, 8 + 48,   0,  0, 28, 18, 16 + 48, BS, BAD },
    { "relative substitute name",      MP, 8 + 48,   0, 26, 28, 18, 16 + 48, 'C', BAD },
    { "wrong tag",                     IO_REPARSE_TAG_SYMLINK, 8 + 48, 0, 26, 28, 18, 16 + 48, BS, BAD },
    { "data shorter than the fields",  MP, 6,        0,  0,  0,  0, 16 + 48, BS, BAD },
    { "data longer than the buffer",   MP, 8 + 48,   0, 26, 28, 18, 16 + 46, BS, BAD },
    { "data over the reparse maximum", MP, MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 7, 0, 26, 28, 18,
                                       MAXIMUM_REPARSE_DATA_BUFFER_SIZE + 64, BS, BAD },
    { "data at the reparse maximum",   MP, MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 8, 0, 26, 28, 18,
                                       MAXIMUM_REPARSE_DATA_BUFFER_SIZE, BS, OK },
    { "buffer shorter than the fields",MP, 8,        0,  2,  0,  0, 15,      BS, BAD },
};

static int
Oracle(const UCHAR *Bytes, ULONG BufferLength)
{
    uint64_t Data, Path, so, sl, po, pl;
    ULONG Tag;
    USHORT Field[5];
    WCHAR First;

    if (BufferLength < PATH_OFFSET)
        return 0;
    memcpy(&Tag, Bytes, 4);
    memcpy(Field, Bytes + 4, 2);
    memcpy(Field + 1, Bytes + 8, 8);
    Data = Field[0];
    so = Field[1]; sl = Field[2]; po = Field[3]; pl = Field[4];

    if (Tag != MP || Data < FIELDS_LENGTH || Data + 8 > MAXIMUM_REPARSE_DATA_BUFFER_SIZE || Data + 8 > BufferLength)
        return 0;
    Path = Data - FIELDS_LENGTH;
    if (sl < 2 || ((so | sl | po | pl) & 1) || so + sl > Path || po + pl > Path)
        return 0;
    if (pl && so < po + pl && po < so + sl)
        return 0;
    memcpy(&First, Bytes + PATH_OFFSET + so, 2);
    return First == BS;
}

static uint64_t Seed = 0x9E3779B97F4A7C15ull;

static ULONG
Next(void)
{
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    return (ULONG)Seed;
}

static ULONG
Pick(ULONG Body)
{
    switch (Next() % 6)
    {
        case 0: return 0;
        case 1: return Body;
        case 2: return Body + (Next() % 5) - 2;
        case 3: return 0xFFFF - (Next() % 4);
        case 4: return Next() % (Body + 8);
        default: return (Next() % (Body / 2 + 4)) * 2;
    }
}

static void
Generate(ULONG Iterations)
{
    ULONG i, j, BufferLength, Data;
    PUCHAR Bytes;
    USHORT Field[4];
    WCHAR First;
    int Expected, Got, Accepted = 0;

    for (i = 0; i < Iterations; i++)
    {
        Data = (Next() % 8) ? Next() % 600 : MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 16 + Next() % 32;
        BufferLength = (Next() % 4) ? Data + 8 : Next() % (Data + 32);
        Bytes = malloc(BufferLength ? BufferLength : 1);
        if (!Bytes)
            abort();
        for (j = 0; j < BufferLength; j++)
            Bytes[j] = (UCHAR)Next();

        if (BufferLength >= PATH_OFFSET)
        {
            ULONG Tag = (Next() % 8) ? MP : Next();
            USHORT Length = (USHORT)Data;
            memcpy(Bytes, &Tag, 4);
            memcpy(Bytes + 4, &Length, 2);
            for (j = 0; j < 4; j++)
                Field[j] = (USHORT)Pick(Data >= FIELDS_LENGTH ? Data - FIELDS_LENGTH : 0);
            memcpy(Bytes + 8, Field, 8);
            First = (Next() % 4) ? BS : (WCHAR)Next();
            if ((ULONG)PATH_OFFSET + Field[0] + 2 <= BufferLength)
                memcpy(Bytes + PATH_OFFSET + Field[0], &First, 2);
        }

        Expected = Oracle(Bytes, BufferLength);
        Got = NT_SUCCESS(IopValidateMountPointBuffer((PREPARSE_DATA_BUFFER)Bytes, BufferLength));
        Checks++;
        if (Got != Expected)
        {
            printf("FAIL generated case %u: validator %d, oracle %d\n", (unsigned)i, Got, Expected);
            if (++Failures > 20)
                exit(1);
        }
        Accepted += Got;
        free(Bytes);
    }
    printf("generated %u buffers, %d accepted\n", (unsigned)Iterations, Accepted);
    if (Accepted == 0)
    {
        printf("FAIL the generator never produced a valid buffer\n");
        Failures++;
    }
}

int
main(int argc, char **argv)
{
    size_t i;
    ULONG Iterations = argc > 1 ? (ULONG)strtoul(argv[1], NULL, 0) : 200000;

    for (i = 0; i < sizeof(Cases) / sizeof(Cases[0]); i++)
        Check(&Cases[i]);

    Checks++;
    if (IopValidateMountPointBuffer(NULL, 64) != BAD)
    {
        printf("FAIL NULL buffer accepted\n");
        Failures++;
    }

    Generate(Iterations);

    printf("%d checks, %d failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}

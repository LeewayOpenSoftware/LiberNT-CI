/*
 * PROJECT:     LiberNT host-native tests
 * FILE:        submodules/host-tests/mmcc/pool/host/t_special.c
 * PURPOSE:     Special pool: selection rules, placement in the page, pattern checks
 *
 * SPDX-FileCopyrightText: 2026 Leeway Open Software
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <nvs/pool/include/poolspecial.h>

_Thread_local volatile int *PoolHostLockBlocked;
_Thread_local ULONG PoolHostProcessor;

#define TAG(a, b, c, d) ((ULONG)(a) | ((ULONG)(b) << 8) | ((ULONG)(c) << 16) | ((ULONG)(d) << 24))

static int Failures;
static int Checks;

#define CHECK(e) do { Checks++; if (!(e)) { Failures++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #e); } } while (0)

static void
TestSelect(void)
{
    CHECK(!PoolSpecialSelect(0, 32, TAG('A', 'b', 'c', 'd')));
    CHECK(PoolSpecialSelect(POOL_SPECIAL_SELECT_ALL, 1, TAG('A', 'b', 'c', 'd')));
    CHECK(PoolSpecialSelect(POOL_SPECIAL_SELECT_ALL, PAGE_SIZE - 1, 0));
    CHECK(!PoolSpecialSelect(POOL_SPECIAL_SELECT_ALL, PAGE_SIZE, 0));
    CHECK(!PoolSpecialSelect(POOL_SPECIAL_SELECT_ALL, 0, 0));

    CHECK(PoolSpecialSelect(0x40, 0x40, 0));
    CHECK(PoolSpecialSelect(0x40, 0x31, 0));
    CHECK(!PoolSpecialSelect(0x40, 0x30, 0));
    CHECK(!PoolSpecialSelect(0x40, 0x41, 0));

    CHECK(PoolSpecialSelect(TAG('A', 'b', 'c', 'd'), 8, TAG('A', 'b', 'c', 'd')));
    CHECK(PoolSpecialSelect(TAG('A', 'b', 'c', 'd'), 8, TAG('A', 'b', 'c', 'd') | 0x80000000u));
    CHECK(!PoolSpecialSelect(TAG('A', 'b', 'c', 'd'), 8, TAG('A', 'b', 'c', 'e')));
    CHECK(PoolSpecialSelect(TAG('A', '?', 'c', 'd'), 8, TAG('A', 'x', 'c', 'd')));
    CHECK(!PoolSpecialSelect(TAG('A', '?', 'c', 'd'), 8, TAG('B', 'x', 'c', 'd')));
    CHECK(PoolSpecialSelect(TAG('A', 'b', '*', 0), 8, TAG('A', 'b', 'z', 'z')));
    CHECK(!PoolSpecialSelect(TAG('A', 'b', '*', 0), 8, TAG('A', 'c', 'z', 'z')));
    CHECK(!PoolSpecialSelect(TAG('A', 'b', 'c', 'd'), PAGE_SIZE, TAG('A', 'b', 'c', 'd')));
}

static void
TestPlace(void)
{
    ULONG Offset;
    SIZE_T Size;

    CHECK(!PoolSpecialPlace(0, 0, FALSE, &Offset));
    CHECK(!PoolSpecialPlace(PAGE_SIZE, 0, FALSE, &Offset));
    CHECK(!PoolSpecialPlace(PAGE_SIZE + 1, 0, TRUE, &Offset));

    for (Size = 1; Size < PAGE_SIZE; Size++)
    {
        CHECK(PoolSpecialPlace(Size, 0, FALSE, &Offset));
        CHECK((Offset & (POOL_SPECIAL_ALIGNMENT - 1)) == 0);
        CHECK(Offset + Size <= PAGE_SIZE);
        CHECK(PAGE_SIZE - (Offset + Size) < POOL_SPECIAL_ALIGNMENT);

        CHECK(PoolSpecialPlace(Size, 0, TRUE, &Offset));
        CHECK(Offset == 0);

        CHECK(PoolSpecialPlace(Size, 64, FALSE, &Offset));
        CHECK((Offset & 63) == 0);
        CHECK(Offset + Size <= PAGE_SIZE);
        CHECK(PAGE_SIZE - (Offset + Size) < 64);
    }

    CHECK(PoolSpecialPlace(100, 3, FALSE, &Offset) && Offset == PAGE_SIZE - 112);
    CHECK(PoolSpecialPlace(100, 24, FALSE, &Offset) && Offset == PAGE_SIZE - 112);
    CHECK(PoolSpecialPlace(100, PAGE_SIZE * 2, FALSE, &Offset) && Offset == PAGE_SIZE - 112);
}

static void
TestPattern(void)
{
    POOL_SPECIAL_RECORD Record;
    ULONG_PTR Corrupt;
    PUCHAR Memory, Page, Other, Block;
    ULONG Offset, Position;
    SIZE_T Size;
    UCHAR Saved;

    Memory = malloc((SIZE_T)PAGE_SIZE * 3);
    if (!Memory)
        abort();

    Page = (PUCHAR)(((ULONG_PTR)Memory + PAGE_SIZE - 1) & ~(ULONG_PTR)(PAGE_SIZE - 1));
    Other = Page + PAGE_SIZE;

    for (Size = 1; Size < PAGE_SIZE; Size += (Size < 80) ? 1 : 211)
    {
        CHECK(PoolSpecialPlace(Size, 0, FALSE, &Offset));
        memset(Page, 0, PAGE_SIZE);
        Block = PoolSpecialPrepare(&Record, Page, Size, Offset, TAG('P', 'a', 't', 't'), 5);
        CHECK(Block == Page + Offset);
        CHECK(Record.Size == Size && Record.Offset == Offset && Record.Tag == TAG('P', 'a', 't', 't') &&
              Record.Flags == 5);
        CHECK(PoolSpecialOwns(&Record, Page, Block));
        CHECK(!PoolSpecialOwns(&Record, Page, Block + 1));
        CHECK(!PoolSpecialOwns(&Record, Other, Block));
        CHECK(Page[0] != 0);

        memset(Block, 0xEE, Size);
        CHECK(PoolSpecialCheck(&Record, Page, &Corrupt) == PoolSpecialIntact && Corrupt == 0);

        if (Offset != 0)
        {
            for (Position = 0; Position < Offset; Position += (Offset > 64) ? Offset - 1 : 1)
            {
                Saved = Page[Position];
                Page[Position] ^= 0x10;
                CHECK(PoolSpecialCheck(&Record, Page, &Corrupt) == PoolSpecialCorruptBefore);
                CHECK(Corrupt == (ULONG_PTR)&Page[Position]);
                Page[Position] = Saved;
            }
        }

        for (Position = Offset + (ULONG)Size; Position < PAGE_SIZE; Position++)
        {
            Saved = Page[Position];
            Page[Position] = (UCHAR)~Saved;
            CHECK(PoolSpecialCheck(&Record, Page, &Corrupt) == PoolSpecialCorruptAfter);
            CHECK(Corrupt == (ULONG_PTR)&Page[Position]);
            Page[Position] = Saved;
        }

        CHECK(PoolSpecialPlace(Size, 0, TRUE, &Offset) && Offset == 0);
        Block = PoolSpecialPrepare(&Record, Page, Size, Offset, 1, 0);
        CHECK(Block == Page);
        CHECK(PoolSpecialCheck(&Record, Page, &Corrupt) == PoolSpecialIntact);
        Saved = Page[Size];
        Page[Size] ^= 1;
        CHECK(PoolSpecialCheck(&Record, Page, &Corrupt) == PoolSpecialCorruptAfter);
        CHECK(Corrupt == (ULONG_PTR)&Page[Size]);
        Page[Size] = Saved;
        Saved = Page[PAGE_SIZE - 1];
        Page[PAGE_SIZE - 1] ^= 0x80;
        CHECK(PoolSpecialCheck(&Record, Page, &Corrupt) == PoolSpecialCorruptAfter);
        CHECK(Corrupt == (ULONG_PTR)&Page[PAGE_SIZE - 1]);
        Page[PAGE_SIZE - 1] = Saved;
    }

    PoolSpecialPrepare(&Record, Page, 64, PAGE_SIZE - 64, 1, 0);
    PoolSpecialPrepare(&Record, Other, 64, PAGE_SIZE - 64, 1, 0);
    CHECK(Page[0] != Other[0]);

    memset(&Record, 0, sizeof(Record));
    CHECK(!PoolSpecialOwns(&Record, Page, Page));
    Record.Offset = PAGE_SIZE - 16;
    Record.Size = 32;
    CHECK(PoolSpecialCheck(&Record, Page, &Corrupt) == PoolSpecialCorruptBefore);
    Record.Offset = PAGE_SIZE + 16;
    Record.Size = 1;
    CHECK(PoolSpecialCheck(&Record, Page, &Corrupt) == PoolSpecialCorruptBefore);

    free(Memory);
}

int
main(void)
{
    TestSelect();
    TestPlace();
    TestPattern();

    printf("special pool: %d checks, %d failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}

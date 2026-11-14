/*
 * PROJECT:     LiberNT host-native tests
 * FILE:        submodules/host-tests/mmcc/nvs/core/t_guard.c
 * PURPOSE:     Guarded system region host-native tests
 *
 * SPDX-FileCopyrightText: 2026 Leeway Open Software
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mmharness.h"
#include <nvs/include/misys.h>

#define GUARD_THREADS    4
#define GUARD_ITERATIONS 5000

static
NTSTATUS
GuardWrite64(TEST_WORLD *World, ULONG Cpu, ULONG64 Va, ULONG64 Value)
{
    return MachineAccessMemory(&World->Machine, Cpu, Va, &Value, sizeof(Value), MachineWrite, FALSE);
}

static
ULONG64
GuardRead64(TEST_WORLD *World, ULONG Cpu, ULONG64 Va, NTSTATUS *Status)
{
    ULONG64 Value = 0xDEADDEADDEADDEADULL;

    *Status = MachineAccessMemory(&World->Machine, Cpu, Va, &Value, sizeof(Value), MachineRead, FALSE);
    return Value;
}

static
void
GuardWorldCreate(TEST_WORLD *World, ULONG Frames, ULONG Cpus, BOOLEAN StrictTlb)
{
    ULONG i;

    WorldCreate(World, Frames, Cpus, 1000000);
    World->Machine.StrictTlb = StrictTlb;
    for (i = 0; i < Cpus; i++)
        WorldAttach(World, i, NULL);
}

static
void
GuardWorldDestroy(TEST_WORLD *World, ULONG Frames, PMI_GUARD_REGION Region)
{
    MI_GUARD_STATS Stats;

    MiGuardQueryStats(Region, &Stats);
    CHECK(Stats.LiveSlots == 0);
    CHECK(Stats.CommittedPages == (ULONG64)Stats.ReadySlots * Region->DataPages);
    MiGuardRegionDelete(Region);
    CHECK(MI_ATOMIC_READ64(&World->System.SystemSpace.PageTablePages) == World->WindowTables);
    CHECK(World->System.SystemSpace.VadRoot.NodeCount == World->WindowVads);
    CHECK(WorldCheck(World) == 0);
    WorldExpectClean(World, Frames);
    WorldDestroy(World);
}

static
void
GuardLayout(void)
{
    static TEST_WORLD World;
    PMI_GUARD_REGION Region;
    MI_GUARD_STATS Stats;
    ULONG64 Data, Owner;
    PVOID Context, Found;
    NTSTATUS Status;
    ULONG i;

    GuardWorldCreate(&World, 1024, 2, TRUE);
    CHECK(NT_SUCCESS(MiGuardRegionCreate(&World.System, 8, 1, 2, 2, 6, 32, &Region)));
    CHECK(Region->Base >= World.System.Arch->SystemAddressStart);
    CHECK(Region->End == Region->Base + (8 * 2 + 1) * 4096ULL);

    MiGuardQueryStats(Region, &Stats);
    CHECK(Stats.ReadySlots == 2 && Stats.CommittedPages == 2 && Stats.FreeSlots == 6);
    CHECK(MiGuardQuery(Region, Region->Base + 4096, NULL, NULL) == MiGuardFreed);
    CHECK(MiGuardQuery(Region, Region->Base + 3 * 4096, NULL, NULL) == MiGuardFreed);
    CHECK(MiGuardFree(Region, Region->Base + 4096) == STATUS_INVALID_ADDRESS);
    GuardRead64(&World, 0, Region->Base + 4096, &Status);
    CHECK(Status == STATUS_ACCESS_VIOLATION);
    CHECK(GuardWrite64(&World, 1, Region->Base + 3 * 4096 + 8, 1) == STATUS_ACCESS_VIOLATION);
    CHECK(MiGuardAllocate(Region, MI_PROT_NOACCESS, &Data, &Context) == STATUS_INVALID_PAGE_PROTECTION && Data == 0);
    CHECK(MiGuardAllocate(Region, MI_PROT_READONLY, &Data, NULL) == STATUS_INVALID_PAGE_PROTECTION);

    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &Data, &Context)));
    CHECK((Data & 4095) == 0 && Context != NULL);
    for (i = 0; i < 32; i++)
        CHECK(((PUCHAR)Context)[i] == 0);

    CHECK(MiGuardQuery(Region, Data, &Owner, &Found) == MiGuardLive && Owner == Data && Found == Context);
    CHECK(MiGuardQuery(Region, Data + 4095, &Owner, NULL) == MiGuardLive && Owner == Data);
    CHECK(MiGuardQuery(Region, Data - 1, &Owner, NULL) == MiGuardGuardPage && Owner == 0);
    CHECK(MiGuardQuery(Region, Data + 4096, NULL, NULL) == MiGuardGuardPage);
    CHECK(MiGuardQuery(Region, Region->End - 1, NULL, NULL) == MiGuardGuardPage);
    CHECK(MiGuardQuery(Region, Region->End, NULL, NULL) == MiGuardOutside);
    CHECK(MiGuardQuery(Region, Region->Base - 1, NULL, NULL) == MiGuardOutside);

    CHECK(NT_SUCCESS(GuardWrite64(&World, 0, Data, 0x5A5A)));
    CHECK(NT_SUCCESS(GuardWrite64(&World, 0, Data + 4088, 0xA5A5)));
    CHECK(GuardRead64(&World, 1, Data + 4088, &Status) == 0xA5A5 && NT_SUCCESS(Status));
    CHECK(GuardRead64(&World, 0, Data, &Status) == 0x5A5A && NT_SUCCESS(Status));
    CHECK(GuardWrite64(&World, 0, Data + 4096, 1) == STATUS_ACCESS_VIOLATION);
    CHECK(GuardWrite64(&World, 0, Data - 8, 1) == STATUS_ACCESS_VIOLATION);
    GuardRead64(&World, 1, Data + 4089, &Status);
    CHECK(Status == STATUS_ACCESS_VIOLATION);

    CHECK(NT_SUCCESS(MiGuardFree(Region, Data)));
    CHECK(MiGuardQuery(Region, Data, NULL, NULL) == MiGuardFreed);
    GuardRead64(&World, 1, Data + 4088, &Status);
    CHECK(Status == STATUS_ACCESS_VIOLATION);
    CHECK(GuardWrite64(&World, 0, Data, 2) == STATUS_ACCESS_VIOLATION);

    CHECK(MiGuardFree(Region, Data) == STATUS_INVALID_ADDRESS);
    CHECK(MiGuardFree(Region, Data + 8) == STATUS_INVALID_ADDRESS);
    CHECK(MiGuardFree(Region, Data + 4096) == STATUS_INVALID_ADDRESS);
    CHECK(MiGuardFree(Region, Region->Base) == STATUS_INVALID_ADDRESS);
    CHECK(MiGuardFree(Region, Region->End) == STATUS_INVALID_ADDRESS);
    CHECK(MiGuardFree(Region, Region->End - 4096) == STATUS_INVALID_ADDRESS);

    MiGuardQueryStats(Region, &Stats);
    CHECK(Stats.LiveSlots == 0 && Stats.ReadySlots == 2 && Stats.Allocations == 1 && Stats.Frees == 1);
    CHECK(MiGuardRefill(Region) == 0);

    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_EXECUTE_READWRITE, &Data, NULL)));
    CHECK(NT_SUCCESS(GuardWrite64(&World, 0, Data + 16, 0xC3)));
    CHECK(GuardRead64(&World, 1, Data + 16, &Status) == 0xC3 && NT_SUCCESS(Status));
    CHECK(NT_SUCCESS(MiGuardFree(Region, Data)));

    GuardWorldDestroy(&World, 1024, Region);
}

static
void
GuardReuseDelay(void)
{
    static TEST_WORLD World;
    PMI_GUARD_REGION Region;
    ULONG64 Data[8], Extra, Reused[3];
    ULONG i;

    GuardWorldCreate(&World, 1024, 1, TRUE);
    CHECK(NT_SUCCESS(MiGuardRegionCreate(&World.System, 8, 1, 0, 3, 64, 0, &Region)));

    for (i = 0; i < 5; i++)
        CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &Data[i], NULL)));
    CHECK(MiGuardAllocate(Region, MI_PROT_READWRITE, &Extra, NULL) == STATUS_INSUFFICIENT_RESOURCES);

    CHECK(NT_SUCCESS(MiGuardFree(Region, Data[0])));
    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &Data[5], NULL)));
    CHECK(Data[5] != Data[0]);
    CHECK(MiGuardAllocate(Region, MI_PROT_READWRITE, &Extra, NULL) == STATUS_INSUFFICIENT_RESOURCES);

    for (i = 1; i <= 3; i++)
        CHECK(NT_SUCCESS(MiGuardFree(Region, Data[i])));

    for (i = 0; i < 3; i++)
        CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &Reused[i], NULL)));
    CHECK(Reused[0] != Data[0] && Reused[1] != Data[0] && Reused[2] == Data[0]);
    for (i = 0; i < 3; i++)
        CHECK(Reused[i] != Data[1] && Reused[i] != Data[2] && Reused[i] != Data[3]);
    CHECK(MiGuardAllocate(Region, MI_PROT_READWRITE, &Extra, NULL) == STATUS_INSUFFICIENT_RESOURCES);

    CHECK(NT_SUCCESS(GuardWrite64(&World, 0, Reused[2], 7)));
    for (i = 0; i < 3; i++)
        CHECK(NT_SUCCESS(MiGuardFree(Region, Reused[i])));
    CHECK(NT_SUCCESS(MiGuardFree(Region, Data[4])));
    CHECK(NT_SUCCESS(MiGuardFree(Region, Data[5])));

    GuardWorldDestroy(&World, 1024, Region);
}

static
void
GuardBudgetAndReserve(void)
{
    static TEST_WORLD World;
    static ULONG Held[1024];
    PMI_GUARD_REGION Region;
    MI_GUARD_STATS Stats;
    ULONG64 A, B, C;
    ULONG Count = 0, i;
    NTSTATUS Status;

    GuardWorldCreate(&World, 1024, 1, TRUE);
    CHECK(NT_SUCCESS(MiGuardRegionCreate(&World.System, 16, 1, 1, 0, 2, 0, &Region)));
    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &A, NULL)));
    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &B, NULL)));
    CHECK(MiGuardAllocate(Region, MI_PROT_READWRITE, &C, NULL) == STATUS_INSUFFICIENT_RESOURCES);
    MiGuardQueryStats(Region, &Stats);
    CHECK(Stats.CommittedPages == 2 && Stats.PeakCommittedPages == 2 && Stats.Failures == 1);
    CHECK(NT_SUCCESS(MiGuardFree(Region, A)));
    CHECK(NT_SUCCESS(MiGuardFree(Region, B)));
    CHECK(MiGuardRefill(Region) == 1);
    GuardWorldDestroy(&World, 1024, Region);

    GuardWorldCreate(&World, 1024, 1, TRUE);
    CHECK(NT_SUCCESS(MiGuardRegionCreate(&World.System, 16, 1, 2, 0, 16, 0, &Region)));
    while (Count < RTL_NUMBER_OF(Held) &&
           (Held[Count] = MiPfnAllocatePage(&World.System.Pfn, 0)) != MI_FRAME_INVALID)
    {
        Count++;
    }
    CHECK(Count > 0 && Count < RTL_NUMBER_OF(Held));

    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &A, NULL)));
    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_EXECUTE_READWRITE, &B, NULL)));
    CHECK(NT_SUCCESS(GuardWrite64(&World, 0, B + 8, 2)));
    CHECK(NT_SUCCESS(GuardWrite64(&World, 0, A + 8, 1)));
    CHECK(GuardRead64(&World, 0, A + 8, &Status) == 1 && NT_SUCCESS(Status));
    CHECK(MiGuardAllocate(Region, MI_PROT_READWRITE, &C, NULL) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(MiGuardRefill(Region) == 0);
    MiGuardQueryStats(Region, &Stats);
    CHECK(Stats.CommittedPages == 2 && Stats.FreeSlots == 14);

    for (i = 0; i < Count; i++)
        MiPfnFreePage(&World.System.Pfn, Held[i]);

    CHECK(MiGuardRefill(Region) == 2);
    CHECK(NT_SUCCESS(MiGuardFree(Region, A)));
    CHECK(NT_SUCCESS(MiGuardFree(Region, B)));
    GuardWorldDestroy(&World, 1024, Region);
}

static
void
GuardMultiPage(void)
{
    static TEST_WORLD World;
    PMI_GUARD_REGION Region;
    ULONG64 Data, Next;
    NTSTATUS Status;

    GuardWorldCreate(&World, 1024, 1, TRUE);
    CHECK(NT_SUCCESS(MiGuardRegionCreate(&World.System, 4, 3, 3, 0, 12, 0, &Region)));
    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &Data, NULL)));
    CHECK(NT_SUCCESS(MiGuardAllocate(Region, MI_PROT_READWRITE, &Next, NULL)));
    CHECK(Next == Data + 4 * 4096ULL || Data == Next + 4 * 4096ULL);
    CHECK(NT_SUCCESS(GuardWrite64(&World, 0, Data, 1)));
    CHECK(NT_SUCCESS(GuardWrite64(&World, 0, Data + 3 * 4096 - 8, 2)));
    CHECK(GuardRead64(&World, 0, Data + 3 * 4096 - 8, &Status) == 2 && NT_SUCCESS(Status));
    CHECK(GuardWrite64(&World, 0, Data + 3 * 4096, 3) == STATUS_ACCESS_VIOLATION);
    CHECK(GuardWrite64(&World, 0, Data - 8, 3) == STATUS_ACCESS_VIOLATION);
    CHECK(MiGuardQuery(Region, Data + 2 * 4096 + 100, NULL, NULL) == MiGuardLive);
    CHECK(MiGuardFree(Region, Data + 4096) == STATUS_INVALID_ADDRESS);
    CHECK(NT_SUCCESS(MiGuardFree(Region, Data)));
    GuardRead64(&World, 0, Data + 2 * 4096, &Status);
    CHECK(Status == STATUS_ACCESS_VIOLATION);
    CHECK(NT_SUCCESS(MiGuardFree(Region, Next)));
    GuardWorldDestroy(&World, 1024, Region);
}

typedef struct _GUARD_THREAD
{
    TEST_WORLD *World;
    PMI_GUARD_REGION Region;
    ULONG Index;
    ULONG Allocated;
} GUARD_THREAD;

static
void *
GuardWorker(void *Argument)
{
    GUARD_THREAD *Thread = Argument;
    ULONG64 Seed = 0x9E3779B97F4A7C15ULL * (Thread->Index + 1);
    ULONG64 Held[4] = { 0 };
    NTSTATUS Status;
    ULONG i, j;

    MiHostCpu = Thread->Index;
    for (i = 0; i < GUARD_ITERATIONS; i++)
    {
        j = (ULONG)(Rng(&Seed) % RTL_NUMBER_OF(Held));
        if (Held[j] != 0)
        {
            CHECK(GuardRead64(Thread->World, Thread->Index, Held[j] + 4088, &Status) == Held[j] &&
                  NT_SUCCESS(Status));
            CHECK(MiGuardQuery(Thread->Region, Held[j], NULL, NULL) == MiGuardLive);
            CHECK(NT_SUCCESS(MiGuardFree(Thread->Region, Held[j])));
            Held[j] = 0;
        }
        else if (NT_SUCCESS(MiGuardAllocate(Thread->Region, MI_PROT_READWRITE, &Held[j], NULL)))
        {
            Thread->Allocated++;
            CHECK(NT_SUCCESS(GuardWrite64(Thread->World, Thread->Index, Held[j] + 4088, Held[j])));
        }
        if ((i & 63) == 0)
            MiGuardRefill(Thread->Region);
    }

    for (j = 0; j < RTL_NUMBER_OF(Held); j++)
    {
        if (Held[j] != 0)
            CHECK(NT_SUCCESS(MiGuardFree(Thread->Region, Held[j])));
    }
    return NULL;
}

static
void
GuardSmp(void)
{
    static TEST_WORLD World;
    GUARD_THREAD Threads[GUARD_THREADS];
    pthread_t Handles[GUARD_THREADS];
    PMI_GUARD_REGION Region;
    MI_GUARD_STATS Stats;
    ULONG64 Total = 0;
    ULONG i;

    GuardWorldCreate(&World, 2048, GUARD_THREADS, FALSE);
    CHECK(NT_SUCCESS(MiGuardRegionCreate(&World.System, 64, 1, 4, 8, 48, 16, &Region)));

    for (i = 0; i < GUARD_THREADS; i++)
    {
        Threads[i].World = &World;
        Threads[i].Region = Region;
        Threads[i].Index = i;
        Threads[i].Allocated = 0;
        pthread_create(&Handles[i], NULL, GuardWorker, &Threads[i]);
    }
    for (i = 0; i < GUARD_THREADS; i++)
    {
        pthread_join(Handles[i], NULL);
        Total += Threads[i].Allocated;
    }
    MiHostCpu = 0;

    MiGuardQueryStats(Region, &Stats);
    CHECK(Stats.Allocations == Total && Stats.Frees == Total);
    CHECK(Stats.PeakCommittedPages <= 48);
    CHECK(Total > GUARD_THREADS * GUARD_ITERATIONS / 4);
    GuardWorldDestroy(&World, 2048, Region);
}

static
void
GuardBadParameters(void)
{
    static TEST_WORLD World;
    PMI_GUARD_REGION Region;

    GuardWorldCreate(&World, 256, 1, TRUE);
    CHECK(MiGuardRegionCreate(&World.System, 0, 1, 0, 0, 1, 0, &Region) == STATUS_INVALID_PARAMETER && Region == NULL);
    CHECK(MiGuardRegionCreate(&World.System, 4, 0, 0, 0, 1, 0, &Region) == STATUS_INVALID_PARAMETER);
    CHECK(MiGuardRegionCreate(&World.System, 4, 1, 0, 4, 1, 0, &Region) == STATUS_INVALID_PARAMETER);
    CHECK(MiGuardRegionCreate(&World.System, 4, 2, 0, 0, 1, 0, &Region) == STATUS_INVALID_PARAMETER);
    CHECK(MiGuardRegionCreate(&World.System, 4, 1, 4, 1, 4, 0, &Region) == STATUS_INVALID_PARAMETER);
    CHECK(MiGuardRegionCreate(&World.System, 0x80000000, 0xFFFFFFFF, 0, 0, 0xFFFFFFFF, 0, &Region) ==
          STATUS_INVALID_PARAMETER);
    CHECK(MiGuardQuery(NULL, 0x1000, NULL, NULL) == MiGuardOutside);
    CHECK(WorldCheck(&World) == 0);
    WorldExpectClean(&World, 256);
    WorldDestroy(&World);
}

void
TestGuard(void)
{
    GuardBadParameters();
    GuardLayout();
    GuardReuseDelay();
    GuardBudgetAndReserve();
    GuardMultiPage();
    GuardSmp();
}

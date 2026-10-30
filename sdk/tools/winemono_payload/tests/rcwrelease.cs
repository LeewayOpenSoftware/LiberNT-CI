/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test that a garbage collection does not wait for a blocking Release of a finalized COM object
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

public static class Program
{
    [DllImport("kernel32.dll")]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protect);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)]
    static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr GetModuleHandleW(string name);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int QueryInterfaceFn(IntPtr self, ref Guid iid, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int AddRefFn(IntPtr self);

    static readonly Guid IID_IUnknown = new Guid("00000000-0000-0000-C000-000000000046");
    static QueryInterfaceFn queryInterface = QueryInterface;
    static AddRefFn addRef = AddRef;
    static int released;
    static int failures;

    static void Check(bool condition, string message)
    {
        Console.WriteLine((condition ? "ok: " : "FAILED: ") + message);
        if (!condition)
            failures++;
    }

    static int QueryInterface(IntPtr self, ref Guid iid, out IntPtr result)
    {
        if (iid == IID_IUnknown)
        {
            result = self;
            return 0;
        }
        result = IntPtr.Zero;
        return unchecked((int)0x80004002);
    }

    static int AddRef(IntPtr self)
    {
        return 2;
    }

    static void Wrap(IntPtr unknown)
    {
        object rcw = Marshal.GetObjectForIUnknown(unknown);
        Check(rcw != null, "wrapper created");
    }

    [MTAThread]
    public static int Main()
    {
        IntPtr block = VirtualAlloc(new IntPtr(0x100000000), new UIntPtr(0x10000), 0x3000, 0x04);
        Check(block == new IntPtr(0x100000000), "object block at 4 GB");
        if (block != new IntPtr(0x100000000))
            return failures;

        IntPtr unknown = new IntPtr(0x100000BB8);
        IntPtr vtable = new IntPtr(0x100001000);
        Marshal.WriteIntPtr(unknown, vtable);
        Marshal.WriteIntPtr(vtable, 0, Marshal.GetFunctionPointerForDelegate(queryInterface));
        Marshal.WriteIntPtr(vtable, IntPtr.Size, Marshal.GetFunctionPointerForDelegate(addRef));
        Marshal.WriteIntPtr(vtable, 2 * IntPtr.Size, GetProcAddress(GetModuleHandleW("kernel32.dll"), "Sleep"));

        Thread creator = new Thread(() => Wrap(unknown));
        creator.Start();
        creator.Join();

        long finalizing = 0;
        Thread finalize = new Thread(() =>
        {
            Stopwatch spent = Stopwatch.StartNew();
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            GC.WaitForPendingFinalizers();
            finalizing = spent.ElapsedMilliseconds;
            Interlocked.Exchange(ref released, 1);
        });
        finalize.IsBackground = true;
        finalize.Start();
        Thread.Sleep(1000);

        Stopwatch watch = Stopwatch.StartNew();
        GC.Collect();
        long collect = watch.ElapsedMilliseconds;
        Check(collect < 1000, "collection while the finalizer blocks in Release took " + collect + " ms");
        Check(finalize.Join(30000), "finalizer finished releasing");
        Check(released == 1 && finalizing >= 2500, "the finalizer released the object in " + finalizing + " ms");

        Console.WriteLine("rcwrelease: " + failures + " failures");
        return failures;
    }
}

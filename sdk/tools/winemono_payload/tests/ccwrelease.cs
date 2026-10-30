/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the reference count of a COM callable wrapper released more often than referenced
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.Runtime.InteropServices;

[ComVisible(true), Guid("6A3E2D51-0C47-4B8E-9F21-7D4C5B6A8E90"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ICcwReleased
{
    [PreserveSig]
    int Value();
}

[ComVisible(true), ClassInterface(ClassInterfaceType.None)]
public class CcwReleased : ICcwReleased
{
    public int Value()
    {
        return 42;
    }
}

public static class Program
{
    static int failures;

    static void Check(bool condition, string message)
    {
        Console.WriteLine((condition ? "ok: " : "FAILED: ") + message);
        if (!condition)
            failures++;
    }

    static void Expect(int expected, int actual, string message)
    {
        Check(actual == expected, message + " " + actual);
    }

    public static int Main()
    {
        CcwReleased target = new CcwReleased();
        IntPtr unknown = Marshal.GetIUnknownForObject(target);
        IntPtr itf = Marshal.GetComInterfaceForObject(target, typeof(ICcwReleased));

        Expect(1, Marshal.Release(itf), "release of the interface");
        Expect(0, Marshal.Release(unknown), "release of the last reference");
        Expect(-1, Marshal.Release(unknown), "release without a reference");
        Expect(-1, Marshal.Release(itf), "release of the interface without a reference");
        Expect(1, Marshal.AddRef(unknown), "reference taken after the extra releases");
        Expect(0, Marshal.Release(unknown), "release of that reference");
        IntPtr again = Marshal.GetIUnknownForObject(target);
        Check(again == unknown, "the object keeps its IUnknown");
        Expect(0, Marshal.Release(again), "release of the new reference");
        GC.KeepAlive(target);

        Console.WriteLine("ccwrelease: " + failures + " failures");
        return failures;
    }
}

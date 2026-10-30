/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test VARIANT arguments passed to a managed object through its COM interface
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.Runtime.InteropServices;

[ComVisible(true), Guid("4D2F6B1A-8C3E-4A57-9B10-2E6F7A8B9C0D"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ICcwVariants
{
    [PreserveSig]
    int ByValue([MarshalAs(UnmanagedType.Struct)] object value);

    [PreserveSig]
    int ByRef([In, MarshalAs(UnmanagedType.Struct)] ref object value);

    [PreserveSig]
    int Both([MarshalAs(UnmanagedType.LPWStr)] string key, [In, MarshalAs(UnmanagedType.Struct)] ref object value, [MarshalAs(UnmanagedType.Struct)] object state);

    [return: MarshalAs(UnmanagedType.Interface)]
    ICcwVariants Clear([MarshalAs(UnmanagedType.Struct)] object state);
}

[ComVisible(true), ClassInterface(ClassInterfaceType.None)]
public class CcwVariants : ICcwVariants
{
    public static object Last;

    public int ByValue(object value)
    {
        Last = value;
        return value is int ? (int)value : -1;
    }

    public int ByRef(ref object value)
    {
        Last = value;
        return value is string ? ((string)value).Length : -1;
    }

    public static int Cleared;

    public ICcwVariants Clear(object state)
    {
        Cleared++;
        return this;
    }

    public int Both(string key, ref object value, object state)
    {
        return key.Length * 100 + (value is int ? (int)value : 0) + (state == null ? 1 : 2);
    }
}

[StructLayout(LayoutKind.Sequential)]
public struct NativeVariant
{
    public ushort vt;
    public ushort reserved1;
    public ushort reserved2;
    public ushort reserved3;
    public IntPtr data;
    public IntPtr data2;
}

public static class Program
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int ByValueFn(IntPtr self, NativeVariant value);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int ByRefFn(IntPtr self, ref NativeVariant value);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int BothFn(IntPtr self, IntPtr key, ref NativeVariant value, NativeVariant state);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int ClearFn(IntPtr self, NativeVariant state, IntPtr result);

    static int failures;

    static void Check(bool condition, string message)
    {
        Console.WriteLine((condition ? "ok: " : "FAILED: ") + message);
        if (!condition)
            failures++;
    }

    static T Slot<T>(IntPtr itf, int index) where T : class
    {
        IntPtr vtable = Marshal.ReadIntPtr(itf);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(vtable, index * IntPtr.Size), typeof(T));
    }

    public static int Main()
    {
        IntPtr itf = Marshal.GetComInterfaceForObject(new CcwVariants(), typeof(ICcwVariants));
        NativeVariant i4 = new NativeVariant { vt = 3, data = (IntPtr)42 };
        NativeVariant empty = new NativeVariant();
        NativeVariant text = new NativeVariant { vt = 8, data = Marshal.StringToBSTR("hello") };
        IntPtr key = Marshal.StringToCoTaskMemUni("key");

        Check(Slot<ByValueFn>(itf, 3)(itf, i4) == 42, "VT_I4 by value");
        Check(Slot<ByValueFn>(itf, 3)(itf, empty) == -1 && CcwVariants.Last == null, "VT_EMPTY by value");
        Check(Slot<ByRefFn>(itf, 4)(itf, ref text) == 5, "VT_BSTR by reference");
        Check(text.vt == 8, "VT_BSTR by reference stays unchanged");
        Check(Slot<BothFn>(itf, 5)(itf, key, ref i4, empty) == 343, "string, VARIANT by reference and VARIANT by value");
        Check(Slot<BothFn>(itf, 5)(itf, key, ref i4, i4) == 344, "VARIANT by value after a VARIANT by reference");

        IntPtr slot = Marshal.AllocCoTaskMem(IntPtr.Size);
        Marshal.WriteIntPtr(slot, IntPtr.Zero);
        int hr = Slot<ClearFn>(itf, 6)(itf, empty, slot);
        IntPtr returned = Marshal.ReadIntPtr(slot);
        Check(hr == 0 && returned != IntPtr.Zero && CcwVariants.Cleared == 1, "interface result through a pointer hr " + hr.ToString("x"));
        if (returned != IntPtr.Zero)
            Marshal.Release(returned);
        Marshal.FreeCoTaskMem(slot);
        hr = Slot<ClearFn>(itf, 6)(itf, empty, IntPtr.Zero);
        Check(hr == 0 && CcwVariants.Cleared == 2, "method runs with a NULL result pointer hr " + hr.ToString("x"));

        Marshal.FreeCoTaskMem(key);
        Marshal.FreeBSTR(text.data);
        Marshal.Release(itf);
        Console.WriteLine("ccwvariant: " + failures + " failures");
        return failures;
    }
}

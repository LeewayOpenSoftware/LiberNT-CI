/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test interface arrays passed to a managed object through its COM interface
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.Runtime.InteropServices;

[ComVisible(true), Guid("2B0C8F7E-6A1D-4C8E-9F3B-5D7A1E2C4B60"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ICcwItem
{
    [PreserveSig]
    int Value();
}

[StructLayout(LayoutKind.Sequential)]
public struct CcwRecord
{
    [MarshalAs(UnmanagedType.BStr)]
    public string Name;
    public int Value;
}

[ComVisible(true), Guid("7E4A9D21-3B6F-4F0A-8C5E-1A2B3C4D5E6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ICcwItems
{
    [PreserveSig]
    int Next(uint count, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] ICcwItem[] items, out uint fetched);

    [PreserveSig]
    int Sum(uint count, [In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] ICcwItem[] items);

    [PreserveSig]
    int GetRecords(uint count, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] CcwRecord[] records);

    [PreserveSig]
    int SumRecords(uint count, [In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] CcwRecord[] records);

    [PreserveSig]
    int GetDates(uint count, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] DateTime[] dates);

    [PreserveSig]
    int CountDates(uint count, [In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] DateTime[] dates);
}

[ComVisible(true), ClassInterface(ClassInterfaceType.None)]
public class CcwItem : ICcwItem
{
    public int value;

    public int Value()
    {
        return value;
    }
}

[ComVisible(true), ClassInterface(ClassInterfaceType.None)]
public class CcwItems : ICcwItems
{
    public int Next(uint count, ICcwItem[] items, out uint fetched)
    {
        items[0] = new CcwItem { value = 7 };
        fetched = 1;
        return 0;
    }

    public int GetRecords(uint count, CcwRecord[] records)
    {
        for (int i = 0; i < records.Length; i++)
            records[i] = new CcwRecord { Name = "record" + i, Value = 10 + i };
        return records.Length;
    }

    public int SumRecords(uint count, CcwRecord[] records)
    {
        int sum = 0;

        foreach (CcwRecord record in records)
            sum += record.Value + (record.Name == null ? 0 : record.Name.Length * 100);
        return sum;
    }

    public int GetDates(uint count, DateTime[] dates)
    {
        for (int i = 0; i < dates.Length; i++)
            dates[i] = new DateTime(2020 + i, 1, 2, 12, 0, 0);
        return dates.Length;
    }

    public int CountDates(uint count, DateTime[] dates)
    {
        int years = 0;

        foreach (DateTime date in dates)
            years += date.Year;
        return years;
    }

    public int Sum(uint count, ICcwItem[] items)
    {
        int sum = 0;

        foreach (ICcwItem item in items)
        {
            if (item != null)
                sum += item.Value();
        }
        return sum;
    }
}

public static class Program
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int NextFn(IntPtr self, uint count, IntPtr items, out uint fetched);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int SumFn(IntPtr self, uint count, IntPtr items);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int RecordsFn(IntPtr self, uint count, IntPtr records);

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
        CcwItems items = new CcwItems();
        IntPtr itf = Marshal.GetComInterfaceForObject(items, typeof(ICcwItems));
        IntPtr buffer = Marshal.AllocCoTaskMem(2 * IntPtr.Size);
        uint fetched = 0;

        Marshal.WriteIntPtr(buffer, 0, IntPtr.Zero);
        Marshal.WriteIntPtr(buffer, IntPtr.Size, IntPtr.Zero);
        int hr = Slot<NextFn>(itf, 3)(itf, 2, buffer, out fetched);
        IntPtr first = Marshal.ReadIntPtr(buffer, 0);
        IntPtr second = Marshal.ReadIntPtr(buffer, IntPtr.Size);
        Check(hr == 0, "Next hr " + hr);
        Check(fetched == 1, "Next fetched " + fetched);
        Check(first != IntPtr.Zero, "Next returns the element as an interface pointer");
        Check(second == IntPtr.Zero, "Next returns NULL for the unset element");
        if (first != IntPtr.Zero)
        {
            ICcwItem item = Marshal.GetObjectForIUnknown(first) as ICcwItem;
            Check(item != null && item.Value() == 7, "Next element value");
            Check(Marshal.Release(first) == 0, "Next element is returned with one reference");
        }

        IntPtr element = Marshal.GetComInterfaceForObject(new CcwItem { value = 5 }, typeof(ICcwItem));
        Marshal.WriteIntPtr(buffer, 0, element);
        Marshal.WriteIntPtr(buffer, IntPtr.Size, IntPtr.Zero);
        int sum = Slot<SumFn>(itf, 4)(itf, 2, buffer);
        Check(sum == 5, "Sum of the elements passed in " + sum);
        Check(Marshal.ReadIntPtr(buffer, 0) == element, "Sum leaves the caller's array unchanged");
        Marshal.Release(element);

        int recordSize = IntPtr.Size * 2;
        IntPtr records = Marshal.AllocCoTaskMem(2 * recordSize);
        for (int i = 0; i < 2 * recordSize; i++)
            Marshal.WriteByte(records, i, 0);
        int count = Slot<RecordsFn>(itf, 5)(itf, 2, records);
        Check(count == 2, "GetRecords result " + count);
        for (int i = 0; i < 2; i++)
        {
            IntPtr name = Marshal.ReadIntPtr(records, i * recordSize);
            int value = Marshal.ReadInt32(records, i * recordSize + IntPtr.Size);
            string text = name == IntPtr.Zero ? null : Marshal.PtrToStringBSTR(name);
            Check(text == "record" + i, "GetRecords element " + i + " name " + (text ?? "(null)"));
            Check(value == 10 + i, "GetRecords element " + i + " value " + value);
            if (name != IntPtr.Zero)
                Marshal.FreeBSTR(name);
        }

        IntPtr abc = Marshal.StringToBSTR("abc");
        Marshal.WriteIntPtr(records, 0, abc);
        Marshal.WriteInt32(records, IntPtr.Size, 5);
        Marshal.WriteIntPtr(records, recordSize, IntPtr.Zero);
        Marshal.WriteInt32(records, recordSize + IntPtr.Size, 7);
        int total = Slot<RecordsFn>(itf, 6)(itf, 2, records);
        Check(total == 312, "SumRecords of the records passed in " + total);
        Check(Marshal.ReadIntPtr(records, 0) == abc, "SumRecords leaves the caller's records unchanged");
        Marshal.FreeBSTR(abc);
        Marshal.FreeCoTaskMem(records);

        IntPtr dates = Marshal.AllocCoTaskMem(2 * 8);
        Marshal.WriteInt64(dates, 0, 0);
        Marshal.WriteInt64(dates, 8, 0);
        int dateCount = Slot<RecordsFn>(itf, 7)(itf, 2, dates);
        double firstDate = BitConverter.Int64BitsToDouble(Marshal.ReadInt64(dates, 0));
        double secondDate = BitConverter.Int64BitsToDouble(Marshal.ReadInt64(dates, 8));
        Check(dateCount == 2, "GetDates result " + dateCount);
        Check(firstDate == new DateTime(2020, 1, 2, 12, 0, 0).ToOADate(), "GetDates element 0 is an OLE date " + firstDate);
        Check(secondDate == new DateTime(2021, 1, 2, 12, 0, 0).ToOADate(), "GetDates element 1 is an OLE date " + secondDate);
        Marshal.WriteInt64(dates, 0, BitConverter.DoubleToInt64Bits(new DateTime(2000, 6, 1).ToOADate()));
        Marshal.WriteInt64(dates, 8, BitConverter.DoubleToInt64Bits(new DateTime(2001, 6, 1).ToOADate()));
        int years = Slot<RecordsFn>(itf, 8)(itf, 2, dates);
        Check(years == 4001, "CountDates of the OLE dates passed in " + years);
        Marshal.FreeCoTaskMem(dates);

        Marshal.FreeCoTaskMem(buffer);
        Marshal.Release(itf);
        Console.WriteLine("ccwarray: " + failures + " failures");
        return failures;
    }
}

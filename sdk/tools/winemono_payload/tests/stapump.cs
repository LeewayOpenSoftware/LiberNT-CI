/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test which blocking waits of an STA thread deliver messages sent from another thread
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

public static class Program
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateWindowExW(int exStyle, string className, string windowName, int style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);

    [DllImport("user32.dll")]
    static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);

    [DllImport("user32.dll")]
    static extern bool DestroyWindow(IntPtr hwnd);

    static IntPtr window;
    static int failures;

    static void Check(bool condition, string message)
    {
        Console.WriteLine((condition ? "ok: " : "FAILED: ") + message);
        if (!condition)
            failures++;
    }

    static bool Send()
    {
        IntPtr result;
        Thread.Sleep(500);
        return SendMessageTimeoutW(window, 0, IntPtr.Zero, IntPtr.Zero, 0, 4000, out result) != IntPtr.Zero;
    }

    static void Expect(string name, bool expected, Func<bool> scenario)
    {
        Stopwatch watch = Stopwatch.StartNew();
        bool delivered = scenario();
        Check(delivered == expected, name + " delivers a sent message " + delivered + " in " + watch.ElapsedMilliseconds + " ms");
    }

    static bool LockContention()
    {
        object gate = new object();
        ManualResetEvent held = new ManualResetEvent(false);
        bool delivered = false;
        Thread worker = new Thread(() => { lock (gate) { held.Set(); delivered = Send(); } });
        worker.Start();
        held.WaitOne();
        lock (gate) { }
        worker.Join();
        return delivered;
    }

    static bool Join()
    {
        bool delivered = false;
        Thread worker = new Thread(() => { delivered = Send(); });
        worker.Start();
        worker.Join();
        return delivered;
    }

    static bool WaitOne()
    {
        ManualResetEvent done = new ManualResetEvent(false);
        bool delivered = false;
        Thread worker = new Thread(() => { delivered = Send(); done.Set(); });
        worker.Start();
        done.WaitOne();
        worker.Join();
        return delivered;
    }

    static bool MonitorWait()
    {
        object gate = new object();
        bool delivered = false;
        bool finished = false;
        Thread worker = new Thread(() => { delivered = Send(); lock (gate) { finished = true; Monitor.Pulse(gate); } });
        lock (gate)
        {
            worker.Start();
            while (!finished)
                Monitor.Wait(gate);
        }
        worker.Join();
        return delivered;
    }

    static bool TaskWait()
    {
        bool delivered = false;
        Task.Run(() => { delivered = Send(); }).Wait();
        return delivered;
    }

    [STAThread]
    public static int Main()
    {
        window = CreateWindowExW(0, "STATIC", "stapump", 0, 0, 0, 0, 0, new IntPtr(-3), IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        Check(window != IntPtr.Zero, "message-only window created");
        Check(Thread.CurrentThread.GetApartmentState() == ApartmentState.STA, "main thread is STA");

        Expect("lock contention", true, LockContention);
        Expect("Thread.Join", true, Join);
        Expect("WaitHandle.WaitOne", true, WaitOne);
        Expect("Monitor.Wait", true, MonitorWait);
        Expect("Task.Wait", true, TaskWait);

        DestroyWindow(window);
        Console.WriteLine("stapump: " + failures + " failures");
        return failures;
    }
}

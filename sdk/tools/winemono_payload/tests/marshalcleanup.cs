/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test Marshal.CleanupUnusedObjectsInCurrentContext in STA and MTA threads
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class Program
{
    static int failures;

    static void Check(bool condition, string message)
    {
        Console.WriteLine((condition ? "ok: " : "FAILED: ") + message);
        if (!condition)
            failures++;
    }

    static string Cleanup()
    {
        try
        {
            Marshal.CleanupUnusedObjectsInCurrentContext();
            return null;
        }
        catch (Exception e)
        {
            return e.GetType().FullName;
        }
    }

    static void Run(ApartmentState state)
    {
        string error = "not run";
        bool available = true;
        Thread thread = new Thread(() =>
        {
            error = Cleanup();
            available = Marshal.AreComObjectsAvailableForCleanup();
        });
        thread.SetApartmentState(state);
        thread.Start();
        thread.Join();
        Check(error == null, "cleanup in " + state + " returns without exception " + (error ?? ""));
        Check(!available, "no objects available for cleanup in " + state);
    }

    public static int Main()
    {
        Run(ApartmentState.STA);
        Run(ApartmentState.MTA);
        Console.WriteLine("marshalcleanup: " + failures + " failures");
        return failures;
    }
}

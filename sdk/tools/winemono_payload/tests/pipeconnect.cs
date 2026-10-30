/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test asynchronous connections to a managed named pipe server
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.IO;
using System.IO.Pipes;
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

    static void Connect(string name, PipeDirection direction, PipeTransmissionMode mode, bool clientFirst)
    {
        Connect(name, direction, mode, clientFirst, false, false);
    }

    static void Connect(string name, PipeDirection direction, PipeTransmissionMode mode, bool clientFirst, bool clientCloses, bool clientProcess)
    {
        string pipe = "libernt-pipeconnect-" + name;
        PipeDirection clientDirection = direction == PipeDirection.In ? PipeDirection.Out : direction == PipeDirection.Out ? PipeDirection.In : PipeDirection.InOut;
        Exception error = null;
        bool connected = false;
        bool ended = false;

        using (NamedPipeServerStream server = new NamedPipeServerStream(pipe, direction, 1, mode, PipeOptions.Asynchronous))
        using (NamedPipeClientStream client = new NamedPipeClientStream(".", pipe, clientDirection, PipeOptions.Asynchronous))
        using (ManualResetEvent done = new ManualResetEvent(false))
        {
            if (clientFirst)
                client.Connect(5000);
            server.BeginWaitForConnection(result =>
            {
                try
                {
                    server.EndWaitForConnection(result);
                    ended = true;
                    connected = !server.SafePipeHandle.IsClosed;
                }
                catch (Exception e)
                {
                    error = e;
                }
                done.Set();
            }, null);
            if (clientProcess)
            {
                using (System.Diagnostics.Process child = System.Diagnostics.Process.Start(System.Reflection.Assembly.GetExecutingAssembly().Location, "client " + pipe + " " + clientDirection))
                    child.WaitForExit(10000);
            }
            else if (!clientFirst)
                client.Connect(5000);
            if (clientCloses)
                client.Dispose();
            bool signaled = done.WaitOne(10000);
            Check(signaled, name + ": connection callback ran");
            Check(ended, name + ": EndWaitForConnection " + (error == null ? "succeeded" : error.GetType().Name + " " + error.Message));
            Check(connected, name + ": server handle stays open");
        }
    }

    static void DisposeWhileWaiting(PipeTransmissionMode mode)
    {
        string name = "server-disposed-" + mode.ToString().ToLowerInvariant();
        NamedPipeServerStream server = new NamedPipeServerStream("libernt-pipeconnect-" + name, PipeDirection.In, 1, mode, PipeOptions.Asynchronous);
        Exception error = null;
        bool ended = false;

        using (ManualResetEvent done = new ManualResetEvent(false))
        {
            server.BeginWaitForConnection(result =>
            {
                try
                {
                    server.EndWaitForConnection(result);
                    ended = true;
                }
                catch (Exception e)
                {
                    error = e;
                }
                done.Set();
            }, null);
            server.Dispose();
            Check(done.WaitOne(10000), name + ": connection callback ran");
            Check(!ended, name + ": EndWaitForConnection fails");
            Check(error is ObjectDisposedException, name + ": EndWaitForConnection throws " + (error == null ? "nothing" : error.GetType().FullName + " " + error.Message));
        }
    }

    public static int Main(string[] args)
    {
        if (args.Length == 3 && args[0] == "client")
        {
            using (NamedPipeClientStream client = new NamedPipeClientStream(".", args[1], (PipeDirection)Enum.Parse(typeof(PipeDirection), args[2]), PipeOptions.Asynchronous))
                client.Connect(5000);
            return 0;
        }

        Connect("in-byte", PipeDirection.In, PipeTransmissionMode.Byte, false);
        Connect("in-message", PipeDirection.In, PipeTransmissionMode.Message, false);
        Connect("inout-byte", PipeDirection.InOut, PipeTransmissionMode.Byte, false);
        Connect("inout-message", PipeDirection.InOut, PipeTransmissionMode.Message, false);
        Connect("in-byte-client-first", PipeDirection.In, PipeTransmissionMode.Byte, true);
        Connect("inout-message-client-first", PipeDirection.InOut, PipeTransmissionMode.Message, true);
        Connect("in-byte-client-closes", PipeDirection.In, PipeTransmissionMode.Byte, false, true, false);
        Connect("in-message-client-closes", PipeDirection.In, PipeTransmissionMode.Message, false, true, false);
        Connect("in-byte-client-process", PipeDirection.In, PipeTransmissionMode.Byte, false, false, true);
        Connect("in-message-client-process", PipeDirection.In, PipeTransmissionMode.Message, false, false, true);
        DisposeWhileWaiting(PipeTransmissionMode.Byte);
        DisposeWhileWaiting(PipeTransmissionMode.Message);
        Console.WriteLine("pipeconnect: " + failures + " failures");
        return failures;
    }
}

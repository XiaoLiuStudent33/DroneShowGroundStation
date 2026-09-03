// Test-only helper. Attach only to the isolated fixture's Console and send Ctrl+C.
using System;
using System.Runtime.InteropServices;

class ConsoleStop
{
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll")] static extern bool FreeConsole();
    [DllImport("kernel32.dll")] static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll")] static extern bool GenerateConsoleCtrlEvent(uint ctrl, uint group);
    static int Main(string[] args)
    {
        uint pid;
        if (args.Length != 1 || !uint.TryParse(args[0], out pid)) return 1;
        if (!AttachConsole(pid)) return 2;
        try
        {
            if (!SetConsoleCtrlHandler(IntPtr.Zero, true)) return 3;
            return GenerateConsoleCtrlEvent(0, 0) ? 0 : 4;
        }
        finally { FreeConsole(); }
    }
}

// Test-only parent: exits normally without sending STOP. Child must notice owner exit.
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

class FakeOwner
{
    static int Main(string[] args)
    {
        var token = Process.GetCurrentProcess().Id + "_" + Guid.NewGuid();
        using (var ready = new EventWaitHandle(false, EventResetMode.ManualReset,
            "Local\\DroneGCS.Managed." + token + ".ready"))
        using (var stop = new EventWaitHandle(false, EventResetMode.ManualReset,
            "Local\\DroneGCS.Managed." + token + ".stop"))
        {
            var config = args.Length == 1 ? args[0] : "gcs-config.ini";
            File.WriteAllText("owner-session-token.txt", token);
            var info = new ProcessStartInfo("DroneGCS.exe", config + " --launcher-session " + token);
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            using (var backend = Process.Start(info))
            {
                File.WriteAllText("owner-child-pid.txt", backend.Id.ToString());
                if (!ready.WaitOne(10000)) { stop.Set(); return 1; }
                return 0;
            }
        }
    }
}

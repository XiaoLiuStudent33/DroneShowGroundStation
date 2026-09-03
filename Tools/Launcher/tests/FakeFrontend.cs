// Test-only replacement for UE. Receives DSP telemetry over loopback, never sends.
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;

class FakeFrontend
{
    static int Main()
    {
        try
        {
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 12002)))
            {
                socket.Client.ReceiveTimeout = 5000;
                IPEndPoint remote = new IPEndPoint(IPAddress.Any, 0);
                var frame = socket.Receive(ref remote);
                if (frame.Length < 20 || !remote.Address.Equals(IPAddress.Loopback) || remote.Port != 12001)
                    return 2;
                File.WriteAllText("frontend-result.txt", "DSP telemetry received: " + frame.Length +
                    "\nArtifactWorkspace=" + Environment.GetEnvironmentVariable("DRONEGCS_ARTIFACT_WORKSPACE"));
                Thread.Sleep(4000); // Allows duplicate-launch test while both children live.
                return 0;
            }
        }
        catch (Exception e)
        {
            File.WriteAllText("frontend-result.txt", e.ToString());
            return 3;
        }
    }
}

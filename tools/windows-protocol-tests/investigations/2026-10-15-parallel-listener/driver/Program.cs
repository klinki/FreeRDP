using System.Net;
using System.Net.Sockets;
using System.Reflection;
using Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr;
using Microsoft.Protocols.TestTools.StackSdk.Transport;

static class Program
{
    const BindingFlags Fields = BindingFlags.Instance | BindingFlags.NonPublic;
    static object Get(object target, string name) => target.GetType().GetField(name, Fields)!.GetValue(target)!;
    static void Require(bool condition, string message) { if (!condition) throw new Exception(message); }
    static int FreePort() { using var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp); socket.Bind(new IPEndPoint(IPAddress.Loopback, 0)); return ((IPEndPoint)socket.LocalEndPoint!).Port; }
    static void Invoke(object target, string method)
    {
        try { target.GetType().GetMethod(method)!.Invoke(target, null); }
        catch (TargetInvocationException error) { System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException!).Throw(); }
    }
    static Socket SocketOf(RdpbcgrServer server) => (Socket)Get(Get(server, "directedTransportStack"), "listenSock");
    static int BoundPort(RdpbcgrServer server) => ((IPEndPoint)SocketOf(server).LocalEndPoint!).Port;
    static void Connect(RdpbcgrServer server, int port)
    {
        using var peer = new TcpClient();
        peer.Connect(IPAddress.Loopback, port);
        var session = server.ExpectConnect(TimeSpan.FromSeconds(2));
        Require(((IPEndPoint)session.LocalIdentity).Port == port, "Accepted session did not retain actual bound port");
    }
    static Socket Occupy(int port = 0)
    {
        var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        socket.Bind(new IPEndPoint(IPAddress.Loopback, port)); socket.Listen(1); return socket;
    }
    static void Baseline()
    {
        using var occupied = Occupy(); int port = ((IPEndPoint)occupied.LocalEndPoint!).Port;
        using var server = new RdpbcgrServer(port, EncryptedProtocol.NegotiationTls);
        try {
            server.Start(IPAddress.Loopback);
            Console.WriteLine($"BASELINE Start returned success for occupied port {port}");
        } catch (SocketException error) { throw new Exception("Baseline unexpectedly synchronous", error); }
        try { server.ExpectConnect(TimeSpan.FromSeconds(2)); throw new Exception("Expected queued bind failure"); }
        catch (InvalidOperationException error) {
            Console.WriteLine($"BASELINE asynchronous exception: {error}");
            Require(error.InnerException is SocketException socket && socket.SocketErrorCode == SocketError.AddressAlreadyInUse, "Missing original bind exception");
        }
    }
    static void OccupiedFailure(EncryptedProtocol protocol)
    {
        using var occupied = Occupy(); int port = ((IPEndPoint)occupied.LocalEndPoint!).Port;
        using var server = new RdpbcgrServer(port, protocol);
        try { server.Start(IPAddress.Loopback); throw new Exception("Start returned with an occupied port"); }
        catch (SocketException error) {
            Console.WriteLine($"SOCKET {protocol}: {error.SocketErrorCode} native={error.NativeErrorCode}");
            Require(error.SocketErrorCode == SocketError.AddressAlreadyInUse, "SocketError was lost");
        }
        var transport = Get(server, "directedTransportStack");
        Require(Get(transport, "listenSock") == null, "Failed start leaked socket");
        Require(Get(transport, "acceptThread") == null, "Failed start leaked thread");
        Require(Get(transport, "acceptThreadCancellationTokenSource") == null, "Failed start leaked cancellation source");
        Require(!(bool)Get(transport, "started"), "Failed start set started");
        occupied.Dispose();
        Invoke(transport, "Start");
        Require(BoundPort(server) == port, "Retry did not own requested port");
        Invoke(transport, "Stop");
        using var rebound = Occupy(port);
        Console.WriteLine($"CLEANUP retry and rebind succeeded on {port}");
    }
    static void Restart()
    {
        int port = FreePort();
        using var server = new RdpbcgrServer(port, EncryptedProtocol.NegotiationTls);
        server.Start(IPAddress.Loopback);
        var transport = Get(server, "directedTransportStack");
        for (int iteration = 0; iteration < 12; iteration++) {
            Invoke(transport, "Stop");
            Require(!(bool)Get(transport, "isTcpServerTransportStarted"), "Stop left transport started");
            using (var rebound = Occupy(port)) { }
            Invoke(transport, "Start");
            Require(BoundPort(server) == port, "Restart changed fixed port");
        }
        // Unexpected shutdown exceptions would be returned before this connection.
        Connect(server, port);
    }
    static void ParallelListeners()
    {
        const int count = 32;
        var servers = new RdpbcgrServer[count];
        try {
            Parallel.For(0, count, index => {
                var server = new RdpbcgrServer(0, EncryptedProtocol.NegotiationTls);
                servers[index] = server; server.Start(IPAddress.Loopback);
                Require(server.ServerPort > 0 && server.ServerPort == BoundPort(server), "Port zero was not replaced by actual bound port");
            });
            var ports = servers.Select(server => server.ServerPort).ToArray();
            Require(ports.Distinct().Count() == count, "Concurrent listeners collided");
            Parallel.For(0, count, index => Connect(servers[index], ports[index]));
            Console.WriteLine($"PARALLEL {count} distinct listeners accepted connections: {string.Join(",", ports)}");
        } finally { foreach (var server in servers) server?.Dispose(); }
    }
    static void ProtocolReady(EncryptedProtocol protocol)
    {
        using var server = new RdpbcgrServer(0, protocol);
        server.Start(IPAddress.Loopback);
        Require(server.ServerPort > 0 && BoundPort(server) == server.ServerPort, $"{protocol} returned before binding");
        // Direct CredSSP authenticates inside accept; establishing its SSPI session
        // requires Windows and is deliberately outside these listener checks.
        if (protocol != EncryptedProtocol.DirectCredSsp) Connect(server, server.ServerPort);
    }
    static void RawRdp()
    {
        int port = FreePort();
        using var server = new RdpbcgrServer(port, EncryptedProtocol.Rdp);
        server.Start(IPAddress.Loopback); Connect(server, port);
    }
    public static int Main(string[] args)
    {
        if (args.Contains("--baseline")) { Baseline(); return 0; }
        var tests = new (string Name, Action Run)[] {
            ("tls_start_is_ready_and_reports_os_port", () => ProtocolReady(EncryptedProtocol.NegotiationTls)),
            ("negotiated_credssp_listener_readiness", () => ProtocolReady(EncryptedProtocol.NegotiationCredSsp)),
            ("direct_credssp_listener_readiness_without_authentication", () => ProtocolReady(EncryptedProtocol.DirectCredSsp)),
            ("tls_occupied_port_throws_and_cleans_up_for_retry", () => OccupiedFailure(EncryptedProtocol.NegotiationTls)),
            ("negotiated_credssp_occupied_port_throws_and_cleans_up", () => OccupiedFailure(EncryptedProtocol.NegotiationCredSsp)),
            ("direct_credssp_occupied_port_throws_and_cleans_up", () => OccupiedFailure(EncryptedProtocol.DirectCredSsp)),
            ("fixed_port_stop_restart_keeps_queue_clean", Restart),
            ("32_parallel_kernel_selected_listeners_accept", ParallelListeners),
            ("standard_rdp_fixed_port_remains_ready", RawRdp),
        };
        int failures = 0;
        foreach (var test in tests) {
            try { test.Run(); Console.WriteLine($"PASS {test.Name}"); }
            catch (Exception error) { failures++; Console.WriteLine($"FAIL {test.Name}: {error}"); }
        }
        Console.WriteLine($"{tests.Length - failures}/{tests.Length} passed"); return failures == 0 ? 0 : 1;
    }
}

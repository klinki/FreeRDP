using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Threading;
using Microsoft.Protocols.TestTools.StackSdk;
using Microsoft.Protocols.TestTools.StackSdk.Transport;
using Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr;

sealed class OrdinaryPacket : StackPacket
{
    public override StackPacket Clone() => new OrdinaryPacket();
    public override byte[] ToBytes() => new byte[] { 1 };
}
sealed class ResetStream : MemoryStream
{
    public override int Read(byte[] buffer, int offset, int count) =>
        throw new IOException("Synthetic connection reset", new SocketException((int)SocketError.ConnectionReset));
}
sealed class Fixture : IDisposable
{
    internal readonly RdpbcgrServer Server = new RdpbcgrServer(0, EncryptedProtocol.NegotiationTls);
    internal readonly RdpbcgrServerSessionContext Session;
    internal readonly QueueManager Queue;
    internal readonly object Endpoint = new IPEndPoint(IPAddress.Loopback, 40001);
    private static readonly BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    public Fixture()
    {
        // Use the actual SDK queue without opening sockets or starting a receive thread.
        Type transportType = typeof(RdpbcgrServer).Assembly.GetType(
            "Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr.RdpbcgrServerTransportStack", true)!;
        object transport = Activator.CreateInstance(transportType, new object?[] {
            Server, new RdpcbgrServerTransportConfig(SecurityStreamType.Ssl, IPAddress.Loopback, 0), null
        })!;
        typeof(RdpbcgrServer).GetField("directedTransportStack", Private)!.SetValue(Server, transport);
        Queue = (QueueManager)transportType.GetProperty("PacketQueue")!.GetValue(transport)!;
        Session = new RdpbcgrServerSessionContext { Identity = Endpoint, Server = Server };
        typeof(RdpbcgrServerContext).GetMethod("AddSession", Private)!.Invoke(Server.ServerContext, new object[] { Session });
    }
    internal void Add(EventType type, object? detail = null, object? endpoint = null) =>
        Queue.AddObject(new TransportEvent(type, endpoint ?? Endpoint, detail));
    internal void ReceiveFrom(Stream stream)
    {
        // Run the real TLS EOF/reset conversion synchronously with controlled stream input.
        Type receiverType = typeof(RdpbcgrServer).Assembly.GetType(
            "Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr.RdpbcgrReceiveThread", true)!;
        Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr.DecodePacketCallback decoder = (object endpoint, byte[] bytes, out int consumed, out int expected) => {
            consumed = bytes.Length; expected = 1; return Array.Empty<StackPacket>();
        };
        object receiver = Activator.CreateInstance(receiverType, new object[] { Endpoint, Queue, decoder, stream, 32 })!;
        using var cancellation = new CancellationTokenSource();
        receiverType.GetField("receiveThreadCancellationTokenSource", Private)!.SetValue(receiver, cancellation);
        receiverType.GetMethod("ReceiveLoopInternal", Private)!.Invoke(receiver, null);
    }
    public void Dispose() { Session.Dispose(); Server.Dispose(); }
}
static class Program
{
    static void Require(bool condition, string message) { if (!condition) throw new Exception(message); }
    static void Timeout(Action action)
    {
        var timer = Stopwatch.StartNew();
        try { action(); throw new Exception("Ordinary traffic incorrectly satisfied disconnect"); }
        catch (TimeoutException) {
            Require(timer.Elapsed >= TimeSpan.FromMilliseconds(100), "Wait returned before bounded deadline");
            Require(timer.Elapsed < TimeSpan.FromSeconds(3), "Wait exceeded bounded deadline");
        }
    }
    public static int Main()
    {
        var cases = new (string Name, Action Run)[] {
            ("ordinary_packet_requires_timeout", () => {
                using var f = new Fixture(); var packet = new OrdinaryPacket(); f.Add(EventType.ReceivedPacket, packet);
                Timeout(() => f.Server.ExpectDisconnect(f.Session, TimeSpan.FromMilliseconds(150)));
                Require(ReferenceEquals(f.Session.GetPacketFromBuffer(), packet), "Ordinary packet was not preserved");
                Require(f.Server.ServerContext.LookupSession(f.Endpoint) == f.Session, "Live session was removed");
            }),
            ("matching_disconnect_succeeds", () => {
                using var f = new Fixture(); f.Add(EventType.Disconnected);
                f.Server.ExpectDisconnect(f.Session, TimeSpan.FromSeconds(1));
                Require(f.Server.ServerContext.LookupSession(f.Endpoint) == null, "Disconnected session was not removed");
            }),
            ("ordinary_packet_then_disconnect_succeeds", () => {
                using var f = new Fixture(); var packet = new OrdinaryPacket();
                f.Add(EventType.ReceivedPacket, packet); f.Add(EventType.Disconnected);
                f.Server.ExpectDisconnect(f.Session, TimeSpan.FromSeconds(1));
                Require(ReferenceEquals(f.Session.GetPacketFromBuffer(), packet), "Ordinary packet was not preserved");
            }),
            ("decoder_exception_is_not_disconnect", () => {
                using var f = new Fixture(); var sentinel = new FormatException("Synthetic decoder failure"); f.Add(EventType.Exception, sentinel);
                try { f.Server.ExpectDisconnect(f.Session, TimeSpan.FromSeconds(1)); throw new Exception("Decoder failure incorrectly satisfied disconnect"); }
                catch (InvalidOperationException error) { Require(ReferenceEquals(error.InnerException, sentinel), "Original decoder failure was lost"); }
            }),
            ("other_session_disconnect_is_rejected", () => {
                using var f = new Fixture(); f.Add(EventType.Disconnected, endpoint: new IPEndPoint(IPAddress.Loopback, 40002));
                try { f.Server.ExpectDisconnect(f.Session, TimeSpan.FromSeconds(1)); throw new Exception("Other session incorrectly satisfied disconnect"); }
                catch (InvalidOperationException error) { Require(error.Message.Contains("Unwanted disconnection"), "Unexpected transport error"); }
            }),
            ("tls_eof_is_disconnect", () => {
                using var f = new Fixture(); using var stream = new MemoryStream(); f.ReceiveFrom(stream);
                f.Server.ExpectDisconnect(f.Session, TimeSpan.FromSeconds(1));
            }),
            ("tls_connection_reset_is_disconnect", () => {
                using var f = new Fixture(); using var stream = new ResetStream(); f.ReceiveFrom(stream);
                f.Server.ExpectDisconnect(f.Session, TimeSpan.FromSeconds(1));
            }),
        };
        int failures = 0;
        foreach (var item in cases) {
            try { item.Run(); Console.WriteLine($"PASS {item.Name}"); }
            catch (Exception error) { failures++; Console.WriteLine($"FAIL {item.Name}: {error.GetType().Name}: {error.Message}"); }
        }
        Console.WriteLine($"{cases.Length - failures}/{cases.Length} passed");
        return failures == 0 ? 0 : 1;
    }
}

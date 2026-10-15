using System.Collections;
using System.Net;
using System.Reflection;
using Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpeudp;
using Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpemt;

static class Program
{
    const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    static readonly FieldInfo? Hook = typeof(RdpeudpTLSChannel).GetField("PublicationObserverForRegression", BindingFlags.Static | BindingFlags.Public);
    static void Require(bool condition, string message) { if (!condition) throw new Exception(message); }
    static RdpeudpSocket Socket(bool automatic) => new(TransportMode.Reliable, new IPEndPoint(IPAddress.Loopback, 40001), automatic, (_, _) => {});
    static Stream Inner(RdpeudpTLSChannel channel) => (Stream)typeof(RdpeudpTLSChannel).GetField("innerStream", Private)!.GetValue(channel)!;
    static int Buffered(RdpeudpTLSChannel channel) => ((IList)Inner(channel).GetType().GetField("receivedBuffer", Private)!.GetValue(Inner(channel))!).Count;
    static void Deliver(RdpeudpSocket socket)
    {
        byte[] first = { 1, 2, 3 }; byte[] second = { 4, 5 };
        socket.ReceiveDataOnHigherLayer(first); first[0] = 99;
        socket.ReceiveDataOnHigherLayer(second); second[0] = 88;
    }
    static void VerifyBytes(RdpeudpTLSChannel channel)
    {
        Require(!channel.IsAuthenticated, "Unexpected authentication");
        Require(Buffered(channel) == 2, "Received chunks were dropped or duplicated");
        var stream = Inner(channel); byte[] bytes = new byte[5];
        Require(stream.Read(bytes, 0, 2) == 2, "First partial read failed");
        Require(stream.Read(bytes, 2, 1) == 1, "Remainder read failed");
        Require(stream.Read(bytes, 3, 2) == 2, "Second chunk read failed");
        Require(bytes.SequenceEqual(new byte[] { 1, 2, 3, 4, 5 }), "Bytes were reordered or caller mutation leaked into buffer");
        Require(Buffered(channel) == 0, "Duplicate data remained after reading");
    }
    static void EarliestCallback(bool automatic)
    {
        Require(Hook != null, "This test requires the explicitly instrumented SDK build");
        var socket = Socket(automatic);
        int callbacks = 0;
        Action<RdpeudpTLSChannel> observer = channel => {
            int constructorThread = Environment.CurrentManagedThreadId;
            Task.Run(() => {
                Require(Environment.CurrentManagedThreadId != constructorThread, "Callback must use a separate thread");
                Deliver(socket); Interlocked.Increment(ref callbacks);
            }).GetAwaiter().GetResult();
        };
        Hook!.SetValue(null, observer);
        try {
            using var channel = new RdpeudpTLSChannel(socket);
            Require(callbacks == 1, "Constructor publication observer not called exactly once");
            VerifyBytes(channel);
        } finally { Hook.SetValue(null, null); }
    }
    static void AfterConstruction(bool automatic)
    {
        var socket = Socket(automatic);
        using var channel = new RdpeudpTLSChannel(socket);
        Deliver(socket); VerifyBytes(channel);
    }
    public static int Main(string[] args)
    {
        var tests = new List<(string Name, Action Run)> {
            ("public_callback_retains_order_and_copies_non_auto", () => AfterConstruction(false)),
            ("public_callback_retains_order_and_copies_auto", () => AfterConstruction(true)),
            ("null_and_empty_receive_do_not_queue_data", () => {
                var socket = Socket(false); using var channel = new RdpeudpTLSChannel(socket);
                channel.ReceiveBytes(null!); channel.ReceiveBytes(Array.Empty<byte>());
                Require(Buffered(channel) == 0, "Empty receive added data");
            }),
            ("dispose_unsubscribes_socket_callbacks", () => {
                var socket = Socket(false); var channel = new RdpeudpTLSChannel(socket);
                var callback = typeof(RdpeudpSocket).GetField("Received", Private)!;
                Require(callback.GetValue(socket) != null, "No callback subscription");
                channel.Dispose(); Require(callback.GetValue(socket) == null, "Receive callback retained after dispose");
            }),
            ("send_still_requires_authentication", () => {
                using var channel = new RdpeudpTLSChannel(Socket(false));
                try { channel.Send(new byte[] { 1 }); throw new Exception("Unauthenticated send was accepted"); }
                catch (NotSupportedException) { }
            }),
            ("null_socket_rejected", () => {
                try { using var channel = new RdpeudpTLSChannel(null!); throw new Exception("Null socket was accepted"); }
                catch (NotSupportedException) { }
            }),
            ("lossy_socket_rejected", () => {
                var socket = new RdpeudpSocket(TransportMode.Lossy, new IPEndPoint(IPAddress.Loopback, 40001), false, (_, _) => {});
                try { using var channel = new RdpeudpTLSChannel(socket); throw new Exception("Lossy socket was accepted"); }
                catch (NotSupportedException) { }
            }),
        };
        if (args.Contains("--instrumented")) {
            tests.Add(("earliest_callback_buffers_bytes_non_auto", () => EarliestCallback(false)));
            tests.Add(("earliest_callback_buffers_bytes_auto", () => EarliestCallback(true)));
        } else Require(Hook == null, "Uninstrumented qualification must not load a scheduling hook");
        int failures = 0;
        foreach (var test in tests) {
            try { test.Run(); Console.WriteLine($"PASS {test.Name}"); }
            catch (Exception error) { failures++; Console.WriteLine($"FAIL {test.Name}: {error}"); }
        }
        Console.WriteLine($"{tests.Count - failures}/{tests.Count} passed"); return failures == 0 ? 0 : 1;
    }
}

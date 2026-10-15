using System.Collections;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using Microsoft.Protocols.TestSuites.Rdpbcgr;
using Microsoft.Protocols.TestTools.StackSdk;
using Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr;
using Microsoft.Protocols.TestTools.StackSdk.Transport;

// Call the actual adapter entry points and capture bytes from the SDK's real
// send path. Only the established TLS stream is replaced with memory; no
// adapter mutation or serialization code is copied into this regression.
sealed class Fixture : IDisposable
{
    const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    readonly RdpbcgrServer server = new(0, EncryptedProtocol.NegotiationTls);
    readonly RdpbcgrServerSessionContext session;
    readonly RdpbcgrAdapter adapter = new();
    readonly MemoryStream stream = new();
    readonly TcpClient peer = new();
    readonly Socket socket;
    readonly object transport;
    readonly IDictionary receivers;

    public Fixture(bool encrypted = false)
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        peer.Connect((IPEndPoint)listener.LocalEndpoint);
        socket = listener.AcceptSocket();
        var endpoint = socket.RemoteEndPoint!;
        Type transportType = typeof(RdpbcgrServer).Assembly.GetType(
            "Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr.RdpbcgrServerTransportStack", true)!;
        transport = Activator.CreateInstance(transportType, new object?[] {
            server, new RdpcbgrServerTransportConfig(SecurityStreamType.Ssl, IPAddress.Loopback, 0), null
        })!;
        Set(server, "directedTransportStack", transport);
        Set(transport, "isTcpServerTransportStarted", true);
        var queue = (QueueManager)transportType.GetProperty("PacketQueue")!.GetValue(transport)!;
        Type receiverType = typeof(RdpbcgrServer).Assembly.GetType(
            "Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr.RdpbcgrReceiveThread", true)!;
        Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpbcgr.DecodePacketCallback decoder =
            (object remote, byte[] bytes, out int consumed, out int expected) => {
                consumed = bytes.Length; expected = 1; return Array.Empty<StackPacket>();
            };
        object receiver = Activator.CreateInstance(receiverType,
            new object[] { endpoint, queue, decoder, stream, 4096 })!;
        receivers = (IDictionary)transportType.GetField("receivingStreams", Private)!.GetValue(transport)!;
        receivers.Add(socket, receiver);
        session = new RdpbcgrServerSessionContext {
            Identity = endpoint, LocalIdentity = socket.LocalEndPoint, Server = server,
            UserChannelId = 1002, ServerChannelId = 1002,
        };
        Set(session, "ioChannelId", (ushort)1003);
        if (encrypted)
        {
            var response = new Server_MCS_Connect_Response_Pdu_with_GCC_Conference_Create_Response {
                mcsCrsp = new MCSConnectResponse(),
            };
            response.mcsCrsp.gccPdu.serverSecurityData.encryptionLevel = EncryptionLevel.ENCRYPTION_LEVEL_HIGH;
            response.mcsCrsp.gccPdu.serverSecurityData.encryptionMethod = EncryptionMethods.ENCRYPTION_METHOD_128BIT;
            Set(session, "mcsConnectResponsePdu", response);
            var algorithm = new EncryptionAlgorithm(EncryptionMethods.ENCRYPTION_METHOD_128BIT);
            byte[] clientRandom = Enumerable.Range(0, 32).Select(value => (byte)value).ToArray();
            byte[] serverRandom = Enumerable.Range(32, 32).Select(value => (byte)value).ToArray();
            typeof(EncryptionAlgorithm).GetMethod("GenerateSessionKey", Private)!.Invoke(
                algorithm, new object[] { clientRandom, serverRandom });
            Set(session, "encryptionAlgorithm", algorithm);
        }
        Set(adapter, "rdpbcgrServerStack", server);
        Set(adapter, "sessionContext", session);
    }

    static void Set(object instance, string name, object value) =>
        instance.GetType().GetField(name, Private)!.SetValue(instance, value);

    public byte[] Disconnect(NegativeType kind, bool admin = true)
    {
        adapter.Server_MCSDisconnectProviderUltimatum(admin, kind);
        return stream.ToArray();
    }

    public uint EncryptionCount => session.EncryptionCount;

    public byte[] Pointer(SlowPathTest_InvalidType kind)
    {
        adapter.SendSlowPathOutputPdu(kind);
        return stream.ToArray();
    }

    public void Dispose()
    {
        // No receiver thread was started, so remove the fixture before the
        // production dispose code tries to cancel an active receive loop.
        receivers.Clear();
        Set(transport, "isTcpServerTransportStarted", false);
        session.Dispose(); server.Dispose(); socket.Dispose(); peer.Dispose(); stream.Dispose();
    }
}

static class Program
{
    static void Require(bool condition, string message) { if (!condition) throw new Exception(message); }
    static int Tpkt(byte[] bytes) => (bytes[2] << 8) | bytes[3];
    static byte[] Disconnect(NegativeType kind, bool admin = true)
    { using var fixture = new Fixture(); return fixture.Disconnect(kind, admin); }
    static byte[] Pointer(SlowPathTest_InvalidType kind)
    { using var fixture = new Fixture(); return fixture.Pointer(kind); }
    static void CheckFrame(byte[] bytes, int actual, int declared)
    {
        Console.WriteLine($"WIRE actual={bytes.Length} declared={Tpkt(bytes)} hex={Convert.ToHexString(bytes)}");
        Require(bytes.Length == actual, $"Expected {actual} bytes, got {bytes.Length}");
        Require(bytes[0] == 3 && bytes[1] == 0, "TPKT version/reserved changed");
        Require(Tpkt(bytes) == declared, $"Expected declared size {declared}, got {Tpkt(bytes)}");
    }
    static void OnlyByteChanged(byte[] original, byte[] modified, int offset, int expected)
    {
        Require(original.Length == modified.Length, "Packet size changed");
        Require(modified[offset] == expected, $"Byte {offset} has unexpected value");
        for (int index = 0; index < original.Length; index++)
            if (index != offset) Require(original[index] == modified[index], $"Unexpected mutation at byte {index}");
    }

    public static int Main()
    {
        var cases = new (string Name, Action Run)[] {
            ("disconnect_positive_remains_9_bytes", () => CheckFrame(Disconnect(NegativeType.None), 9, 9)),
            ("disconnect_negative_declares_8_of_9_bytes", () => {
                var bytes = Disconnect(NegativeType.InvalidTPKTHeader);
                CheckFrame(bytes, 9, 8);
                OnlyByteChanged(Disconnect(NegativeType.None), bytes, 3, 8);
            }),
            ("provider_disconnect_positive_remains_9_bytes", () => CheckFrame(Disconnect(NegativeType.None, false), 9, 9)),
            ("provider_disconnect_negative_declares_8_of_9_bytes", () => {
                var bytes = Disconnect(NegativeType.InvalidTPKTHeader, false);
                CheckFrame(bytes, 9, 8);
                OnlyByteChanged(Disconnect(NegativeType.None, false), bytes, 3, 8);
            }),
            ("pointer_positive_remains_40_bytes", () => CheckFrame(Pointer(SlowPathTest_InvalidType.None), 40, 40)),
            ("pointer_negative_declares_39_of_40_bytes", () => {
                var bytes = Pointer(SlowPathTest_InvalidType.InvalidTPKTLength);
                CheckFrame(bytes, 40, 39);
                OnlyByteChanged(Pointer(SlowPathTest_InvalidType.None), bytes, 3, 39);
            }),
            ("pointer_mcs_negative_remains_independent", () => {
                var bytes = Pointer(SlowPathTest_InvalidType.InvalidMCSLength);
                CheckFrame(bytes, 40, 40);
                OnlyByteChanged(Pointer(SlowPathTest_InvalidType.None), bytes, 13, 25);
            }),
            ("encrypted_pointer_serializes_once_and_only_tpkt_length_changes", () => {
                using var positive = new Fixture(encrypted: true);
                using var negative = new Fixture(encrypted: true);
                var original = positive.Pointer(SlowPathTest_InvalidType.None);
                var bytes = negative.Pointer(SlowPathTest_InvalidType.InvalidTPKTLength);
                CheckFrame(original, 52, 52);
                CheckFrame(bytes, 52, 51);
                Require(positive.EncryptionCount == 1, "Positive encryption count is not one");
                Require(negative.EncryptionCount == 1, "Negative encryption count is not one");
                OnlyByteChanged(original, bytes, 3, 51);
            }),
            ("pointer_x224_negative_remains_independent", () => {
                var bytes = Pointer(SlowPathTest_InvalidType.InvalidX224Length);
                CheckFrame(bytes, 40, 40);
                OnlyByteChanged(Pointer(SlowPathTest_InvalidType.None), bytes, 4, 1);
            }),
        };
        int failures = 0;
        foreach (var item in cases) {
            try { item.Run(); Console.WriteLine($"PASS {item.Name}"); }
            catch (Exception error) { failures++; Console.WriteLine($"FAIL {item.Name}: {error}"); }
        }
        Console.WriteLine($"{cases.Length - failures}/{cases.Length} passed");
        return failures == 0 ? 0 : 1;
    }
}

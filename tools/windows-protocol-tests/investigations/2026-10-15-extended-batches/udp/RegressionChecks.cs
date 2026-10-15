using System;
using System.Diagnostics;
using System.Linq;
using System.Net;
using Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpeudp;
using Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpeudp2;
using Microsoft.Protocols.TestTools.StackSdk.RemoteDesktop.Rdpeudp2.Types;

static class Program
{
    static int count;
    static void Check(bool ok, string name)
    {
        if (!ok) throw new Exception(name);
        Console.WriteLine("PASS " + name); count++;
    }
    static Rdpeudp2ProtocolHandler Handler() => new(false, _ => {}, () => {}, _ => {});
    static void Ack(ushort expected, ushort received, bool shouldMatch)
    {
        using var h = Handler();
        var p = new Rdpeudp2Packet();
        p.Header.Flags = Rdpeudp2PacketHeaderFlags.ACK;
        p.ACK = new AcknowledgementPayload { SeqNum = received, delayAckTimeAdditions = Array.Empty<byte>() };
        h.ReceivePacket(p);
        Check((h.ExpectAckPacket(TimeSpan.FromMilliseconds(30), expected) != null) == shouldMatch,
            $"ACK({received}) expected {expected}: {shouldMatch}");
    }
    public static void Main()
    {
        foreach (bool timestamp in new[]{false, true})
        {
            var original = new AcknowledgementVectorPayload { BaseSeqNum=7, codedAckVecSize=1,
                TimeStampPresent=timestamp, TimeStamp=timestamp ? 123u : null,
                SendAckTimeGapInMs=timestamp ? (byte)4 : null, codedAckVector=new byte[]{2} };
            var wire=original.ToBytes();
            var actual=new AcknowledgementVectorPayload(wire, out int used);
            Check(used==wire.Length && actual.TimeStampPresent==timestamp && actual.BaseSeqNum==7
                && actual.codedAckVector.SequenceEqual(new byte[]{2}) && actual.GetAckedSeq().Contains(8),
                $"ACKVEC optional timestamp {timestamp}");
        }
        Ack(7,7,true); Ack(7,8,true); Ack(7,6,false);
        Ack(65535,0,true); Ack(0,65535,false); Ack(0,32768,false);
        using(var h = Handler())
        {
            var timer=Stopwatch.StartNew();
            var p=h.CreateDataPacket(new byte[]{1});
            Check(timer.Elapsed < TimeSpan.FromSeconds(1) && p.DataBody.HasValue && !p.ACK.HasValue,
                "outgoing DATA without pending ACK is immediate");
        }
        using(var h = Handler())
        {
            Check(!h.CreateRepeatedAckPayload().HasValue,
                "piggyback fixture cannot invent an acknowledgement before receiving DATA");
            var incoming = new Rdpeudp2Packet();
            incoming.Header.Flags = Rdpeudp2PacketHeaderFlags.DATA;
            incoming.DataHeader = new DataHeaderPayload { DataSeqNum = 7 };
            incoming.DataBody = new DataBodyPayload { ChannelSeqNum = 7, Data = new byte[]{1} };
            h.ReceivePacket(incoming);
            var outgoing = h.CreateDataPacket(new byte[]{2});
            Check(outgoing.ACK.HasValue && outgoing.ACK.Value.SeqNum == 7,
                "pending incoming DATA is piggyback acknowledged");
            var repeated = h.CreateRepeatedAckPayload();
            Check(repeated.HasValue && repeated.Value.SeqNum == 7,
                "piggyback fixture repeats only observed client DATA acknowledgement");
            Check(!h.CreateDataPacket(new byte[]{3}).ACK.HasValue,
                "consumed pending DATA is not acknowledged again");
        }
        using(var h = Handler())
        {
            var notAck = new Rdpeudp2Packet();
            notAck.Header.Flags = Rdpeudp2PacketHeaderFlags.DATA;
            h.ReceivePacket(notAck);
            Check(h.ExpectAckPacket(TimeSpan.FromMilliseconds(30),7) == null,
                "non-ACK packet cannot satisfy acknowledgement expectation");
        }
        using(var s = new RdpeudpServer(new IPEndPoint(IPAddress.Loopback,0),false))
        {
            s.Start();
            var timer=Stopwatch.StartNew();
            s.Stop();
            Check(timer.Elapsed < TimeSpan.FromSeconds(3), "UDP listener shutdown finishes");
        }
        Console.WriteLine($"{count} checks passed");
    }
}

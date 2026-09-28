# Capture comparison: udp-tmp2 — 2026-09-06

Inputs: `/tmp/udp-tmp2.pcap` and
`/Users/david/Downloads/rdp-udp-win-win.pcapng`.
Current trace: 70 packets over 6.149117 seconds. Windows reference contains
raw IPv4 packets despite its link-layer metadata; extracted EPB packet data,
removed consecutive identical packets, and decoded 2978 packets as LINKTYPE_RAW.
Reference frame numbers below refer to that normalized copy
(`/tmp/udp-review-win-reference.pcap`), not original pcapng frame numbers.

## Current trace

- Frame 3, +0.004219: client DATA, DataSeq=100, ChannelSeq=1.
- Frame 4, +0.013296: server dummy packet, flags=0x0155,
  containing ACK(100), AOA=100, DataSeq=100. This acknowledges the first chunk.
- Frames 4–49: server dummy burst, DataSeq=100–145.
- Frame 50, +0.220125: client retransmits ChannelSeq=1 with DataSeq=101.
- Frame 51, +0.253055: standalone server ACK(101).
- Frame 52, +0.253304: client sends ChannelSeq=2, DataSeq=102, completing
  the ClientHello record. Frame 53 acknowledges it.
- Frame 54, +0.515786: server sends ServerHello/CCS/encrypted TLS records,
  ChannelSeq=1, DataSeq=146, AOA=146.
- Server repeatedly retransmits ChannelSeq=1, with DataSeq advancing through
  160 (interleaved dummy packets). There are no client packets after frame 52.

## Concrete discrepancy

Current `rdpeudp_recv_one` returns immediately for dummy packets before processing
ACK, AOA, or DataSeq. This discards the ACK(100) actually present in frame 4 and
is consistent with the unnecessary retry in frame 50. It also ignores the dummy
burst's receive sequence state. Current `rdpeudp2_recv_reliable` only schedules
ACKs when higher-layer bytes are buffered, which will also need attention when
dummy processing is corrected: dummy payload must not enter the TLS stream.

The working Windows client acknowledges dummy packets: reference frame 5 carries
ACK(100) in a dummy; client frames 23 onward acknowledge server DataSeq values
100, 104, 105, 109, etc. These are transport dummy packets in the same UDP flow,
not evidence of a separate lossy/DTLS channel.

Microsoft specifies that dummy packets participate in normal UDP transport
processing; their contents are ignored by higher layers and their loss must not
cause retransmission. See:
https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeudp2/9a92f0eb-7353-4c6a-859e-eab072c36c30

## Next verification

Process control fields and receive DataSeq for dummy packets, generate ACKs
independently of TLS delivery, and omit dummy data from ChannelSeq/TLS delivery.
Verify AOA handling as well; the current receive path does not apply parsed AOA
to its receive window. Add regression coverage using the frame-4 combination
(dummy + ACK + DATA + AOA), then dummy-only traffic followed by real ChannelSeq=1.

This trace proves that Windows received the complete ClientHello and responded.
It does not prove why the client fails to acknowledge the later real ServerHello;
that still needs the matching client log/receive-path tracing. Do not claim that
fixing dummy handling alone necessarily completes TLS or UDP interoperability.

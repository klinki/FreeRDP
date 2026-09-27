# udp-bioevent capture — 2026-09-06

Input: `/tmp/udp-bioevent.pcap`, 149 packets, first-to-last interval 30.262848 s.
Inspected with tshark, including local TLS keylog-assisted decoding. Secret
material is not copied into this report. `/tmp/rdp-udp-test.log` timestamps and
sequence diagnostics align with this trace.

## Milestones

- Frame 54 (+0.522813): server TLS flight, DataSeq=146, ChannelSeq=1.
- Frames 55 onward: client now ACKs 146, unlike udp-tmp2.
- Frame 62 (+0.535837): client TLS Finished; frame 63: server session ticket.
  The matching client log reports TMP2-TLS status=1.
- Frame 110 (+0.550799): encrypted client Tunnel Create Request.
- Frame 112 (+0.562255): decrypted response `01 04 00 04 00 00 00 00`:
  Create Response, four-byte payload, status zero. Tunnel creation succeeds.
- Frames 128–129: server sends additional tunnel traffic. Decrypted frame 129
  includes Microsoft::Windows::RDS::CoreInput, Graphics, and MouseCursor strings.
  No successful DVC migration or useful graphics delivery is established.

## Remaining discrepancies

The dummy-packet defect remains visible. Frame 4 contains ACK(100) in a dummy
packet, but frame 50 retransmits the same client channel chunk. Frames 64–109
are 46 server dummy packets with DataSeq 148–193. Client ACKVEC frame 113 starts
at 148 and reports precisely those 46 values missing, followed by receipt of
194/195. Later keepalive ACKs remain at 147 (0x0093). The log independently
shows recvDataBase stuck at 148. Handle dummy transport fields/DataSeq and AOA;
do not deliver dummy bytes to TLS. ACK scheduling must not depend solely on
new higher-layer payload, including for duplicate ChannelSeq retransmissions.

The client also rejects post-creation traffic. Its matching log reports
`bad UDP tunnel subheader, ignoring rest` and `bad UDP channel packet, ignoring`.
The decrypted frame-128 payload starts `02 00 00 16 12 ...`; frame-129 payloads
start `02 25 00 04 18 02 ...`, `02 24 00 04 18 07 ...`, and
`02 27 00 04 18 08 ...`. These give concrete input fixtures for reviewing tunnel
subheader and higher-layer dispatch assumptions. They do not by themselves prove
which parser field or routing decision is incorrect. tshark's generic TPKT
“Continuation” label is not a validation of the RDP multitransport payload.

Prioritize transport dummy/AOA/ACK-state correction and then reproduce the
post-creation parsing errors with decrypted fixtures. Preserve the newly working
TLS/BIO path. This trace establishes TLS and tunnel creation, not full UDP channel
operation. Production code and other active review documents were not modified.

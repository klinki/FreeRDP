# VM reconnection test — 2026-09-08

Rebuilt project SDL client at HEAD 764326c61 and connected to UTM VM
192.168.64.2 via the bounded localhost TCP/UDP relay in reconnect-relay.py.
Enabled auto-reconnect with 10 retries. This test actively closes the TCP
connection and drops UDP for 15 seconds; it does not simulate a silent TCP
blackhole or UDP-only graphics stall.

- Initial ACTIVE: 07:50:19.605.
- An unplanned transport failure at 07:50:29.237 already triggered successful
  automatic reconnect (ACTIVE 07:50:29.397), before relay fault injection.
- Injected break: about 07:50:39.6; immediate reconnect attempt while offline.
- Restored relay 15 seconds later, about 07:50:54.6.
- Next TCP attempt at 07:50:57.147; ACTIVE again at 07:50:58.121.
- Client remained alive through the observation interval, then the harness
  terminated only its own client and closed its relay sockets.

Outcome: automatic reconnection to an active RDP session is demonstrated,
roughly 3.5 seconds after restoration. This is not a UDP health pass: reliable
UDP send timeouts began before injection and continued afterward. The relay
and the application's UDP reconnect handling need separate analysis before
attributing those timeouts. No full desktop interaction test was performed.

Raw logs remain at /tmp/rdp-vm-reconnect-client.log and
/tmp/rdp-vm-reconnect-relay.log. Credentials are supplied through the environment,
not stored in the harness. The debug log may contain session tokens; it is not
copied into the repository.

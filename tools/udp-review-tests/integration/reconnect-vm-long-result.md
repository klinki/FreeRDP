# Five-retry exhaustion test — 2026-09-08

Used the project client and VM 192.168.64.2 through reconnect-relay.py,
with RDP_TEST_OUTAGE=180 and RDP_TEST_RETRIES=5. TCP is actively broken;
offline attempts are accepted and immediately closed, and UDP is dropped.
This is not a silent SYN/packet blackhole test.

Initial session reached ACTIVE. Outage began at 08:30:00.930. Reconnect
attempts (0 through 4) started at:

- 08:30:00.930
- 08:30:20.635
- 08:30:40.293
- 08:30:59.958
- 08:31:19.646

At 08:31:39.356 the client logged AutoReconnect retries exceeded:
98.426 seconds after the break. The process exited without harness
termination (exit status 131). The relay counted six TCP connections total:
one initial connection and exactly five reconnect attempts.

Relay restored after 180 seconds. No further reconnect occurred during
the 40-second post-restoration observation. The harness then closed itself.

The code defaults TcpConnectTimeout to 15000 ms. SDL's retry dialog uses
that setting as the post-failure delay. Actual retry spacing here was about
19.7 seconds; scheduling and connection/cleanup work add wall-clock time.
An attempt that itself times out can add further delay, so 5*30 seconds is
not a fixed universal limit. The retry count, not a fixed total duration,
is what this test confirms.

Logs: /tmp/rdp-vm-reconnect-long-client.log and
/tmp/rdp-vm-reconnect-long-relay.log. Raw debug session tokens were not copied
into this repository. No implementation files were changed.

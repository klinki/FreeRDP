# Private macOS launcher bridge

Configure with `-DWITH_LAUNCHER_BRIDGE=ON` (the default) to compile the bridge
for macOS when WinPR JSON support is available. Set `-DWITH_LAUNCHER_BRIDGE=OFF`
to omit the bridge and its transport tests. It is inactive during normal CLI usage. A native launcher opts in by inheriting a
connected Unix stream socket and passing `/launcher-fd:3` and
`/launcher-session:<session-id>`. The descriptor must be at least 3; it is marked
close-on-exec after startup. Diagnostics use the existing logging channels,
never the protocol socket.

Messages are UTF-8, newline-delimited JSON with `v:1`, `type`, and `sessionId`.
Frames are limited to 1 MiB. After SDL initialization on its main thread the
client sends `hello` with engine version, capabilities, and display inventory.
The launcher responds with `start` containing `arguments` and `displaySelections`,
or `cancel` for discovery. Selected display UUIDs are mapped to current SDL IDs
by unambiguous CoreGraphics bounds matching. Missing, duplicate, or ambiguous
selections fail before connecting.

The client emits `auth_request`, `certificate_request`, `state`, `retry`, and
one `ended` event. Prompts include string `requestId` values. The launcher replies
with `auth_response` or `certificate_response`, and may send `cancel` or `focus`.
Stale session/request identifiers are ignored. Credentials travel only in an
accepted `auth_response`; managed arguments reject password, certificate bypass,
console callback, and other authentication overrides. EOF and cancellation wake
all pending requests and abort the RDP session.

Protocol reading and writing run on worker threads. Focus and display APIs run
on the SDL main thread. Authentication/certificate waits run on the RDP worker.
Terminal events are produced after the completed session thread is joined;
ordinary `PostDisconnect` callbacks during automatic reconnect are not terminal.
The existing renderer, input handling, and reconnect overlay remain in use.

Configure with `-DWITH_LAUNCHER_BRIDGE=ON -DSDL_LAUNCHER_TEST=ON`, build `TestSDLLauncherTransport`, and run
`ctest --test-dir <build>/client/SDL/SDL3 -R TestSDLLauncherTransport` for socketpair
regressions covering prompts, fragmented/oversized frames, stale commands,
cancellation, EOF, versions, and terminal distinctions. The tests do not open a
remote connection or use real credentials.

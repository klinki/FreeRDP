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

A client advertising `close_confirmation` accepts `start.confirmSessionClose`.
When enabled, SDL window Close, fullscreen Close, and Quit queue a `close_request`
with a unique `requestId` without waiting on the SDL thread. Repeated Close events
are coalesced until a matching `close_response` with boolean `accepted` arrives.
Rejecting resumes the session; accepting intentionally cancels it. Stale responses
are ignored. Parent EOF and `cancel` always terminate without a close decision.
The default is immediate close, preserving normal CLI behavior.

A client advertising `session_thumbnail` accepts `start.thumbnailsEnabled` and
`thumbnail_request` with a string `requestId`. While enabled and connected,
completed GDI BGRA frames are downsampled to an owned snapshot at most once per
second. PNG encoding and protocol delivery run on a separate worker. A correlated
`thumbnail` event contains `pngBase64`, `width`, and `height`; the image preserves
aspect ratio, is at most 320×200, and is limited to 262144 PNG bytes and 350000
base64 characters. Requests are limited to one every two seconds and one pending
request is retained. A `thumbnail_request` with `thumbnailsEnabled:false` and no
request ID disables capture, clears cached pixels and pending/queued images, and
invalidates an encoding in progress. Cancel/EOF also clear capture state. Images
remain in process memory, are never logged or saved, and do not use screen capture
or require screen recording permission.

Protocol reading and writing run on worker threads. Focus and display APIs run
on the SDL main thread. Authentication/certificate waits run on the RDP worker.
Terminal events are produced after the completed session thread is joined;
ordinary `PostDisconnect` callbacks during automatic reconnect are not terminal.
The existing renderer, input handling, and reconnect overlay remain in use.

Configure with `-DWITH_LAUNCHER_BRIDGE=ON -DSDL_LAUNCHER_TEST=ON`, build `TestSDLLauncherTransport`, and run
`ctest --test-dir <build>/client/SDL/SDL3 -R TestSDLLauncherTransport` for socketpair
regressions covering prompts, fragmented/oversized frames, stale commands,
cancellation, EOF, versions, terminal distinctions, correlated Close decisions,
bounded thumbnail PNGs, disabled capture, and worker teardown under backpressure. The tests do not open a
remote connection or use real credentials.

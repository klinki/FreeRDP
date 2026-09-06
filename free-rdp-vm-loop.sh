#!/usr/bin/env bash
#
# VM live-test loop for RDP-UDP multitransport.
# Copy of free-rdp-02-better-codec-better-text.sh adapted to the local UTM VM
# (192.168.64.2, local account David). The test-VM password comes from the
# environment (RDP_PASS) and is never stored in this file: export it in the
# calling shell before running.
#
# Usage: export RDP_PASS='<vm-password>'
#        ./free-rdp-vm-loop.sh [iterations]   (default 1, ~40s per round)
#
# Usage: ./free-rdp-vm-loop.sh [iterations]   (default 1, ~40s per round)
# Each round writes /tmp/udp-vm-<n>.pcap + /tmp/rdp-vm-<n>.log and prints a
# verdict. The agent (not this script) diagnoses and fixes between rounds.

set -euo pipefail

FREERDP_BIN="${FREERDP_BIN:-/tmp/freerdp-build/client/SDL/SDL3/sdl-freerdp}"
SERVER="${SERVER:-192.168.64.2}"
RDP_USER="${RDP_USER:-David}"
RDP_PASS="${RDP_PASS:?set RDP_PASS to the test-VM password before running}"
IFACE="${IFACE:-bridge100}"
DUR="${DUR:-30}"
ITERS="${1:-1}"

for ((n = 1; n <= ITERS; n++)); do
	PCAP="/tmp/udp-vm-${n}.pcap"
	LOG="/tmp/rdp-vm-${n}.log"
	SECRETS="/tmp/rdp-vm-secrets-${n}.txt"
	echo "=== round $n/$ITERS: ${DUR}s capture -> $PCAP ==="
	rm -f "${SECRETS}"
	dumpcap -i "${IFACE}" -w "${PCAP}" -f "udp port 3389" -a "duration:${DUR}" \
		>/dev/null 2>&1 &
	dcap=$!
	sleep 2 # let capture settle
	# NOTE: /p: instead of piped /from-stdin:force — the passphrase reader
	# needs a tty for termios and fails headless (prints usage, no connect).
	# Test-VM-only password from $RDP_PASS (never committed with a value).
	"${FREERDP_BIN}" \
		/v:"${SERVER}" \
		/u:"${RDP_USER}" \
		/p:"${RDP_PASS}" \
		/cert:tofu \
		+multitransport \
		/tls:secrets-file:"${SECRETS}" \
		/log-filters:com.freerdp.core.rdpeudp:DEBUG,com.freerdp.core.multitransport:DEBUG \
		>"${LOG}" 2>&1 &
	client=$!
	sleep "${DUR}" || true
	kill "${client}" 2>/dev/null || true
	wait "${client}" 2>/dev/null || true
	wait "${dcap}" 2>/dev/null || true

	echo "--- round $n analysis ---"
	counts=$(tcpdump -n -r "${PCAP}" 2>/dev/null | awk '$2=="IP"{
		split($3,a,"."); split($5,b,"."); dp=b[5]; sub(/:/,"",dp);
		if (dp==3389) c2s++; else s2c++; n++} END{print n+0, c2s+0, s2c+0}')
	# shellcheck disable=SC2086
	set -- $counts
	echo "packets=$1 c2s=$2 s2c=$3 secrets=${SECRETS}"
	if [ "$1" -eq 0 ]; then
		echo "VERDICT[$n]=NO_TRAFFIC rig failure (client never sent; see log)"
	elif grep -q "send timeout" "${LOG}" 2>/dev/null; then
		echo "VERDICT[$n]=STALLED reliable-send timeout:"
		grep "send timeout" "${LOG}" | head -n 3
	elif grep -q "bad UDP" "${LOG}" 2>/dev/null; then
		echo "VERDICT[$n]=TUNNEL_PARSE_FAIL:"
		grep "bad UDP\|UDP-TUNNEL" "${LOG}" | head -n 6
	elif [ "$3" -le 1 ]; then
		echo "VERDICT[$n]=HANDSHAKE_ONLY server silent after SYN+ACK"
	elif grep -q "TLS over RDP-UDP established\|RDP-UDP connected" "${LOG}" 2>/dev/null; then
		echo "VERDICT[$n]=PROGRESS tunnel/log milestones reached (inspect pcap)"
	else
		echo "VERDICT[$n]=TWO_WAY_UDP no timeout/WARNs (inspect pcap for migration)"
	fi
done

#!/usr/bin/env bash
#
# Live UDP test client: same fine-tuned 4K/AVC444 config as free-rdp-02,
# but running OUR build (feat/add-udp) with multitransport + TLS secrets
# for Wireshark dissection. Logs go to /tmp/rdp-udp-test.log.
#
# Env overrides: SERVER (default davidpc), RDP_USER (default david),
# FREERDP_BIN (default: our /tmp build).

set -euo pipefail

FREERDP_BIN="${FREERDP_BIN:-/tmp/freerdp-build/client/SDL/SDL3/sdl-freerdp}"
SERVER="${SERVER:-davidpc}"
RDP_USER="${RDP_USER:-david}"
SECRETS_FILE="${SECRETS_FILE:-/tmp/rdp-secrets.txt}"
LOG_FILE="${LOG_FILE:-/tmp/rdp-udp-test.log}"

# SDL monitor IDs from `sdl-freerdp /list:monitor`.
# The first selected monitor becomes the RDP primary when the MacBook display
# (ID 1) is excluded.
PRIMARY_MONITOR_ID="${PRIMARY_MONITOR_ID:-3}"       # M27UP
SECONDARY_MONITOR_ID="${SECONDARY_MONITOR_ID:-2}"   # DELL U2419HC

# Fresh secrets each run so Wireshark never uses a stale session.
rm -f "${SECRETS_FILE}"

# Use M27UP as the RDP primary and DELL U2419HC as the secondary.
"${FREERDP_BIN}" \
  /v:"${SERVER}" \
  /port:3389 \
  /u:"${RDP_USER}" \
  /d:davidpc \
  /gfx:AVC444:on \
  /network:lan \
  /cert:tofu \
  /clipboard \
  /from-stdin:force \
  /multimon \
  +f \
  /monitors:"${PRIMARY_MONITOR_ID},${SECONDARY_MONITOR_ID}" \
  /sdl-monitor-scale:"${PRIMARY_MONITOR_ID}=175/100,${SECONDARY_MONITOR_ID}=100/100" \
  +multitransport \
  +async-update \
  /auto-reconnect \
  /auto-reconnect-max-retries:5 \
  /tls:secrets-file:"${SECRETS_FILE}" \
  /log-filters:com.freerdp.core.rdpeudp:DEBUG,com.freerdp.core.multitransport:DEBUG,com.freerdp.core.autodetect:DEBUG,com.freerdp.core:DEBUG \
  2>&1 | tee "${LOG_FILE}"

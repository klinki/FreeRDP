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

# Fresh secrets each run so Wireshark never uses a stale session.
rm -f "${SECRETS_FILE}"

# SDL monitor 2 is the external 4K display (PHL 288P6L).
"${FREERDP_BIN}" \
  /v:"${SERVER}" \
  /port:3389 \
  /u:"${RDP_USER}" \
  /d:davidpc \
  /size:3840x2160 \
  /scale-desktop:175 \
  /scale-device:100 \
  /gfx:AVC444:on \
  /network:lan \
  /cert:tofu \
  /clipboard \
  /from-stdin:force \
  +f \
  /monitors:1 \
  +multitransport \
  +async-update \
  /auto-reconnect \
  /auto-reconnect-max-retries:5 \
  /tls:secrets-file:"${SECRETS_FILE}" \
  /log-filters:com.freerdp.core.rdpeudp:DEBUG,com.freerdp.core.multitransport:DEBUG,com.freerdp.core.autodetect:DEBUG,com.freerdp.core:DEBUG \
  2>&1 | tee "${LOG_FILE}"

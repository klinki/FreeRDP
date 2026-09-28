/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * User Datagram Protocol (UDP) socket layer
 *
 * Copyright 2026 FreeRDP Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * UDP support for multitransport / RDP-UDP ([MS-RDPEUDP], [MS-RDPEUDP2]).
 * See https://www.hardening-consulting.com/en/posts/20210131-udp-support-1.html
 * and https://www.hardening-consulting.com/en/posts/20230109-udp-support-2.html
 */

#ifndef FREERDP_LIB_CORE_UDP_H
#define FREERDP_LIB_CORE_UDP_H

#include <winpr/windows.h>

#include <freerdp/types.h>
#include <freerdp/settings.h>
#include <freerdp/freerdp.h>
#include <freerdp/api.h>
#include <freerdp/transport_io.h>

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/stream.h>
#include <winpr/winsock.h>

#include <openssl/bio.h>

/** Maximum UDP payload we accept.
 * [MS-RDPEUDP2] mandates 1232 as max negotiated MTU, but implementations
 * have been observed sending up to 1239 bytes of RDPUDP payload (see
 * hardening-consulting blog part 2). Use a generous buffer so the TLS
 * handshake is not truncated. */
#define FREERDP_UDP_MAX_DATAGRAM 65535
#define FREERDP_UDP_DEFAULT_MTU 1232
#define FREERDP_UDP_MIN_MTU 1132

WINPR_ATTR_NODISCARD
FREERDP_LOCAL int freerdp_udp_connect(rdpContext* context, const char* hostname, int port,
                                      DWORD timeout);

FREERDP_LOCAL BOOL freerdp_udp_close(int sockfd);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL SSIZE_T freerdp_udp_send(int sockfd, const BYTE* data, size_t len);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL SSIZE_T freerdp_udp_recv(int sockfd, BYTE* buffer, size_t len, DWORD timeoutMs);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL freerdp_udp_wait_readable(int sockfd, DWORD timeoutMs);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL freerdp_udp_wait_writable(int sockfd, DWORD timeoutMs);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL freerdp_udp_set_nonblocking(int sockfd, BOOL nonblocking);

WINPR_ATTR_MALLOC(free, 1)
WINPR_ATTR_NODISCARD
FREERDP_LOCAL char* freerdp_udp_get_peer_address(int sockfd);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL struct addrinfo* freerdp_udp_resolve_host(const char* hostname, int port,
                                                        int ai_flags);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL rdpTransportLayer* freerdp_udp_connect_layer(rdpContext* context,
                                                           const char* hostname, int port,
                                                           DWORD timeout);

#endif /* FREERDP_LIB_CORE_UDP_H */

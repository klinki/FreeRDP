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
 */

#include <freerdp/config.h>

#include "settings.h"

#include <time.h>
#include <errno.h>
#include <fcntl.h>

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/platform.h>
#include <winpr/winsock.h>
#include <winpr/thread.h>

#include "rdp.h"
#include "udp.h"
#include "utils.h"

#if !defined(_WIN32)
#include <netdb.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef WINPR_HAVE_POLL_H
#include <poll.h>
#else
#include <sys/select.h>
#endif
#else
#include <winpr/windows.h>
#include <winpr/crt.h>
#define close(_fd) closesocket(_fd)
#endif

#include <freerdp/log.h>
#define TAG FREERDP_TAG("core.udp")

struct addrinfo* freerdp_udp_resolve_host(const char* hostname, int port, int ai_flags)
{
	char port_str[16] = { 0 };
	char* service = nullptr;
	struct addrinfo hints = { 0 };
	struct addrinfo* result = nullptr;

	WINPR_ASSERT(hostname);

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = ai_flags;

	if (port >= 0)
	{
		(void)_snprintf(port_str, sizeof(port_str) - 1, "%d", port);
		service = port_str;
	}

	const int status = getaddrinfo(hostname, service, &hints, &result);
	if (status != 0)
	{
		WLog_WARN(TAG, "getaddrinfo(%s) failed with %s", hostname, gai_strerror(status));
		return nullptr;
	}

	return result;
}

BOOL freerdp_udp_set_nonblocking(int sockfd, BOOL nonblocking)
{
	if (sockfd < 0)
		return FALSE;
#if !defined(_WIN32)
	int flags = fcntl(sockfd, F_GETFL, 0);
	if (flags == -1)
		return FALSE;
	if (nonblocking)
		flags |= O_NONBLOCK;
	else
		flags &= ~(O_NONBLOCK);
	return fcntl(sockfd, F_SETFL, flags) == 0;
#else
	u_long arg = nonblocking ? 1UL : 0UL;
	return _ioctlsocket((SOCKET)sockfd, FIONBIO, &arg) == 0;
#endif
}

BOOL freerdp_udp_wait_readable(int sockfd, DWORD timeoutMs)
{
	if (sockfd < 0)
		return FALSE;
#ifdef WINPR_HAVE_POLL_H
	struct pollfd pfd = { 0 };
	pfd.fd = sockfd;
	pfd.events = POLLIN;
	int status = -1;
	do
	{
		status = poll(&pfd, 1, (int)timeoutMs);
	} while ((status < 0) && (errno == EINTR));
	return status > 0;
#else
	fd_set rset = { 0 };
	struct timeval tv = { 0 };
	FD_ZERO(&rset);
	FD_SET(sockfd, &rset);
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	int status = -1;
	do
	{
		status = select(sockfd + 1, &rset, nullptr, nullptr,
		                (timeoutMs == INFINITE) ? nullptr : &tv);
	} while ((status < 0) && (errno == EINTR));
	return status > 0;
#endif
}

BOOL freerdp_udp_wait_writable(int sockfd, DWORD timeoutMs)
{
	if (sockfd < 0)
		return FALSE;
#ifdef WINPR_HAVE_POLL_H
	struct pollfd pfd = { 0 };
	pfd.fd = sockfd;
	pfd.events = POLLOUT;
	int status = -1;
	do
	{
		status = poll(&pfd, 1, (int)timeoutMs);
	} while ((status < 0) && (errno == EINTR));
	return status > 0;
#else
	fd_set wset = { 0 };
	struct timeval tv = { 0 };
	FD_ZERO(&wset);
	FD_SET(sockfd, &wset);
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	int status = -1;
	do
	{
		status = select(sockfd + 1, nullptr, &wset, nullptr,
		                (timeoutMs == INFINITE) ? nullptr : &tv);
	} while ((status < 0) && (errno == EINTR));
	return status > 0;
#endif
}

int freerdp_udp_connect(rdpContext* context, const char* hostname, int port, DWORD timeout)
{
	int sockfd = -1;
	struct addrinfo* result = nullptr;
	struct addrinfo* addr = nullptr;

	WINPR_ASSERT(context);
	WINPR_ASSERT(hostname);

	if (port <= 0)
		port = 3389;

	result = freerdp_udp_resolve_host(hostname, port, 0);
	if (!result)
	{
		freerdp_set_last_error_if_not(context, FREERDP_ERROR_DNS_NAME_NOT_FOUND);
		return -1;
	}
	freerdp_set_last_error_log(context, 0);

	for (addr = result; addr != nullptr; addr = addr->ai_next)
	{
		char* peerAddress = freerdp_tcp_address_to_string(
		    (const struct sockaddr_storage*)addr->ai_addr, nullptr);
		if (peerAddress)
		{
			WLog_DBG(TAG, "trying UDP %s:%d (%s)", hostname, port, peerAddress);
			free(peerAddress);
		}

		sockfd = socket(addr->ai_family, SOCK_DGRAM, IPPROTO_UDP);
		if (sockfd < 0)
			continue;

		/* connect() on a datagram socket only stores the default peer and
		 * filters incoming datagrams. No handshake is performed. */
		if (_connect((SOCKET)sockfd, addr->ai_addr, (int)addr->ai_addrlen) == 0)
			break;

		const int err = WSAGetLastError();
		WLog_WARN(TAG, "UDP connect(%s:%d) failed: %d", hostname, port, err);
		close(sockfd);
		sockfd = -1;
	}

	freeaddrinfo(result);

	if (sockfd < 0)
	{
		freerdp_set_last_error_if_not(context, FREERDP_ERROR_CONNECT_FAILED);
		return -1;
	}

	/* enlarge buffers, RDP-UDP is very chatty and bursts easily overflow
	 * the default socket buffers (see hardening blog part 2, OOM killer). */
	{
		int rcvbuf = 2 * 1024 * 1024;
		(void)setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (const void*)&rcvbuf,
		                 sizeof(rcvbuf));
		int sndbuf = 1 * 1024 * 1024;
		(void)setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const void*)&sndbuf,
		                 sizeof(sndbuf));
	}

	if (!freerdp_udp_set_nonblocking(sockfd, TRUE))
		WLog_WARN(TAG, "could not set UDP socket non-blocking");

	if (WaitForSingleObject(utils_get_abort_event(context->rdp), 0) == WAIT_OBJECT_0)
	{
		close(sockfd);
		return -1;
	}

	WINPR_UNUSED(timeout);
	return sockfd;
}

BOOL freerdp_udp_close(int sockfd)
{
	if (sockfd < 0)
		return FALSE;
#if !defined(_WIN32)
	(void)shutdown(sockfd, SHUT_RDWR);
#endif
	close(sockfd);
	return TRUE;
}

SSIZE_T freerdp_udp_send(int sockfd, const BYTE* data, size_t len)
{
	if ((sockfd < 0) || !data || (len == 0) || (len > INT_MAX))
		return -1;

	const int status = _send((SOCKET)sockfd, (const char*)data, (int)len, 0);
	if (status < 0)
	{
		const int err = WSAGetLastError();
		if ((err == WSAEWOULDBLOCK) || (err == WSAEINTR) || (err == WSAEINPROGRESS) ||
		    (err == WSAEALREADY))
			errno = EAGAIN;
		return -1;
	}
	return status;
}

SSIZE_T freerdp_udp_recv(int sockfd, BYTE* buffer, size_t len, DWORD timeoutMs)
{
	if ((sockfd < 0) || !buffer || (len == 0))
		return -1;

	if (!freerdp_udp_wait_readable(sockfd, timeoutMs))
	{
		errno = EAGAIN;
		return 0;
	}

	const size_t capped = (len > (size_t)INT_MAX) ? (size_t)INT_MAX : len;
	const int status = _recv((SOCKET)sockfd, (char*)buffer, (int)capped, 0);
	if (status < 0)
	{
		const int err = WSAGetLastError();
		if ((err == WSAEWOULDBLOCK) || (err == WSAEINTR) || (err == WSAEINPROGRESS) ||
		    (err == WSAEALREADY))
			errno = EAGAIN;
		return -1;
	}
	return status;
}

char* freerdp_udp_get_peer_address(int sockfd)
{
	struct sockaddr_storage saddr = { 0 };
	socklen_t length = sizeof(saddr);

	if (sockfd < 0)
		return nullptr;
	if (getpeername(sockfd, (struct sockaddr*)&saddr, &length) != 0)
		return nullptr;
	return freerdp_tcp_address_to_string(&saddr, nullptr);
}

struct rdp_udp_layer
{
	int sockfd;
	HANDLE hEvent;
};
typedef struct rdp_udp_layer rdpUdpLayer;

static int freerdp_udp_layer_read(void* userContext, void* data, int bytes)
{
	rdpUdpLayer* udpLayer = (rdpUdpLayer*)userContext;
	if (!udpLayer || !data || (bytes <= 0))
		return -1;
	(void)WSAResetEvent(udpLayer->hEvent);
	const int status = _recv((SOCKET)udpLayer->sockfd, data, bytes, 0);
	if (status >= 0)
		return status;
	const int error = WSAGetLastError();
	if ((error == WSAEWOULDBLOCK) || (error == WSAEINTR) || (error == WSAEINPROGRESS) ||
	    (error == WSAEALREADY))
		errno = EAGAIN;
	return -1;
}

static int freerdp_udp_layer_write(void* userContext, const void* data, int bytes)
{
	rdpUdpLayer* udpLayer = (rdpUdpLayer*)userContext;
	if (!udpLayer || !data || (bytes <= 0))
		return -1;
	const int status = _send((SOCKET)udpLayer->sockfd, data, bytes, 0);
	if (status >= 0)
		return status;
	const int error = WSAGetLastError();
	if ((error == WSAEWOULDBLOCK) || (error == WSAEINTR) || (error == WSAEINPROGRESS) ||
	    (error == WSAEALREADY))
		errno = EAGAIN;
	return -1;
}

static BOOL freerdp_udp_layer_close(void* userContext)
{
	rdpUdpLayer* udpLayer = (rdpUdpLayer*)userContext;
	if (!udpLayer)
		return FALSE;
	if (udpLayer->sockfd >= 0)
	{
		closesocket((SOCKET)udpLayer->sockfd);
		udpLayer->sockfd = -1;
	}
	if (udpLayer->hEvent)
	{
		(void)CloseHandle(udpLayer->hEvent);
		udpLayer->hEvent = nullptr;
	}
	return TRUE;
}

static BOOL freerdp_udp_layer_wait(void* userContext, BOOL waitWrite, DWORD timeout)
{
	rdpUdpLayer* udpLayer = (rdpUdpLayer*)userContext;
	if (!udpLayer)
		return FALSE;
	if (waitWrite)
		return freerdp_udp_wait_writable(udpLayer->sockfd, timeout);
	return freerdp_udp_wait_readable(udpLayer->sockfd, timeout);
}

static HANDLE freerdp_udp_layer_get_event(void* userContext)
{
	rdpUdpLayer* udpLayer = (rdpUdpLayer*)userContext;
	if (!udpLayer)
		return nullptr;
	return udpLayer->hEvent;
}

rdpTransportLayer* freerdp_udp_connect_layer(rdpContext* context, const char* hostname, int port,
                                             DWORD timeout)
{
	WINPR_ASSERT(context);

	int sockfd = freerdp_udp_connect(context, hostname, port, timeout);
	if (sockfd < 0)
		return nullptr;

	rdpTransportLayer* layer =
	    transport_layer_new(freerdp_get_transport(context), sizeof(rdpUdpLayer));
	if (!layer)
	{
		freerdp_udp_close(sockfd);
		return nullptr;
	}

	layer->Read = freerdp_udp_layer_read;
	layer->Write = freerdp_udp_layer_write;
	layer->Close = freerdp_udp_layer_close;
	layer->Wait = freerdp_udp_layer_wait;
	layer->GetEvent = freerdp_udp_layer_get_event;

	rdpUdpLayer* udpLayer = (rdpUdpLayer*)layer->userContext;
	WINPR_ASSERT(udpLayer);
	udpLayer->sockfd = -1;
	udpLayer->hEvent = WSACreateEvent();
	if (!udpLayer->hEvent)
	{
		transport_layer_free(layer);
		freerdp_udp_close(sockfd);
		return nullptr;
	}

	if (WSAEventSelect((SOCKET)sockfd, udpLayer->hEvent, FD_READ | FD_WRITE | FD_CLOSE) != 0)
		WLog_WARN(TAG, "WSAEventSelect(UDP) failed 0x%08x", (unsigned)WSAGetLastError());

	udpLayer->sockfd = sockfd;
	return layer;
}

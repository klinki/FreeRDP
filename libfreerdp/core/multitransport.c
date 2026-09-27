/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MULTITRANSPORT PDUs
 *
 * Copyright 2014 Dell Software <Mike.McDonald@software.dell.com>
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

#include <winpr/assert.h>
#include <freerdp/config.h>
#include <freerdp/log.h>

#include <winpr/thread.h>
#include <winpr/synch.h>

#include "settings.h"
#include "rdp.h"
#include "multitransport.h"
#include "rdpeudp.h"
#include "udp.h"
#include "autodetect.h"

struct rdp_multitransport
{
	rdpRdp* rdp;

	MultiTransportRequestCb MtRequest;
	MultiTransportResponseCb MtResponse;

	/* server-side data */
	UINT32 reliableReqId;

	BYTE reliableCookie[RDPUDP_COOKIE_LEN];
	BYTE reliableCookieHash[RDPUDP_COOKIE_HASHLEN];

	/* client/server UDP transport (established async) */
	rdpUdpTransport* udp;
	CRITICAL_SECTION lock;
	HANDLE udpThread;
	BOOL closing;
	BOOL threadRunning;
};

#define TAG FREERDP_TAG("core.multitransport")

static state_run_t multitransport_client_request_udp(rdpMultitransport* multi, UINT32 reqId,
                                                     UINT16 reqProto, const BYTE* cookie);
static DWORD WINAPI multitransport_udp_connect_thread(LPVOID arg);
static DWORD WINAPI multitransport_udp_accept_thread(LPVOID arg);

typedef struct
{
	rdpMultitransport* multi;
	UINT32 reqId;
	UINT16 reqProto;
	BYTE cookie[RDPUDP_COOKIE_LEN];
	char hostname[256];
	int port;
} UdpConnectArgs;

typedef struct
{
	rdpMultitransport* multi;
	int port;
	UINT32 reqId;
	BYTE cookie[RDPUDP_COOKIE_LEN];
} UdpAcceptArgs;

state_run_t multitransport_recv_request(rdpMultitransport* multi, wStream* s)
{
	WINPR_ASSERT(multi);
	rdpSettings* settings = multi->rdp->settings;

	if (freerdp_settings_get_bool(settings, FreeRDP_ServerMode))
	{
		WLog_ERR(TAG, "not expecting a multi-transport request in server mode");
		return STATE_RUN_FAILED;
	}

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 24))
		return STATE_RUN_FAILED;

	UINT32 requestId = 0;
	UINT16 requestedProto = 0;
	UINT16 reserved = 0;
	const BYTE* cookie = nullptr;

	Stream_Read_UINT32(s, requestId);      /* requestId (4 bytes) */
	Stream_Read_UINT16(s, requestedProto); /* requestedProtocol (2 bytes) */
	Stream_Read_UINT16(s, reserved);       /* reserved (2 bytes) */
	cookie = Stream_ConstPointer(s);
	Stream_Seek(s, RDPUDP_COOKIE_LEN); /* securityCookie (16 bytes) */
	if (reserved != 0)
	{
		/*
		 * If the reserved filed is not 0 the request PDU seems to contain some extra data.
		 * If the reserved value is 1, then two bytes of 0 (probably a version field)
		 * are followed by a JSON payload (not null terminated, until the end of the packet.
		 * There seems to be no dedicated length field)
		 *
		 * for now just ignore all that
		 */
		WLog_WARN(TAG,
		          "reserved is %" PRIu16 " instead of 0, skipping %" PRIuz "bytes of unknown data",
		          reserved, Stream_GetRemainingLength(s));
		if (!Stream_SafeSeek(s, Stream_GetRemainingLength(s)))
			return STATE_RUN_FAILED;
	}

	WINPR_ASSERT(multi->MtRequest);
	return multi->MtRequest(multi, requestId, requestedProto, cookie);
}

static BOOL multitransport_request_send(rdpMultitransport* multi, UINT32 reqId, UINT16 reqProto,
                                        const BYTE* cookie)
{
	WINPR_ASSERT(multi);
	UINT16 sec_flags = 0;
	wStream* s = rdp_message_channel_pdu_init(multi->rdp, &sec_flags);
	if (!s)
		return FALSE;

	if (!Stream_EnsureRemainingCapacity(s, 24))
	{
		Stream_Release(s);
		return FALSE;
	}

	Stream_Write_UINT32(s, reqId);              /* requestId (4 bytes) */
	Stream_Write_UINT16(s, reqProto);           /* requestedProtocol (2 bytes) */
	Stream_Zero(s, 2);                          /* reserved (2 bytes) */
	Stream_Write(s, cookie, RDPUDP_COOKIE_LEN); /* securityCookie (16 bytes) */

	return rdp_send_message_channel_pdu(multi->rdp, s, sec_flags | SEC_TRANSPORT_REQ);
}

state_run_t multitransport_server_request(rdpMultitransport* multi, UINT16 reqProto)
{
	WINPR_ASSERT(multi);

	/* TODO: move this static variable to the listener */
	static UINT32 reqId = 0;

	if (reqProto == INITIATE_REQUEST_PROTOCOL_UDPFECR)
	{
		EnterCriticalSection(&multi->lock);
		multi->reliableReqId = reqId++;
		if (winpr_RAND(multi->reliableCookie, sizeof(multi->reliableCookie)) < 0)
		{
			LeaveCriticalSection(&multi->lock);
			return STATE_RUN_FAILED;
		}
		const UINT32 myReq = multi->reliableReqId;
		BYTE cookieCopy[RDPUDP_COOKIE_LEN] = { 0 };
		memcpy(cookieCopy, multi->reliableCookie, sizeof(cookieCopy));
		/* Only one accept thread at a time (single-peer MVP) */
		const BOOL already = multi->threadRunning;
		if (!already)
			multi->threadRunning = TRUE;
		LeaveCriticalSection(&multi->lock);

		if (!multitransport_request_send(multi, myReq, reqProto, cookieCopy))
		{
			EnterCriticalSection(&multi->lock);
			multi->threadRunning = FALSE;
			LeaveCriticalSection(&multi->lock);
			return STATE_RUN_FAILED;
		}

		if (!already)
		{
			UdpAcceptArgs* args = calloc(1, sizeof(UdpAcceptArgs));
			if (args)
			{
				args->multi = multi;
				args->reqId = myReq;
				memcpy(args->cookie, cookieCopy, sizeof(args->cookie));
				const UINT32 sport =
				    freerdp_settings_get_uint32(multi->rdp->settings, FreeRDP_ServerPort);
				args->port = (sport != 0) ? (int)sport : 3389;
				HANDLE th = CreateThread(nullptr, 0, multitransport_udp_accept_thread,
				                         args, 0, nullptr);
				if (th && (th != INVALID_HANDLE_VALUE))
				{
					EnterCriticalSection(&multi->lock);
					if (multi->udpThread && (multi->udpThread != INVALID_HANDLE_VALUE))
						(void)CloseHandle(multi->udpThread);
					multi->udpThread = th;
					LeaveCriticalSection(&multi->lock);
				}
				else
				{
					free(args);
					EnterCriticalSection(&multi->lock);
					multi->threadRunning = FALSE;
					LeaveCriticalSection(&multi->lock);
				}
			}
			else
			{
				EnterCriticalSection(&multi->lock);
				multi->threadRunning = FALSE;
				LeaveCriticalSection(&multi->lock);
			}
		}
		return STATE_RUN_SUCCESS;
	}

	WLog_ERR(TAG, "only reliable transport is supported");
	return STATE_RUN_CONTINUE;
}

BOOL multitransport_client_send_response(rdpMultitransport* multi, UINT32 reqId, HRESULT hr)
{
	WINPR_ASSERT(multi);

	UINT16 sec_flags = 0;
	wStream* s = rdp_message_channel_pdu_init(multi->rdp, &sec_flags);
	if (!s)
		return FALSE;

	if (!Stream_EnsureRemainingCapacity(s, 28))
	{
		Stream_Release(s);
		return FALSE;
	}

	Stream_Write_UINT32(s, reqId); /* requestId (4 bytes) */

	/* [MS-RDPBCGR] 2.2.15.2 Client Initiate Multitransport Response PDU defines this as 4byte
	 * UNSIGNED but https://learn.microsoft.com/en-us/windows/win32/learnwin32/error-codes-in-com
	 * defines this as signed... assume the spec is (implicitly) assuming twos complement. */
	Stream_Write_INT32(s, hr); /* HResult (4 bytes) */
	return rdp_send_message_channel_pdu(multi->rdp, s, sec_flags | SEC_TRANSPORT_RSP);
}

state_run_t multitransport_recv_response(rdpMultitransport* multi, wStream* s)
{
	WINPR_ASSERT(multi && multi->rdp);
	WINPR_ASSERT(s);

	rdpSettings* settings = multi->rdp->settings;
	WINPR_ASSERT(settings);

	if (!freerdp_settings_get_bool(settings, FreeRDP_ServerMode))
	{
		WLog_ERR(TAG, "client is not expecting a multi-transport resp packet");
		return STATE_RUN_FAILED;
	}

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 8))
		return STATE_RUN_FAILED;

	UINT32 requestId = 0;
	UINT32 hr = 0;

	Stream_Read_UINT32(s, requestId); /* requestId (4 bytes) */
	Stream_Read_UINT32(s, hr);        /* hrResponse (4 bytes) */

	state_run_t res = STATE_RUN_SUCCESS;
	IFCALLRET(multi->MtResponse, res, multi, requestId, hr);
	return res;
}

static state_run_t multitransport_no_udp(rdpMultitransport* multi, UINT32 reqId,
                                         WINPR_ATTR_UNUSED UINT16 reqProto,
                                         WINPR_ATTR_UNUSED const BYTE* cookie)
{
	return multitransport_client_send_response(multi, reqId, E_ABORT) ? STATE_RUN_SUCCESS
	                                                                  : STATE_RUN_FAILED;
}

static DWORD WINAPI multitransport_udp_connect_thread(LPVOID arg)
{
	UdpConnectArgs* a = (UdpConnectArgs*)arg;
	if (!a)
		return 1;
	rdpMultitransport* multi = a->multi;
	const UINT32 reqId = a->reqId;

	WLog_INFO(TAG, "UDP connect thread started reqId=%u -> %s:%d", reqId, a->hostname,
	          a->port);

	rdpUdpTransport* udp =
	    rdpeudp_new(multi->rdp->context, a->hostname, a->port, reqId, a->reqProto, a->cookie);
	free(a);
	if (!udp)
	{
		(void)multitransport_client_send_response(multi, reqId, E_ABORT);
		EnterCriticalSection(&multi->lock);
		multi->threadRunning = FALSE;
		LeaveCriticalSection(&multi->lock);
		return 1;
	}

	BOOL ok = FALSE;
	if (rdpeudp_connect(udp, 5000) && rdpeudp_tls_connect(udp) &&
	    rdpeudp_tunnel_create(udp, 5000))
		ok = TRUE;

	EnterCriticalSection(&multi->lock);
	if (multi->closing)
	{
		LeaveCriticalSection(&multi->lock);
		rdpeudp_free(udp);
		EnterCriticalSection(&multi->lock);
		multi->threadRunning = FALSE;
		LeaveCriticalSection(&multi->lock);
		return 1;
	}
	if (ok)
	{
		if (multi->udp)
			rdpeudp_free(multi->udp);
		multi->udp = udp;
	}
	LeaveCriticalSection(&multi->lock);

	if (ok)
	{
		WLog_INFO(TAG, "RDP-UDP tunnel %u ready (async), confirming S_OK", reqId);
		(void)multitransport_client_send_response(multi, reqId, S_OK);
	}
	else
	{
		WLog_WARN(TAG, "RDP-UDP async establishment failed reqId=%u, declining", reqId);
		rdpeudp_free(udp);
		(void)multitransport_client_send_response(multi, reqId, E_ABORT);
	}

	EnterCriticalSection(&multi->lock);
	multi->threadRunning = FALSE;
	LeaveCriticalSection(&multi->lock);
	return ok ? 0 : 1;
}

static DWORD WINAPI multitransport_udp_accept_thread(LPVOID arg)
{
	UdpAcceptArgs* a = (UdpAcceptArgs*)arg;
	if (!a)
		return 1;
	rdpMultitransport* multi = a->multi;
	WLog_INFO(TAG, "UDP accept thread started reqId=%u port=%d", a->reqId, a->port);
	rdpUdpTransport* udp =
	    rdpeudp_accept(multi->rdp->context, a->port, a->reqId, a->cookie, 30000);
	free(a);
	EnterCriticalSection(&multi->lock);
	if (multi->closing)
	{
		LeaveCriticalSection(&multi->lock);
		if (udp)
			rdpeudp_free(udp);
		EnterCriticalSection(&multi->lock);
		multi->threadRunning = FALSE;
		LeaveCriticalSection(&multi->lock);
		return 1;
	}
	if (udp)
	{
		if (multi->udp)
			rdpeudp_free(multi->udp);
		multi->udp = udp;
		WLog_INFO(TAG, "RDP-UDP server tunnel ready");
	}
	multi->threadRunning = FALSE;
	LeaveCriticalSection(&multi->lock);
	return udp ? 0 : 1;
}

static state_run_t multitransport_client_request_udp(rdpMultitransport* multi, UINT32 reqId,
                                                     UINT16 reqProto, const BYTE* cookie)
{
	WINPR_ASSERT(multi);
	WINPR_ASSERT(cookie);

	rdpRdp* rdp = multi->rdp;
	WINPR_ASSERT(rdp);
	rdpSettings* settings = rdp->settings;
	WINPR_ASSERT(settings);

	if (reqProto != INITIATE_REQUEST_PROTOCOL_UDPFECR)
	{
		WLog_WARN(TAG, "server requested protocol 0x%04" PRIx16 ", only reliable supported",
		          reqProto);
		return multitransport_no_udp(multi, reqId, reqProto, cookie);
	}

	if (!freerdp_settings_get_bool(settings, FreeRDP_SupportMultitransport) ||
	    ((freerdp_settings_get_uint32(settings, FreeRDP_MultitransportFlags) &
	      TRANSPORT_TYPE_UDP_FECR) == 0))
	{
		return multitransport_no_udp(multi, reqId, reqProto, cookie);
	}

	const char* hostname = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
	const UINT32 port = freerdp_settings_get_uint32(settings, FreeRDP_ServerPort);
	if (!hostname || (hostname[0] == '\0'))
		return multitransport_no_udp(multi, reqId, reqProto, cookie);

	EnterCriticalSection(&multi->lock);
	if (multi->threadRunning)
	{
		LeaveCriticalSection(&multi->lock);
		WLog_WARN(TAG, "UDP establishment already in progress, declining req %u", reqId);
		return multitransport_no_udp(multi, reqId, reqProto, cookie);
	}
	multi->threadRunning = TRUE;
	LeaveCriticalSection(&multi->lock);

	UdpConnectArgs* args = calloc(1, sizeof(UdpConnectArgs));
	if (!args)
	{
		EnterCriticalSection(&multi->lock);
		multi->threadRunning = FALSE;
		LeaveCriticalSection(&multi->lock);
		return multitransport_no_udp(multi, reqId, reqProto, cookie);
	}
	args->multi = multi;
	args->reqId = reqId;
	args->reqProto = reqProto;
	memcpy(args->cookie, cookie, RDPUDP_COOKIE_LEN);
	strncpy(args->hostname, hostname, sizeof(args->hostname) - 1);
	args->port = (int)port;

	WLog_INFO(TAG, "spawning async UDP establishment reqId=%u -> %s:%d", reqId, args->hostname,
	          args->port);
	HANDLE th = CreateThread(nullptr, 0, multitransport_udp_connect_thread, args, 0, nullptr);
	if (!th || (th == INVALID_HANDLE_VALUE))
	{
		free(args);
		EnterCriticalSection(&multi->lock);
		multi->threadRunning = FALSE;
		LeaveCriticalSection(&multi->lock);
		return multitransport_no_udp(multi, reqId, reqProto, cookie);
	}
	EnterCriticalSection(&multi->lock);
	if (multi->udpThread && (multi->udpThread != INVALID_HANDLE_VALUE))
		(void)CloseHandle(multi->udpThread);
	multi->udpThread = th;
	LeaveCriticalSection(&multi->lock);
	/* Response (S_OK/E_ABORT) will be sent by the thread when ready.
	 * Return SUCCESS to keep RDP state machine moving (capabilities
	 * exchange proceeds on TCP meanwhile). */
	return STATE_RUN_SUCCESS;
}

static state_run_t multitransport_server_handle_response(rdpMultitransport* multi,
                                                         WINPR_ATTR_UNUSED UINT32 reqId,
                                                         WINPR_ATTR_UNUSED UINT32 hrResponse)
{
	rdpRdp* rdp = multi->rdp;

	if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_CAPABILITIES_EXCHANGE_DEMAND_ACTIVE))
		return STATE_RUN_FAILED;

	return STATE_RUN_CONTINUE;
}

rdpMultitransport* multitransport_new(rdpRdp* rdp, WINPR_ATTR_UNUSED UINT16 protocol)
{
	WINPR_ASSERT(rdp);

	rdpSettings* settings = rdp->settings;
	WINPR_ASSERT(settings);

	rdpMultitransport* multi = calloc(1, sizeof(rdpMultitransport));
	if (!multi)
		return nullptr;

	if (!InitializeCriticalSectionAndSpinCount(&multi->lock, 4000))
	{
		free(multi);
		return nullptr;
	}
	multi->udpThread = nullptr;
	multi->closing = FALSE;
	multi->threadRunning = FALSE;

	if (freerdp_settings_get_bool(settings, FreeRDP_ServerMode))
	{
		multi->MtResponse = multitransport_server_handle_response;
	}
	else
	{
		multi->MtRequest = multitransport_client_request_udp;
	}

	multi->rdp = rdp;
	return multi;
}

void multitransport_free(rdpMultitransport* multitransport)
{
	if (!multitransport)
		return;
	EnterCriticalSection(&multitransport->lock);
	multitransport->closing = TRUE;
	HANDLE th = multitransport->udpThread;
	multitransport->udpThread = nullptr;
	LeaveCriticalSection(&multitransport->lock);
	if (th && (th != INVALID_HANDLE_VALUE))
	{
		(void)WaitForSingleObject(th, 5000);
		(void)CloseHandle(th);
	}
	EnterCriticalSection(&multitransport->lock);
	if (multitransport->udp)
	{
		rdpeudp_free(multitransport->udp);
		multitransport->udp = nullptr;
	}
	LeaveCriticalSection(&multitransport->lock);
	DeleteCriticalSection(&multitransport->lock);
	free(multitransport);
}

BOOL multitransport_is_udp_connected(const rdpMultitransport* multi)
{
	if (!multi)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&multi->lock);
	BOOL rc = multi->udp && rdpeudp_is_connected(multi->udp);
	LeaveCriticalSection((CRITICAL_SECTION*)&multi->lock);
	return rc;
}

rdpUdpTransport* multitransport_get_udp(rdpMultitransport* multi)
{
	if (!multi)
		return nullptr;
	EnterCriticalSection(&multi->lock);
	rdpUdpTransport* udp = multi->udp;
	LeaveCriticalSection(&multi->lock);
	return udp;
}

static BOOL multitransport_is_drdynvc_channel(rdpRdp* rdp, UINT16 channelId)
{
	if (!rdp || !rdp->mcs || !rdp->mcs->channels)
		return FALSE;
	for (UINT32 i = 0; i < rdp->mcs->channelCount; i++)
	{
		const rdpMcsChannel* ch = &rdp->mcs->channels[i];
		if (ch->ChannelId == channelId)
			return strncmp(ch->Name, "drdynvc", 7) == 0;
	}
	return FALSE;
}

BOOL multitransport_send_channel_packet(rdpMultitransport* multi, UINT16 channelId,
                                         size_t totalSize, UINT32 flags, const BYTE* chunk,
                                         size_t chunkLen)
{
	if (!multi)
		return FALSE;
	EnterCriticalSection(&multi->lock);
	rdpUdpTransport* udp = multi->udp;
	BOOL connected = udp && rdpeudp_is_connected(udp);
	LeaveCriticalSection(&multi->lock);
	if (!connected)
		return FALSE;
	/* Only dynamic-channel multiplex (drdynvc, carries egfx/audio/etc.)
	 * goes over UDP for now. Static channels stay on TCP to avoid
	 * reassembly divergences. */
	if (!multitransport_is_drdynvc_channel(multi->rdp, channelId))
		return FALSE;

	wStream* pkt = rdpeudp_build_channel_packet(channelId, (UINT32)totalSize, flags, chunk,
	                                            chunkLen);
	if (!pkt)
		return FALSE;
	const size_t plen = Stream_Length(pkt);
	BYTE* pbuf = Stream_Buffer(pkt);
	/* Tunnel DATA payload is the channel packet bytes */
	const SSIZE_T rc = rdpeudp_tunnel_send(udp, pbuf, plen);
	Stream_Release(pkt);
	if (rc != (SSIZE_T)plen)
	{
		WLog_WARN(TAG, "UDP channel send failed, falling back to TCP");
		return FALSE;
	}
	/* Opportunistic keepalive piggybacks on data */
	(void)rdpeudp_check_keepalive(udp, 5000);
	return TRUE;
}

HANDLE multitransport_get_event(rdpMultitransport* multi)
{
	if (!multi)
		return nullptr;
	EnterCriticalSection(&multi->lock);
	rdpUdpTransport* udp = multi->udp;
	HANDLE ev = udp ? rdpeudp_get_event(udp) : nullptr;
	LeaveCriticalSection(&multi->lock);
	return ev;
}

BOOL multitransport_send_autodetect(rdpMultitransport* multi, BOOL isRequest, UINT16 secFlags,
                                     const BYTE* pdu, size_t pduLen)
{
	if (!multi)
		return FALSE;
	EnterCriticalSection(&multi->lock);
	rdpUdpTransport* udp = multi->udp;
	BOOL connected = udp && rdpeudp_is_connected(udp);
	LeaveCriticalSection(&multi->lock);
	if (!connected)
		return FALSE;

	wStream* pkt = rdpeudp_build_autodetect_packet(isRequest, secFlags, pdu, pduLen);
	if (!pkt)
		return FALSE;
	const size_t plen = Stream_Length(pkt);
	const SSIZE_T rc = rdpeudp_tunnel_send(udp, Stream_Buffer(pkt), plen);
	Stream_Release(pkt);
	if (rc != (SSIZE_T)plen)
		return FALSE;
	(void)rdpeudp_check_keepalive(udp, 5000);
	return TRUE;
}

int multitransport_check_fds(rdpMultitransport* multi)
{
	BYTE buf[65535] = { 0 };
	if (!multi)
		return 0;
	EnterCriticalSection(&multi->lock);
	rdpUdpTransport* udp = multi->udp;
	BOOL connected = udp && rdpeudp_is_connected(udp);
	LeaveCriticalSection(&multi->lock);
	if (!connected)
		return 0;

	/* Drain all pending Tunnel DATA without blocking */
	int dispatched = 0;
	while (TRUE)
	{
		/* Use locked udp pointer (may change async, re-fetch each loop is safer
		 * but pointer stays valid until multitransport_free which waits) */
		EnterCriticalSection(&multi->lock);
		rdpUdpTransport* curUdp = multi->udp;
		LeaveCriticalSection(&multi->lock);
		if (!curUdp)
			break;
		const SSIZE_T r = rdpeudp_tunnel_recv(curUdp, buf, sizeof(buf), 0);
		if (r <= 0)
			break;
		BYTE ptype = 0xFF;
		if (!rdpeudp_parse_tunnel_ptype(buf, (size_t)r, &ptype))
		{
			WLog_WARN(TAG, "bad UDP tunnel payload, ignoring");
			continue;
		}
		if (ptype == 0x00)
		{
			UINT16 channelId = 0;
			UINT32 totalSize = 0;
			UINT32 flags = 0;
			const BYTE* chunk = nullptr;
			size_t chunkLen = 0;
			if (!rdpeudp_parse_channel_packet(buf, (size_t)r, &channelId, &totalSize,
			                                   &flags, &chunk, &chunkLen))
			{
				WLog_WARN(TAG, "bad UDP channel packet, ignoring");
				continue;
			}
		rdpRdp* rdp = multi->rdp;
		if (!rdp || !rdp->context || !rdp->context->instance)
			continue;
		const BOOL serverMode =
		    freerdp_settings_get_bool(rdp->settings, FreeRDP_ServerMode);
		if (!serverMode)
		{
			freerdp* instance = rdp->context->instance;
			if (instance->ReceiveChannelData)
			{
				if (!instance->ReceiveChannelData(instance, channelId, chunk, chunkLen,
				                                  flags, totalSize))
					return -1;
				dispatched++;
			}
		}
		else
		{
			/* Server: dispatch to peer callbacks if present */
			freerdp_peer* client = rdp->context->peer;
			if (client && client->ReceiveChannelData)
			{
				if (!client->ReceiveChannelData(client, channelId, chunk,
				                                (UINT32)chunkLen, flags, totalSize))
					return -1;
				dispatched++;
			}
			else if (client && client->VirtualChannelRead)
			{
				/* Fallback: find handle by channelId */
				HANDLE hChannel = nullptr;
				for (UINT32 i = 0; i < rdp->mcs->channelCount; i++)
				{
					if (rdp->mcs->channels[i].ChannelId == channelId)
					{
						hChannel = (HANDLE)rdp->mcs->channels[i].handle;
						break;
					}
				}
				if (hChannel &&
				    (client->VirtualChannelRead(client, hChannel, (BYTE*)chunk,
				                                (UINT32)chunkLen) < 0))
					return -1;
				dispatched++;
			}
		}
		}
		else if ((ptype == 0x01) || (ptype == 0x02))
		{
			BOOL isReq = (ptype == 0x01);
			UINT16 secFlags = 0;
			const BYTE* pdu = nullptr;
			size_t pduLen = 0;
			if (!rdpeudp_parse_autodetect_packet(buf, (size_t)r, &isReq, &secFlags, &pdu,
			                                     &pduLen))
			{
				WLog_WARN(TAG, "bad UDP autodetect packet, ignoring");
				continue;
			}
			/* Wrap PDU bytes into wStream for autodetect handlers (transport=UDP_R) */
			wStream* s = Stream_New((BYTE*)pdu, pduLen);
			if (!s)
				continue;
			Stream_SetPosition(s, 0);
			Stream_SetLength(s, pduLen);
			rdpRdp* rdp2 = multi->rdp;
			state_run_t ares = STATE_RUN_SUCCESS;
			if (rdp2 && rdp2->autodetect)
			{
				if (isReq)
					ares = autodetect_recv_request_packet(rdp2->autodetect,
					                                      RDP_TRANSPORT_UDP_R, s);
				else
					ares = autodetect_recv_response_packet(rdp2->autodetect,
					                                       RDP_TRANSPORT_UDP_R, s);
				if (state_run_failed(ares))
					WLog_WARN(TAG, "UDP autodetect dispatch failed");
				else
					dispatched++;
			}
			Stream_Release(s);
		}
		else
		{
			WLog_WARN(TAG, "unknown UDP tunnel ptype 0x%02x", ptype);
		}
	}
	/* Keepalive if idle */
	EnterCriticalSection(&multi->lock);
	udp = multi->udp;
	LeaveCriticalSection(&multi->lock);
	if (udp)
	{
		(void)rdpeudp_check_keepalive(udp, 5000);
		(void)ResetEvent(rdpeudp_get_event(udp));
	}
	return dispatched;
}

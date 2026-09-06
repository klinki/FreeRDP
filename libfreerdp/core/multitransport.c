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
#include <winpr/wtsapi.h>

#include "settings.h"
#include "rdp.h"
#include "multitransport.h"
#include "rdpeudp.h"
#include "udp.h"
#include "autodetect.h"

/* Hexdump helper for the UDP_TRACE sites below (temporary with the macro:
 * exact failing tunnel bytes for live runs, no add/remove churn). */
static void udp_trace_bytes(const char* label, const BYTE* data, size_t len)
{
	size_t hn = (len < 170) ? len : 170;
	UDP_TRACE("%s len=%zu [", label, len);
	for (size_t hi = 0; hi < hn; hi++)
		UDP_TRACE("%02x ", data[hi]);
	UDP_TRACE("]%s\n", (len > hn) ? " (truncated)" : "");
}

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
	HANDLE abortEvent; /* manual-reset, signaled by free to cancel workers */
	HANDLE stateEvent; /* auto-reset, signaled when async establishment completes */
	BOOL closing;
	BOOL threadRunning;
	/* Soft-Sync migration state (MS-RDPEDYC 3.1.5.3): DVC stays on TCP until
	 * migration is authorized, even when the tunnel is up. */
	BOOL softSyncNegotiated;
	BOOL softSyncComplete;
	BOOL udpSendMigrated;
	BOOL udpRecvMigrated;
	/* Per-DVC migration map (S2): DVC IDs listed under UDPFECR tunnels in the
	 * last validated Soft-Sync Request. Active only when a listed request was
	 * accepted; otherwise (no lists / no Soft-Sync) the whole multiplex may
	 * migrate once the direction is enabled. */
	UINT32* udpDvcIds;
	size_t udpDvcCount;
	BOOL mappingActive;
	/* Explicit install record (T1): set only by a validated request install
	 * (listed IDs or authorized migrate-all). Gates the response hooks so a
	 * failed/skipped install can never become migrate-all. */
	BOOL mappingInstalled;
	/* Soft-Sync request reassembly (T1): drdynvc static-channel chunks for one
	 * request PDU (FIRST..LAST). Passive; normal dispatch is unaffected. */
	BYTE* ssReqBuf;
	size_t ssReqLen;
	size_t ssReqCap;
	BOOL ssReqActive;
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

	EnterCriticalSection(&multi->lock);
	const BOOL earlyClose = multi->closing;
	HANDLE abortEv = multi->abortEvent;
	LeaveCriticalSection(&multi->lock);
	if (earlyClose)
	{
		free(a);
		EnterCriticalSection(&multi->lock);
		multi->threadRunning = FALSE;
		if (multi->stateEvent && (multi->stateEvent != INVALID_HANDLE_VALUE))
			(void)SetEvent(multi->stateEvent);
		LeaveCriticalSection(&multi->lock);
		return 1;
	}

	WLog_INFO(TAG, "UDP connect thread started reqId=%u -> %s:%d", reqId, a->hostname,
	          a->port);

	rdpUdpTransport* udp =
	    rdpeudp_new(multi->rdp->context, a->hostname, a->port, reqId, a->reqProto, a->cookie);
	free(a);
	if (!udp)
	{
		EnterCriticalSection(&multi->lock);
		const BOOL closed = multi->closing;
		HANDLE stateEv = multi->stateEvent;
		multi->threadRunning = FALSE;
		LeaveCriticalSection(&multi->lock);
		if (!closed)
			(void)multitransport_client_send_response(multi, reqId, E_ABORT);
		if (stateEv && (stateEv != INVALID_HANDLE_VALUE))
			(void)SetEvent(stateEv);
		return 1;
	}
	rdpeudp_set_abort_event(udp, abortEv);

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
		HANDLE stateEv = multi->stateEvent;
		LeaveCriticalSection(&multi->lock);
		if (stateEv && (stateEv != INVALID_HANDLE_VALUE))
			(void)SetEvent(stateEv);
		return 1;
	}
	if (ok)
	{
		if (multi->udp)
			rdpeudp_free(multi->udp);
		free(multi->udpDvcIds);
		multi->udpDvcIds = nullptr;
		multi->udpDvcCount = 0;
		multi->mappingActive = FALSE;
		multi->mappingInstalled = FALSE;
		free(multi->ssReqBuf);
		multi->ssReqBuf = nullptr;
		multi->ssReqLen = 0;
		multi->ssReqCap = 0;
		multi->ssReqActive = FALSE;
		multi->udp = udp;
		/* Snapshot negotiated Soft-Sync support for migration gating
		 * (MS-RDPEDYC 3.1.5.3, MS-RDPEMT 1.3).
		 * Without Soft-Sync, migration is authorized by tunnel creation BUT
		 * only if no DVC traffic could have flowed yet (state < ACTIVE).
		 * Post-ACTIVE tunnel completion stays on TCP to avoid reordering
		 * TCP in-flight vs new UDP traffic without a TCP_FLUSHED barrier. */
		{
			const UINT32 flags =
			    freerdp_settings_get_uint32(multi->rdp->settings, FreeRDP_MultitransportFlags);
			const BOOL softSync = ((flags & SOFTSYNC_TCP_TO_UDP) != 0);
			const BOOL preActive =
			    (multi->rdp->state < CONNECTION_STATE_ACTIVE);
			multi->softSyncNegotiated = softSync;
			free(multi->udpDvcIds);
			multi->udpDvcIds = nullptr;
			multi->udpDvcCount = 0;
			multi->mappingActive = FALSE;
			multi->mappingInstalled = FALSE;
			free(multi->ssReqBuf);
			multi->ssReqBuf = nullptr;
			multi->ssReqLen = 0;
			multi->ssReqCap = 0;
			multi->ssReqActive = FALSE;
			if (!softSync)
			{
				multi->softSyncComplete = TRUE;
				/* Latch migration only if pre-ACTIVE; else stay TCP-safe. */
				multi->udpSendMigrated = preActive;
				multi->udpRecvMigrated = preActive;
				if (!preActive)
					WLog_WARN(TAG, "UDP tunnel ready post-ACTIVE without Soft-Sync; "
					               "staying on TCP to preserve ordering");
			}
			else
			{
				/* With Soft-Sync, wait for handshake (see on_soft_sync_*). */
				multi->softSyncComplete = FALSE;
				multi->udpSendMigrated = FALSE;
				multi->udpRecvMigrated = FALSE;
			}
		}
	}
	HANDLE stateEv = multi->stateEvent;
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
	if (stateEv && (stateEv != INVALID_HANDLE_VALUE))
		(void)SetEvent(stateEv);
	return ok ? 0 : 1;
}

static DWORD WINAPI multitransport_udp_accept_thread(LPVOID arg)
{
	UdpAcceptArgs* a = (UdpAcceptArgs*)arg;
	if (!a)
		return 1;
	rdpMultitransport* multi = a->multi;
	EnterCriticalSection(&multi->lock);
	const BOOL earlyClose = multi->closing;
	HANDLE abortEv = multi->abortEvent;
	LeaveCriticalSection(&multi->lock);
	if (earlyClose)
	{
		free(a);
		EnterCriticalSection(&multi->lock);
		multi->threadRunning = FALSE;
		if (multi->stateEvent && (multi->stateEvent != INVALID_HANDLE_VALUE))
			(void)SetEvent(multi->stateEvent);
		LeaveCriticalSection(&multi->lock);
		return 1;
	}
	WLog_INFO(TAG, "UDP accept thread started reqId=%u port=%d", a->reqId, a->port);
	rdpUdpTransport* udp =
	    rdpeudp_accept_ex(multi->rdp->context, a->port, a->reqId, a->cookie, 30000, abortEv);
	free(a);
	if (udp)
		rdpeudp_set_abort_event(udp, abortEv);
	EnterCriticalSection(&multi->lock);
	if (multi->closing)
	{
		LeaveCriticalSection(&multi->lock);
		if (udp)
			rdpeudp_free(udp);
		EnterCriticalSection(&multi->lock);
		multi->threadRunning = FALSE;
		HANDLE stateEv = multi->stateEvent;
		LeaveCriticalSection(&multi->lock);
		if (stateEv && (stateEv != INVALID_HANDLE_VALUE))
			(void)SetEvent(stateEv);
		return 1;
	}
	if (udp)
	{
		if (multi->udp)
			rdpeudp_free(multi->udp);
		free(multi->udpDvcIds);
		multi->udpDvcIds = nullptr;
		multi->udpDvcCount = 0;
		multi->mappingActive = FALSE;
		multi->mappingInstalled = FALSE;
		free(multi->ssReqBuf);
		multi->ssReqBuf = nullptr;
		multi->ssReqLen = 0;
		multi->ssReqCap = 0;
		multi->ssReqActive = FALSE;
		multi->udp = udp;
		{
			const UINT32 flags =
			    freerdp_settings_get_uint32(multi->rdp->settings, FreeRDP_MultitransportFlags);
			const BOOL softSync = ((flags & SOFTSYNC_TCP_TO_UDP) != 0);
			const BOOL preActive = (multi->rdp->state < CONNECTION_STATE_ACTIVE);
			multi->softSyncNegotiated = softSync;
			free(multi->udpDvcIds);
			multi->udpDvcIds = nullptr;
			multi->udpDvcCount = 0;
			multi->mappingActive = FALSE;
			multi->mappingInstalled = FALSE;
			free(multi->ssReqBuf);
			multi->ssReqBuf = nullptr;
			multi->ssReqLen = 0;
			multi->ssReqCap = 0;
			multi->ssReqActive = FALSE;
			if (!softSync)
			{
				multi->softSyncComplete = TRUE;
				multi->udpSendMigrated = preActive;
				multi->udpRecvMigrated = preActive;
				if (!preActive)
					WLog_WARN(TAG, "UDP server tunnel ready post-ACTIVE without "
					               "Soft-Sync; staying on TCP");
			}
			else
			{
				multi->softSyncComplete = FALSE;
				multi->udpSendMigrated = FALSE;
				multi->udpRecvMigrated = FALSE;
			}
		}
		WLog_INFO(TAG, "RDP-UDP server tunnel ready");
	}
	multi->threadRunning = FALSE;
	HANDLE stateEv = multi->stateEvent;
	LeaveCriticalSection(&multi->lock);
	if (stateEv && (stateEv != INVALID_HANDLE_VALUE))
		(void)SetEvent(stateEv);
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
	multi->abortEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	multi->stateEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!multi->abortEvent || (multi->abortEvent == INVALID_HANDLE_VALUE) || !multi->stateEvent ||
	    (multi->stateEvent == INVALID_HANDLE_VALUE))
	{
		if (multi->abortEvent && (multi->abortEvent != INVALID_HANDLE_VALUE))
			(void)CloseHandle(multi->abortEvent);
		if (multi->stateEvent && (multi->stateEvent != INVALID_HANDLE_VALUE))
			(void)CloseHandle(multi->stateEvent);
		DeleteCriticalSection(&multi->lock);
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
	/* Signal cancellation first so blocking establishment loops exit promptly,
	 * then join the worker before destroying shared state it may access. */
	EnterCriticalSection(&multitransport->lock);
	multitransport->closing = TRUE;
	HANDLE th = multitransport->udpThread;
	multitransport->udpThread = nullptr;
	HANDLE abortEv = multitransport->abortEvent;
	LeaveCriticalSection(&multitransport->lock);
	if (abortEv && (abortEv != INVALID_HANDLE_VALUE))
		(void)SetEvent(abortEv);
	if (th && (th != INVALID_HANDLE_VALUE))
	{
		(void)WaitForSingleObject(th, INFINITE);
		(void)CloseHandle(th);
	}
	EnterCriticalSection(&multitransport->lock);
	if (multitransport->udp)
	{
		rdpeudp_free(multitransport->udp);
		multitransport->udp = nullptr;
	}
	free(multitransport->udpDvcIds);
	multitransport->udpDvcIds = nullptr;
	multitransport->udpDvcCount = 0;
	multitransport->mappingActive = FALSE;
	multitransport->mappingInstalled = FALSE;
	free(multitransport->ssReqBuf);
	multitransport->ssReqBuf = nullptr;
	multitransport->ssReqLen = 0;
	multitransport->ssReqCap = 0;
	multitransport->ssReqActive = FALSE;
	HANDLE stateEv = multitransport->stateEvent;
	multitransport->stateEvent = nullptr;
	/* abortEvent closed after worker join; transport only borrows it. */
	LeaveCriticalSection(&multitransport->lock);
	if (stateEv && (stateEv != INVALID_HANDLE_VALUE))
		(void)CloseHandle(stateEv);
	if (abortEv && (abortEv != INVALID_HANDLE_VALUE))
		(void)CloseHandle(abortEv);
	multitransport->abortEvent = nullptr;
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

BOOL multitransport_is_udp_send_migrated(const rdpMultitransport* multi)
{
	BOOL rc = FALSE;
	if (!multi)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&multi->lock);
	rc = multi->udp && rdpeudp_is_connected(multi->udp) && multi->udpSendMigrated;
	LeaveCriticalSection((CRITICAL_SECTION*)&multi->lock);
	return rc;
}

BOOL multitransport_is_udp_recv_migrated(const rdpMultitransport* multi)
{
	BOOL rc = FALSE;
	if (!multi)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&multi->lock);
	rc = multi->udp && rdpeudp_is_connected(multi->udp) && multi->udpRecvMigrated;
	LeaveCriticalSection((CRITICAL_SECTION*)&multi->lock);
	return rc;
}

/* Strict-install a validated Soft-Sync mapping (lock held). Two-phase
 * extraction allocates for the actual validated ID count (T1: no fixed stack
 * cap). Sets mappingInstalled on success: listed IDs (mappingActive) or
 * authorized migrate-all when the request carries no lists. */
static BOOL multitransport_install_mapping_locked(rdpMultitransport* multi, const BYTE* pdu,
                                                  size_t len)
{
	size_t count = 0;
	WINPR_ASSERT(multi);
	/* Phase 1: strict-validate and count. NOTE: never touches ssReqBuf here;
	 * the feed path passes its own reassembly buffer as pdu. */
	if (!rdpeudp_soft_sync_request_udp_dvcs(pdu, len, nullptr, 0, &count))
		return FALSE;
	free(multi->udpDvcIds);
	multi->udpDvcIds = nullptr;
	multi->udpDvcCount = 0;
	multi->mappingActive = FALSE;
	multi->mappingInstalled = FALSE;
	if (count > 0)
	{
		UINT32* ids = malloc(count * sizeof(UINT32));
		size_t got = 0;
		if (!ids)
			return FALSE; /* OOM: stay TCP-safe */
		/* Phase 2: fill (revalidates; cannot fail after phase 1). */
		if (!rdpeudp_soft_sync_request_udp_dvcs(pdu, len, ids, count, &got) || (got != count))
		{
			free(ids);
			return FALSE;
		}
		multi->udpDvcIds = ids;
		multi->udpDvcCount = count;
		multi->mappingActive = TRUE;
	}
	multi->mappingInstalled = TRUE;
	return TRUE;
}

void multitransport_on_soft_sync_request_sent(rdpMultitransport* multi, const BYTE* pdu,
                                                size_t len)
{
	if (!multi || !pdu || (len == 0))
		return;
	EnterCriticalSection(&multi->lock);
	/* Server: after sending Soft-Sync Request it starts sending on UDP
	 * (MS-RDPEDYC 3.1.5.3). Recv still on TCP until Response arrives. */
	if (multi->softSyncNegotiated && multi->udp && rdpeudp_is_connected(multi->udp))
	{
		if (multitransport_install_mapping_locked(multi, pdu, len))
			multi->udpSendMigrated = TRUE;
	}
	LeaveCriticalSection(&multi->lock);
}

/* Feed one drdynvc static-channel chunk (T1: handles fragmented Soft-Sync
 * requests the single-chunk fast path cannot see). Passive: normal dispatch is
 * unaffected. Accumulates FIRST..LAST when the first chunk opens a Soft-Sync
 * Request; on LAST the validated mapping is installed and recv is enabled. */
void multitransport_soft_sync_recv_feed(rdpMultitransport* multi, const BYTE* chunk,
                                        size_t chunkLen, UINT32 flags)
{
	BOOL installed = FALSE;
	if (!multi || !chunk || (chunkLen == 0))
		return;
	EnterCriticalSection(&multi->lock);
	if (!multi->softSyncNegotiated || !multi->udp || !rdpeudp_is_connected(multi->udp))
	{
		LeaveCriticalSection(&multi->lock);
		return;
	}
	if (!multi->ssReqActive)
	{
		/* Start only on a chunk opening a Soft-Sync Request. */
		if (!(flags & CHANNEL_FLAG_FIRST) || (((chunk[0] >> 4) & 0x0F) != 0x08))
		{
			LeaveCriticalSection(&multi->lock);
			return;
		}
		free(multi->ssReqBuf);
		multi->ssReqBuf = malloc(chunkLen);
		if (!multi->ssReqBuf)
		{
			LeaveCriticalSection(&multi->lock);
			return; /* OOM: stay TCP-safe */
		}
		memcpy(multi->ssReqBuf, chunk, chunkLen);
		multi->ssReqLen = chunkLen;
		multi->ssReqCap = chunkLen;
		multi->ssReqActive = TRUE;
	}
	else
	{
		/* Continuation: a new FIRST means the previous PDU never completed;
		 * resync by restarting (TCP-safe: nothing was installed for it). */
		if (flags & CHANNEL_FLAG_FIRST)
		{
			free(multi->ssReqBuf);
			multi->ssReqBuf = nullptr;
			multi->ssReqLen = 0;
			multi->ssReqCap = 0;
			multi->ssReqActive = FALSE;
			if ((((chunk[0] >> 4) & 0x0F) != 0x08))
			{
				LeaveCriticalSection(&multi->lock);
				return;
			}
			multi->ssReqBuf = malloc(chunkLen);
			if (!multi->ssReqBuf)
			{
				LeaveCriticalSection(&multi->lock);
				return;
			}
			memcpy(multi->ssReqBuf, chunk, chunkLen);
			multi->ssReqLen = chunkLen;
			multi->ssReqCap = chunkLen;
			multi->ssReqActive = TRUE;
		}
		else
		{
			if (multi->ssReqLen + chunkLen > 65536)
			{
				/* Runaway accumulation: drop and stay TCP-safe. */
				free(multi->ssReqBuf);
				multi->ssReqBuf = nullptr;
				multi->ssReqLen = 0;
				multi->ssReqCap = 0;
				multi->ssReqActive = FALSE;
				LeaveCriticalSection(&multi->lock);
				return;
			}
			BYTE* grown = realloc(multi->ssReqBuf, multi->ssReqLen + chunkLen);
			if (!grown)
			{
				free(multi->ssReqBuf);
				multi->ssReqBuf = nullptr;
				multi->ssReqLen = 0;
				multi->ssReqCap = 0;
				multi->ssReqActive = FALSE;
				LeaveCriticalSection(&multi->lock);
				return;
			}
			multi->ssReqBuf = grown;
			memcpy(multi->ssReqBuf + multi->ssReqLen, chunk, chunkLen);
			multi->ssReqLen += chunkLen;
			multi->ssReqCap = multi->ssReqLen;
		}
	}
	if (flags & CHANNEL_FLAG_LAST)
	{
		/* Complete: install mapping and enable recv (early server UDP data
		 * must not be missed); send waits for our Response. */
		if (multitransport_install_mapping_locked(multi, multi->ssReqBuf, multi->ssReqLen))
		{
			multi->udpRecvMigrated = TRUE;
			installed = TRUE;
		}
		free(multi->ssReqBuf);
		multi->ssReqBuf = nullptr;
		multi->ssReqLen = 0;
		multi->ssReqCap = 0;
		multi->ssReqActive = FALSE;
	}
	LeaveCriticalSection(&multi->lock);
	if (installed && multi->stateEvent && (multi->stateEvent != INVALID_HANDLE_VALUE))
		(void)SetEvent(multi->stateEvent);
}

BOOL multitransport_is_dvc_migrated(const rdpMultitransport* multi, UINT32 dvcId)
{
	BOOL rc = FALSE;
	if (!multi)
		return FALSE;
	EnterCriticalSection((CRITICAL_SECTION*)&multi->lock);
	if (multi->udp && rdpeudp_is_connected(multi->udp) && multi->udpSendMigrated)
	{
		if (!multi->mappingActive)
			rc = TRUE; /* migrate-all (no lists / no Soft-Sync pre-ACTIVE) */
		else
		{
			for (size_t i = 0; i < multi->udpDvcCount; i++)
			{
				if (multi->udpDvcIds[i] == dvcId)
				{
					rc = TRUE;
					break;
				}
			}
		}
	}
	LeaveCriticalSection((CRITICAL_SECTION*)&multi->lock);
	return rc;
}

void multitransport_on_soft_sync_response_sent(rdpMultitransport* multi)
{
	if (!multi)
		return;
	EnterCriticalSection(&multi->lock);
	/* Client: after sending Response, start sending (and ensure recv) on UDP,
	 * but only when a validated mapping was installed first (T1: a failed or
	 * skipped install must never become migrate-all). */
	if (multi->softSyncNegotiated && multi->mappingInstalled && multi->udp &&
	    rdpeudp_is_connected(multi->udp))
	{
		multi->softSyncComplete = TRUE;
		multi->udpSendMigrated = TRUE;
		multi->udpRecvMigrated = TRUE;
	}
	LeaveCriticalSection(&multi->lock);
	if (multi->stateEvent && (multi->stateEvent != INVALID_HANDLE_VALUE))
		(void)SetEvent(multi->stateEvent);
}

void multitransport_on_soft_sync_response_received(rdpMultitransport* multi)
{
	if (!multi)
		return;
	EnterCriticalSection(&multi->lock);
	/* Server: after receiving Response it starts receiving on UDP, but only
	 * when it previously installed a mapping by sending the Request (T1:
	 * stray responses must not migrate). */
	if (multi->softSyncNegotiated && multi->mappingInstalled && multi->udp &&
	    rdpeudp_is_connected(multi->udp))
	{
		multi->softSyncComplete = TRUE;
		multi->udpRecvMigrated = TRUE;
	}
	LeaveCriticalSection(&multi->lock);
	if (multi->stateEvent && (multi->stateEvent != INVALID_HANDLE_VALUE))
		(void)SetEvent(multi->stateEvent);
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

/* Static channel id of drdynvc in OUR mcs table, for routing UDP-received
 * DVC PDUs into the same ReceiveChannelData path TCP uses. */
static BOOL multitransport_drdynvc_channel_id(rdpRdp* rdp, UINT16* channelIdOut)
{
	if (!rdp || !rdp->mcs || !rdp->mcs->channels)
		return FALSE;
	for (UINT32 i = 0; i < rdp->mcs->channelCount; i++)
	{
		const rdpMcsChannel* ch = &rdp->mcs->channels[i];
		if (strncmp(ch->Name, "drdynvc", 7) == 0)
		{
			if (channelIdOut)
				*channelIdOut = ch->ChannelId;
			return TRUE;
		}
	}
	return FALSE;
}

BOOL multitransport_send_channel_packet(rdpMultitransport* multi, UINT16 channelId,
                                         size_t totalSize, UINT32 flags, const BYTE* chunk,
                                         size_t chunkLen)
{
	if (!multi)
		return FALSE;
	/* Gate on negotiated migration, not mere connectivity (MS-RDPEDYC 3.1.5.3).
	 * Without Soft-Sync this is immediate on tunnel up; with Soft-Sync it
	 * requires the Soft-Sync handshake to complete per direction. */
	if (!multitransport_is_udp_send_migrated(multi))
		return FALSE;
	EnterCriticalSection(&multi->lock);
	rdpUdpTransport* udp = multi->udp;
	LeaveCriticalSection(&multi->lock);
	if (!udp)
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
	HANDLE ev = nullptr;
	if (udp)
		ev = rdpeudp_get_event(udp);
	if (!ev || (ev == INVALID_HANDLE_VALUE))
		ev = multi->stateEvent; /* wakes loop when async setup completes */
	LeaveCriticalSection(&multi->lock);
	return ev;
}

BOOL multitransport_send_autodetect(rdpMultitransport* multi, BOOL isRequest, UINT16 secFlags,
                                     const BYTE* pdu, size_t pduLen)
{
	WINPR_UNUSED(secFlags); /* TLS over UDP provides security; no RDP sec header. */
	if (!multi || !pdu || (pduLen == 0))
		return FALSE;
	EnterCriticalSection(&multi->lock);
	rdpUdpTransport* udp = multi->udp;
	BOOL connected = udp && rdpeudp_is_connected(udp);
	LeaveCriticalSection(&multi->lock);
	if (!connected)
		return FALSE;

	/* MS-RDPEMT 2.2.1.1.1: autodetect PDUs go in Tunnel DATA subheaders,
	 * not in HigherLayerData. */
	const BYTE subType = isRequest ? RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ
	                               : RDP_TUNNEL_SUBHEADER_AUTODETECT_RSP;
	const SSIZE_T rc = rdpeudp_tunnel_send_autodetect(udp, subType, pdu, pduLen);
	if (rc != (SSIZE_T)pduLen)
		return FALSE;
	(void)rdpeudp_check_keepalive(udp, 5000);
	return TRUE;
}

int multitransport_check_fds(rdpMultitransport* multi)
{
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
		BYTE subBuf[512] = { 0 };
		BYTE payloadBuf[65535] = { 0 };
		size_t subLen = 0;
		size_t payloadLen = 0;
		const int fr = rdpeudp_tunnel_recv_full(curUdp, subBuf, sizeof(subBuf), &subLen,
		                                        payloadBuf, sizeof(payloadBuf), &payloadLen,
		                                        0);
		if (fr == 0)
			break; /* timeout, no PDU */
		if (fr < 0)
			break; /* error/incomplete; do not spin, wait for next wakeup */
		/* MS-RDPBCGR 2.2.14: count everything following the Tunnel PDU
		 * header toward TUNNEL bandwidth measurements. */
		if ((subLen + payloadLen) > 0 && multi->rdp && multi->rdp->autodetect)
			autodetect_account_udp_bytes(multi->rdp->autodetect, subLen + payloadLen);

		/* Dispatch each autodetect subheader first (MS-RDPEMT 2.2.1.1.1). */
		if (subLen > 0)
		{
			size_t off = 0;
			while (off < subLen)
			{
				BYTE subType = 0xFF;
				const BYTE* subData = nullptr;
				size_t subDataLen = 0;
				if (!rdpemt_next_subheader(subBuf, subLen, &off, &subType, &subData,
				                           &subDataLen))
				{
					WLog_WARN(TAG, "bad UDP tunnel subheader, ignoring rest");
					udp_trace_bytes("UDP-TUNNEL-SUB", subBuf, subLen);
					break;
				}
				const BOOL isReq =
				    (subType == RDP_TUNNEL_SUBHEADER_AUTODETECT_REQ);
				/* Wrap PDU bytes without taking ownership of the stack buffer. */
				wStream sbuffer = WINPR_C_ARRAY_INIT;
				wStream* s = Stream_StaticConstInit(&sbuffer, subData, subDataLen);
				if (!s)
					continue;
				rdpRdp* rdp2 = multi->rdp;
				state_run_t ares = STATE_RUN_SUCCESS;
				if (rdp2 && rdp2->autodetect)
				{
					if (isReq)
						ares = autodetect_recv_request_packet(
						    rdp2->autodetect, RDP_TRANSPORT_UDP_R, s);
					else
						ares = autodetect_recv_response_packet(
						    rdp2->autodetect, RDP_TRANSPORT_UDP_R, s);
					if (state_run_failed(ares))
						WLog_WARN(TAG, "UDP autodetect dispatch failed");
					else
						dispatched++;
				}
			}
		}

		/* HigherLayerData carries RAW DVC PDUs (may be empty when the PDU
		 * only carried autodetect subheaders). Live servers put one DVC
		 * PDU per Tunnel DATA with no outer envelope (observed: CREATEs);
		 * each PDU is self-delimiting (CREATE: NUL name, DATA_FIRST:
		 * Length, DATA: to end). Gate on recv migration, with observed
		 * migration: without Soft-Sync the server routes DVC over UDP as
		 * soon as the tunnel is up, so latch on the first valid DVC PDU
		 * (loud INFO) instead of dropping peer-sent bytes forever while
		 * TCP stays silent. TCP ordering is preserved because the server
		 * sends this traffic only over UDP. */
		if (payloadLen == 0)
			continue;
		if (!multitransport_is_udp_recv_migrated(multi))
		{
			BOOL softSync = FALSE;
			EnterCriticalSection(&multi->lock);
			softSync = multi->softSyncNegotiated;
			LeaveCriticalSection(&multi->lock);
			if (!softSync)
			{
				size_t probeLen = 0;
				if (rdpeudp_dvc_pdu_length(payloadBuf, payloadLen, &probeLen))
				{
					EnterCriticalSection(&multi->lock);
					multi->udpRecvMigrated = TRUE;
					LeaveCriticalSection(&multi->lock);
					WLog_INFO(TAG,
					          "UDP recv migrated on first DVC PDU (%zu bytes, no Soft-Sync)",
					          payloadLen);
				}
			}
		}
		if (!multitransport_is_udp_recv_migrated(multi))
		{
			WLog_DBG(TAG, "UDP channel data pre-migration, ignoring (%zu bytes)",
			         payloadLen);
			continue;
		}
		/* Split loop: one or more whole DVC PDUs per HigherLayerData,
		 * each fed to the normal ReceiveChannelData path with FIRST|LAST
		 * (same entry TCP chunks use). Anything unparseable stops the
		 * loop loudly instead of being misdelivered. */
		{
			UINT16 drdynvcId = 0;
			if (!multitransport_drdynvc_channel_id(multi->rdp, &drdynvcId))
			{
				WLog_WARN(TAG, "no drdynvc static channel, dropping UDP DVC data");
				continue;
			}
			size_t poff = 0;
			while (poff < payloadLen)
			{
				size_t pduLen = 0;
				if (!rdpeudp_dvc_pdu_length(payloadBuf + poff, payloadLen - poff,
				                            &pduLen) ||
				    (pduLen == 0) || (poff + pduLen > payloadLen))
				{
					WLog_WARN(TAG, "bad UDP DVC PDU at offset %zu/%zu, dropping rest",
					          poff, payloadLen);
					udp_trace_bytes("UDP-TUNNEL-DVC", payloadBuf + poff,
					                payloadLen - poff);
					break;
				}
				UINT16 channelId = drdynvcId;
				const BYTE* chunk = payloadBuf + poff;
				size_t chunkLen = pduLen;
				UINT32 totalSize = (UINT32)pduLen;
				UINT32 flags =
				    (UINT32)(CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST);
				poff += pduLen;
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
					if (!instance->ReceiveChannelData(instance, channelId, chunk,
					                                  chunkLen, flags, totalSize))
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
	}
	}
	/* Keepalive if idle; re-arm readiness (drain + reset + recheck inside). */
	EnterCriticalSection(&multi->lock);
	udp = multi->udp;
	LeaveCriticalSection(&multi->lock);
	if (udp)
	{
		(void)rdpeudp_check_keepalive(udp, 5000);
		rdpeudp_update_event(udp);
	}
	return dispatched;
}

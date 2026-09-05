/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Virtual Channels
 *
 * Copyright 2011 Vic Lee
 * Copyright 2015 Copyright 2015 Thincast Technologies GmbH
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/stream.h>
#include <winpr/wtsapi.h>

#include <freerdp/freerdp.h>
#include <freerdp/constants.h>

#include <freerdp/log.h>
#include <freerdp/svc.h>
#include <freerdp/peer.h>
#include <freerdp/addin.h>

#include <freerdp/client/channels.h>
#include <freerdp/client/drdynvc.h>
#include <freerdp/channels/channels.h>

#include "rdp.h"
#include "client.h"
#include "server.h"
#include "channels.h"
#include "multitransport.h"

#define TAG FREERDP_TAG("core.channels")

/* DVC PDU Cmd values (MS-RDPEDYC 2.2.3): high nibble of first byte. */
#define DVC_CMD_SOFT_SYNC_REQUEST 0x08
#define DVC_CMD_SOFT_SYNC_RESPONSE 0x09

static BOOL channel_is_drdynvc(rdpRdp* rdp, UINT16 channelId)
{
	rdpMcs* mcs = nullptr;
	if (!rdp)
		return FALSE;
	mcs = rdp->mcs;
	if (!mcs || !mcs->channels)
		return FALSE;
	for (UINT32 i = 0; i < mcs->channelCount; i++)
	{
		if ((mcs->channels[i].ChannelId == channelId) &&
		    (strncmp(mcs->channels[i].Name, "drdynvc", 7) == 0))
			return TRUE;
	}
	return FALSE;
}

static BOOL dvc_pdu_cmd(const BYTE* data, size_t size, UINT8* cmdOut)
{
	UINT8 cmd = 0;
	if (!data || (size < 1))
		return FALSE;
	cmd = (UINT8)((data[0] >> 4) & 0x0F);
	if (cmdOut)
		*cmdOut = cmd;
	return (cmd == DVC_CMD_SOFT_SYNC_REQUEST) || (cmd == DVC_CMD_SOFT_SYNC_RESPONSE);
}

BOOL freerdp_channel_send(rdpRdp* rdp, UINT16 channelId, const BYTE* data, size_t size)
{
	size_t left = 0;
	UINT32 flags = 0;
	size_t chunkSize = 0;
	rdpMcs* mcs = nullptr;
	const rdpMcsChannel* channel = nullptr;

	WINPR_ASSERT(rdp);
	WINPR_ASSERT(data || (size == 0));

	mcs = rdp->mcs;
	WINPR_ASSERT(mcs);
	for (UINT32 i = 0; i < mcs->channelCount; i++)
	{
		const rdpMcsChannel* cur = &mcs->channels[i];
		if (cur->ChannelId == channelId)
		{
			channel = cur;
			break;
		}
	}

	if (!channel)
	{
		WLog_ERR(TAG, "freerdp_channel_send: unknown channelId %" PRIu16 "", channelId);
		return FALSE;
	}

	/* Soft-Sync control PDUs MUST stay on TCP (main connection) per
	 * MS-RDPEDYC 3.1.5.3, never on the UDP tunnel. Snoop for migration. */
	UINT8 softSyncCmd = 0;
	const BOOL isSoftSyncPdu =
	    channel_is_drdynvc(rdp, channelId) && dvc_pdu_cmd(data, size, &softSyncCmd);

	/* Pinned UDP path for drdynvc: decide once per PDU to avoid splitting
	 * chunks across TCP/UDP (which breaks ordering/reassembly).
	 * Gated on negotiated migration state (MS-RDPEDYC 3.1.5.3), not mere
	 * tunnel connectivity: with Soft-Sync, migration requires the handshake;
	 * without it, migration is allowed only pre-ACTIVE to avoid reordering
	 * TCP in-flight vs new UDP traffic.
	 * If UDP fails before any chunk was sent, fall back to TCP for the whole
	 * PDU. If it fails mid-PDU, fail without TCP duplicate (caller retries
	 * or disconnects; UDP failure usually means network down anyway). */
	if (!isSoftSyncPdu && rdp->multitransport &&
	    multitransport_is_udp_send_migrated(rdp->multitransport))
	{
		BOOL isDrdynvc = FALSE;
		for (UINT32 i = 0; i < mcs->channelCount; i++)
		{
			if ((mcs->channels[i].ChannelId == channelId) &&
			    (strncmp(mcs->channels[i].Name, "drdynvc", 7) == 0))
			{
				isDrdynvc = TRUE;
				break;
			}
		}
		if (isDrdynvc)
		{
			size_t udpLeft = size;
			const BYTE* udpData = data;
			UINT32 udpFlags = CHANNEL_FLAG_FIRST;
			const UINT32 VCChunkSize =
			    freerdp_settings_get_uint32(rdp->settings, FreeRDP_VCChunkSize);
			const BOOL ServerMode =
			    freerdp_settings_get_bool(rdp->settings, FreeRDP_ServerMode);
			size_t sentChunks = 0;
			BOOL udpFailed = FALSE;
			while (udpLeft > 0)
			{
				size_t cSize = (udpLeft > VCChunkSize) ? VCChunkSize : udpLeft;
				UINT32 cFlags = udpFlags;
				if (cSize == udpLeft)
					cFlags |= CHANNEL_FLAG_LAST;
				if (!ServerMode && (channel->options & CHANNEL_OPTION_SHOW_PROTOCOL))
					cFlags |= CHANNEL_FLAG_SHOW_PROTOCOL;
				if (!multitransport_send_channel_packet(rdp->multitransport, channelId,
				                                        size, cFlags, udpData, cSize))
				{
					udpFailed = TRUE;
					break;
				}
				udpData += cSize;
				udpLeft -= cSize;
				udpFlags = 0;
				sentChunks++;
			}
			if (!udpFailed)
				return TRUE;
			/* Fall back to TCP only if nothing went out on UDP (no duplicates) */
			if (sentChunks == 0)
			{
				WLog_DBG(TAG, "UDP PDU send failed on first chunk, TCP fallback");
			}
			else
			{
				WLog_WARN(TAG, "UDP PDU failed mid-PDU after %zu chunks, no TCP duplicate",
				          sentChunks);
				return FALSE;
			}
		}
	}

	flags = CHANNEL_FLAG_FIRST;
	left = size;

	const UINT32 VCChunkSize = freerdp_settings_get_uint32(rdp->settings, FreeRDP_VCChunkSize);
	const BOOL ServerMode = freerdp_settings_get_bool(rdp->settings, FreeRDP_ServerMode);
	while (left > 0)
	{
		if (left > VCChunkSize)
		{
			chunkSize = VCChunkSize;
		}
		else
		{
			chunkSize = left;
			flags |= CHANNEL_FLAG_LAST;
		}

		if (!ServerMode && (channel->options & CHANNEL_OPTION_SHOW_PROTOCOL))
		{
			flags |= CHANNEL_FLAG_SHOW_PROTOCOL;
		}

		if (!freerdp_channel_send_packet(rdp, channelId, size, flags, data, chunkSize))
			return FALSE;

		data += chunkSize;
		left -= chunkSize;
		flags = 0;
	}

	/* Soft-Sync migration hooks for TCP control PDUs sent above. Only migrate
	 * when UDPFECR is actually offered (honor tunnel lists). */
	if (isSoftSyncPdu && rdp->multitransport)
	{
		const BOOL serverMode =
		    freerdp_settings_get_bool(rdp->settings, FreeRDP_ServerMode);
		if (serverMode && (softSyncCmd == DVC_CMD_SOFT_SYNC_REQUEST) &&
		    rdpeudp_soft_sync_request_offers_udp(data, size))
			multitransport_on_soft_sync_request_sent(rdp->multitransport);
		else if (!serverMode && (softSyncCmd == DVC_CMD_SOFT_SYNC_RESPONSE) &&
		         rdpeudp_soft_sync_response_offers_udp(data, size))
			multitransport_on_soft_sync_response_sent(rdp->multitransport);
	}

	return TRUE;
}

BOOL freerdp_channel_process(freerdp* instance, wStream* s, UINT16 channelId, size_t packetLength)
{
	BOOL rc = FALSE;
	UINT32 length = 0;
	UINT32 flags = 0;
	size_t chunkLength = 0;

	WINPR_ASSERT(instance);

	if (packetLength < 8)
	{
		WLog_ERR(TAG, "Header length %" PRIuz " bytes promised, none available", packetLength);
		return FALSE;
	}
	packetLength -= 8;

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 8))
		return FALSE;

	/* [MS-RDPBCGR] 3.1.5.2.2 Processing of Virtual Channel PDU
	 * chunked data. Length is the total size of the combined data,
	 * chunkLength is the actual data received.
	 * check chunkLength against packetLength, which is the TPKT header size.
	 */
	Stream_Read_UINT32(s, length);
	Stream_Read_UINT32(s, flags);
	chunkLength = Stream_GetRemainingLength(s);
	if (packetLength != chunkLength)
	{
		WLog_ERR(TAG, "Header length %" PRIuz " != actual length %" PRIuz, packetLength,
		         chunkLength);
		return FALSE;
	}

	/* Soft-Sync migration snooping (TCP drdynvc, single-chunk control PDU). */
	if (instance && instance->context && instance->context->rdp &&
	    instance->context->rdp->multitransport &&
	    ((flags & (CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST)) ==
	     (CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST)) &&
	    channel_is_drdynvc(instance->context->rdp, channelId) && (chunkLength >= 1))
	{
		UINT8 rcmd = 0;
		if (dvc_pdu_cmd(Stream_Pointer(s), chunkLength, &rcmd) &&
		    (rcmd == DVC_CMD_SOFT_SYNC_REQUEST) &&
		    rdpeudp_soft_sync_request_offers_udp(Stream_Pointer(s), chunkLength))
			multitransport_on_soft_sync_request_received(
			    instance->context->rdp->multitransport);
	}

	IFCALLRET(instance->ReceiveChannelData, rc, instance, channelId, Stream_Pointer(s), chunkLength,
	          flags, length);
	if (!rc)
	{
		WLog_WARN(TAG, "ReceiveChannelData returned %d", rc);
		return FALSE;
	}

	return Stream_SafeSeek(s, chunkLength);
}

BOOL freerdp_channel_peer_process(freerdp_peer* client, wStream* s, UINT16 channelId)
{
	UINT32 length = 0;
	UINT32 flags = 0;

	WINPR_ASSERT(client);
	WINPR_ASSERT(s);

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 8))
		return FALSE;

	Stream_Read_UINT32(s, length);
	Stream_Read_UINT32(s, flags);
	const size_t chunkLength = Stream_GetRemainingLength(s);
	if (chunkLength > UINT32_MAX)
		return FALSE;

	/* Soft-Sync migration snooping (TCP drdynvc, single-chunk control PDU). */
	if (client && client->context && client->context->rdp &&
	    client->context->rdp->multitransport &&
	    ((flags & (CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST)) ==
	     (CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST)) &&
	    channel_is_drdynvc(client->context->rdp, channelId) && (chunkLength >= 1))
	{
		UINT8 rcmd = 0;
		if (dvc_pdu_cmd(Stream_Pointer(s), chunkLength, &rcmd) &&
		    (rcmd == DVC_CMD_SOFT_SYNC_RESPONSE) &&
		    rdpeudp_soft_sync_response_offers_udp(Stream_Pointer(s), chunkLength))
			multitransport_on_soft_sync_response_received(
			    client->context->rdp->multitransport);
	}

	if (client->VirtualChannelRead)
	{
		int rc = 0;
		BOOL found = FALSE;
		HANDLE hChannel = nullptr;
		rdpContext* context = client->context;
		rdpMcs* mcs = context->rdp->mcs;

		for (UINT32 index = 0; index < mcs->channelCount; index++)
		{
			const rdpMcsChannel* mcsChannel = &(mcs->channels[index]);

			if (mcsChannel->ChannelId == channelId)
			{
				hChannel = (HANDLE)mcsChannel->handle;
				found = TRUE;
				break;
			}
		}

		if (!found)
			return FALSE;

		rc = client->VirtualChannelRead(client, hChannel, Stream_Pointer(s), (UINT32)chunkLength);
		if (rc < 0)
			return FALSE;
	}
	else if (client->ReceiveChannelData)
	{
		BOOL rc = client->ReceiveChannelData(client, channelId, Stream_Pointer(s),
		                                     (UINT32)chunkLength, flags, length);
		if (!rc)
			return FALSE;
	}
	if (!Stream_SafeSeek(s, chunkLength))
	{
		WLog_WARN(TAG, "Short PDU, need %" PRIuz " bytes, got %" PRIuz, chunkLength,
		          Stream_GetRemainingLength(s));
		return FALSE;
	}
	return TRUE;
}

static const WtsApiFunctionTable FreeRDP_WtsApiFunctionTable = {
	0, /* dwVersion */
	0, /* dwFlags */

	FreeRDP_WTSStopRemoteControlSession,        /* StopRemoteControlSession */
	FreeRDP_WTSStartRemoteControlSessionW,      /* StartRemoteControlSessionW */
	FreeRDP_WTSStartRemoteControlSessionA,      /* StartRemoteControlSessionA */
	FreeRDP_WTSConnectSessionW,                 /* ConnectSessionW */
	FreeRDP_WTSConnectSessionA,                 /* ConnectSessionA */
	FreeRDP_WTSEnumerateServersW,               /* EnumerateServersW */
	FreeRDP_WTSEnumerateServersA,               /* EnumerateServersA */
	FreeRDP_WTSOpenServerW,                     /* OpenServerW */
	FreeRDP_WTSOpenServerA,                     /* OpenServerA */
	FreeRDP_WTSOpenServerExW,                   /* OpenServerExW */
	FreeRDP_WTSOpenServerExA,                   /* OpenServerExA */
	FreeRDP_WTSCloseServer,                     /* CloseServer */
	FreeRDP_WTSEnumerateSessionsW,              /* EnumerateSessionsW */
	FreeRDP_WTSEnumerateSessionsA,              /* EnumerateSessionsA */
	FreeRDP_WTSEnumerateSessionsExW,            /* EnumerateSessionsExW */
	FreeRDP_WTSEnumerateSessionsExA,            /* EnumerateSessionsExA */
	FreeRDP_WTSEnumerateProcessesW,             /* EnumerateProcessesW */
	FreeRDP_WTSEnumerateProcessesA,             /* EnumerateProcessesA */
	FreeRDP_WTSTerminateProcess,                /* TerminateProcess */
	FreeRDP_WTSQuerySessionInformationW,        /* QuerySessionInformationW */
	FreeRDP_WTSQuerySessionInformationA,        /* QuerySessionInformationA */
	FreeRDP_WTSQueryUserConfigW,                /* QueryUserConfigW */
	FreeRDP_WTSQueryUserConfigA,                /* QueryUserConfigA */
	FreeRDP_WTSSetUserConfigW,                  /* SetUserConfigW */
	FreeRDP_WTSSetUserConfigA,                  /* SetUserConfigA */
	FreeRDP_WTSSendMessageW,                    /* SendMessageW */
	FreeRDP_WTSSendMessageA,                    /* SendMessageA */
	FreeRDP_WTSDisconnectSession,               /* DisconnectSession */
	FreeRDP_WTSLogoffSession,                   /* LogoffSession */
	FreeRDP_WTSShutdownSystem,                  /* ShutdownSystem */
	FreeRDP_WTSWaitSystemEvent,                 /* WaitSystemEvent */
	FreeRDP_WTSVirtualChannelOpen,              /* VirtualChannelOpen */
	FreeRDP_WTSVirtualChannelOpenEx,            /* VirtualChannelOpenEx */
	FreeRDP_WTSVirtualChannelClose,             /* VirtualChannelClose */
	FreeRDP_WTSVirtualChannelRead,              /* VirtualChannelRead */
	FreeRDP_WTSVirtualChannelWrite,             /* VirtualChannelWrite */
	FreeRDP_WTSVirtualChannelPurgeInput,        /* VirtualChannelPurgeInput */
	FreeRDP_WTSVirtualChannelPurgeOutput,       /* VirtualChannelPurgeOutput */
	FreeRDP_WTSVirtualChannelQuery,             /* VirtualChannelQuery */
	FreeRDP_WTSFreeMemory,                      /* FreeMemory */
	FreeRDP_WTSRegisterSessionNotification,     /* RegisterSessionNotification */
	FreeRDP_WTSUnRegisterSessionNotification,   /* UnRegisterSessionNotification */
	FreeRDP_WTSRegisterSessionNotificationEx,   /* RegisterSessionNotificationEx */
	FreeRDP_WTSUnRegisterSessionNotificationEx, /* UnRegisterSessionNotificationEx */
	FreeRDP_WTSQueryUserToken,                  /* QueryUserToken */
	FreeRDP_WTSFreeMemoryExW,                   /* FreeMemoryExW */
	FreeRDP_WTSFreeMemoryExA,                   /* FreeMemoryExA */
	FreeRDP_WTSEnumerateProcessesExW,           /* EnumerateProcessesExW */
	FreeRDP_WTSEnumerateProcessesExA,           /* EnumerateProcessesExA */
	FreeRDP_WTSEnumerateListenersW,             /* EnumerateListenersW */
	FreeRDP_WTSEnumerateListenersA,             /* EnumerateListenersA */
	FreeRDP_WTSQueryListenerConfigW,            /* QueryListenerConfigW */
	FreeRDP_WTSQueryListenerConfigA,            /* QueryListenerConfigA */
	FreeRDP_WTSCreateListenerW,                 /* CreateListenerW */
	FreeRDP_WTSCreateListenerA,                 /* CreateListenerA */
	FreeRDP_WTSSetListenerSecurityW,            /* SetListenerSecurityW */
	FreeRDP_WTSSetListenerSecurityA,            /* SetListenerSecurityA */
	FreeRDP_WTSGetListenerSecurityW,            /* GetListenerSecurityW */
	FreeRDP_WTSGetListenerSecurityA,            /* GetListenerSecurityA */
	FreeRDP_WTSEnableChildSessions,             /* EnableChildSessions */
	FreeRDP_WTSIsChildSessionsEnabled,          /* IsChildSessionsEnabled */
	FreeRDP_WTSGetChildSessionId,               /* GetChildSessionId */
	FreeRDP_WTSGetActiveConsoleSessionId,       /* GetActiveConsoleSessionId */
	FreeRDP_WTSLogonUser,
	FreeRDP_WTSLogoffUser,
	FreeRDP_WTSStartRemoteControlSessionExW,
	FreeRDP_WTSStartRemoteControlSessionExA
};

const WtsApiFunctionTable* FreeRDP_InitWtsApi(void)
{
	return &FreeRDP_WtsApiFunctionTable;
}

BOOL freerdp_channel_send_packet(rdpRdp* rdp, UINT16 channelId, size_t totalSize, UINT32 flags,
                                 const BYTE* data, size_t chunkSize)
{
	if (totalSize > UINT32_MAX)
		return FALSE;

	/* Only single-chunk (atomic) PDUs may go via UDP here. Multi-chunk PDUs
	 * must go via freerdp_channel_send() whole-PDU pinned path to avoid
	 * splitting chunks across TCP/UDP. Gated on migration state. Soft-Sync
	 * control PDUs always stay on TCP. */
	const BOOL atomic = (totalSize == chunkSize) && (flags & CHANNEL_FLAG_FIRST) &&
	                    (flags & CHANNEL_FLAG_LAST);
	UINT8 ssc = 0;
	const BOOL isSoftSync =
	    atomic && channel_is_drdynvc(rdp, channelId) && dvc_pdu_cmd(data, chunkSize, &ssc);
	if (atomic && !isSoftSync && rdp && rdp->multitransport &&
	    multitransport_is_udp_send_migrated(rdp->multitransport))
	{
		if (multitransport_send_channel_packet(rdp->multitransport, channelId, totalSize,
		                                       flags, data, chunkSize))
			return TRUE;
	}

	UINT16 sec_flags = 0;
	wStream* s = rdp_send_stream_init(rdp, &sec_flags);

	if (!s)
		return FALSE;

	if (!Stream_EnsureRemainingCapacity(s, chunkSize + 8))
	{
		Stream_Release(s);
		return FALSE;
	}

	Stream_Write_UINT32(s, (UINT32)totalSize);
	Stream_Write_UINT32(s, flags);

	Stream_Write(s, data, chunkSize);

	/* WLog_DBG(TAG, "sending data (flags=0x%x size=%d)",  flags, size); */
	return rdp_send(rdp, s, channelId, sec_flags);
}

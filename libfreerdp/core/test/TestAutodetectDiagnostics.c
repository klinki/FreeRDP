/** Bandwidth diagnostic regression coverage. Licensed under Apache-2.0. */
#include <stdio.h>
#include <string.h>

#include <freerdp/freerdp.h>
#include <freerdp/performance.h>
#include "../autodetect.h"
#include "../mcs.h"
#include "../transport.h"

static char records[64][768];
static size_t recordCount;
static BOOL logOverflow;
static rdpNetworkCharacteristicsResult receivedResult;
static UINT32 resultBytes, resultMilliseconds;
static unsigned resultCalls, writes;
static BYTE reply[14];

static BOOL expect(BOOL condition, const char* message)
{
	if (!condition)
		fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

static BOOL capture_log(const wLogMessage* message)
{
	if (!message->TextString || !strstr(message->TextString, "BWDIAG "))
		return TRUE;
	if (recordCount >= ARRAYSIZE(records) || strlen(message->TextString) >= sizeof(records[0]))
	{
		logOverflow = TRUE;
		return FALSE;
	}
	(void)snprintf(records[recordCount++], sizeof(records[0]), "%s", message->TextString);
	return TRUE;
}

static const char* find_record(const char* text)
{
	for (size_t i = 0; i < recordCount; i++)
	{
		if (strstr(records[i], text))
			return records[i];
	}
	return nullptr;
}

static BOOL capture_netchar(WINPR_ATTR_UNUSED rdpAutoDetect* autodetect,
                             WINPR_ATTR_UNUSED RDP_TRANSPORT_TYPE transport,
                             WINPR_ATTR_UNUSED UINT16 sequence,
                             const rdpNetworkCharacteristicsResult* result)
{
	receivedResult = *result;
	return TRUE;
}

static BOOL capture_result(WINPR_ATTR_UNUSED rdpAutoDetect* autodetect,
                            WINPR_ATTR_UNUSED RDP_TRANSPORT_TYPE transport,
                            WINPR_ATTR_UNUSED UINT16 type,
                            WINPR_ATTR_UNUSED UINT16 sequence, UINT32 milliseconds, UINT32 bytes)
{
	resultMilliseconds = milliseconds;
	resultBytes = bytes;
	resultCalls++;
	return TRUE;
}

static int capture_write(WINPR_ATTR_UNUSED rdpTransport* transport, wStream* stream)
{
	const size_t length = Stream_GetPosition(stream);
	if (length < sizeof(reply) || length > INT32_MAX)
		return -1;
	memcpy(reply, Stream_Buffer(stream) + length - sizeof(reply), sizeof(reply));
	writes++;
	return (int)length;
}

static BOOL receive(rdpAutoDetect* autodetect, RDP_TRANSPORT_TYPE transport,
                      const BYTE* data, size_t length, size_t offset, size_t consumed)
{
	wStream buffer = WINPR_C_ARRAY_INIT;
	wStream* stream = Stream_StaticConstInit(&buffer, data, length);
	if (!Stream_SetPosition(stream, offset))
		return FALSE;
	return !state_run_failed(autodetect_recv_request_packet(autodetect, transport, stream)) &&
	       Stream_GetPosition(stream) == offset + consumed;
}

static BOOL test_netchar(rdpAutoDetect* autodetect)
{
	/* A nonzero stream offset and trailing data expose incorrect raw-dump bounds. */
	static const BYTE full[] = { 0xAA, 0xAA, 0xAA, 0xAA,
	                            0x12, 0x00, 0x34, 0x12, 0xC0, 0x08,
	                            0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	                            0x02, 0x00, 0x00, 0x00, 0xA5, 0xA5, 0xA5, 0xA5 };
	if (!expect(receive(autodetect, RDP_TRANSPORT_UDP_R, full, sizeof(full), 4, 18),
	            "full network-characteristics PDU keeps stream consumption unchanged") ||
	    !expect(receivedResult.bandwidth == UINT32_MAX && receivedResult.baseRTT == 1 &&
	                receivedResult.averageRTT == 2,
	            "large unsigned bandwidth reports reach the callback unchanged"))
		return FALSE;
	const char* record = find_record("netchar_rx transport=UDP_R seq=4660 request=0x08c0");
	if (!expect(record && strstr(record, "header_bytes=18 stream_offset=4") &&
	                strstr(record, "bandwidth_present=1 bandwidth_kbps=4294967295") &&
	                strstr(record, "bandwidth_hex=0xffffffff") &&
	                strstr(record, "raw=12003412c00801000000ffffffff02000000"),
	            "raw PDU, units, sequence, offset, transport and unsigned value are correlated"))
		return FALSE;
	const char* raw = strstr(record, "raw=") + 4;
	if (!expect(strlen(raw) == 36 && !strstr(raw, "a5a5") && !strstr(raw, "aaaa"),
	            "raw diagnostics exclude preceding and following data"))
		return FALSE;

	static const BYTE bandwidth[] = { 0x0E, 0x00, 0x35, 0x12, 0x80, 0x08,
	                                 0x80, 0x38, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00 };
	static const BYTE rtt[] = { 0x0E, 0x00, 0x36, 0x12, 0x40, 0x08,
	                           0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
	if (!expect(receive(autodetect, RDP_TRANSPORT_TCP, bandwidth, sizeof(bandwidth), 0,
	                    sizeof(bandwidth)) && receivedResult.bandwidth == 80000 &&
	                find_record("bandwidth_hex=0x00013880 base_rtt_present=0"),
	            "bandwidth-only reports retain their exact decimal-kbit/s interpretation") ||
	    !expect(receive(autodetect, RDP_TRANSPORT_UDP_L, rtt, sizeof(rtt), 0, sizeof(rtt)) &&
	                receivedResult.type == RDP_NETCHAR_RESULT_TYPE_BASE_RTT_AVG_RTT &&
	                find_record("bandwidth_present=0 bandwidth_kbps=0"),
	            "RTT-only reports explicitly identify the absent bandwidth field"))
		return FALSE;

	const size_t before = recordCount;
	wStream buffer = WINPR_C_ARRAY_INIT;
	wStream* truncated = Stream_StaticConstInit(&buffer, bandwidth, sizeof(bandwidth) - 1);
	if (!expect(state_run_failed(autodetect_recv_request_packet(
	                autodetect, RDP_TRANSPORT_TCP, truncated)) && recordCount == before,
	            "truncated messages are rejected without a raw diagnostic dump"))
		return FALSE;
	if (!WLog_SetLogLevel(autodetect->log, WLOG_INFO))
		return FALSE;
	if (!expect(receive(autodetect, RDP_TRANSPORT_UDP_R, full, sizeof(full), 4, 18) &&
	                recordCount == before && receivedResult.bandwidth == UINT32_MAX,
	            "INFO emits no bandwidth diagnostics and preserves protocol behavior"))
		return FALSE;
	return WLog_SetLogLevel(autodetect->log, WLOG_DEBUG);
}

static BOOL test_measurements(rdpAutoDetect* autodetect)
{
	static const BYTE udpStart[] = { 6, 0, 32, 0, 0x14, 0x01 };
	static const BYTE tcpStart[] = { 6, 0, 33, 0, 0x14, 0x00 };
	static const BYTE payload[] = { 8, 0, 34, 0, 2, 0, 4, 0, 0x11, 0x22, 0x33, 0x44 };
	static const BYTE udpStop[] = { 6, 0, 35, 0, 0x29, 0x06 };
	static const BYTE tcpStop[] = { 6, 0, 36, 0, 0x29, 0x04 };
	if (!receive(autodetect, RDP_TRANSPORT_UDP_R, udpStart, sizeof(udpStart), 0, sizeof(udpStart)))
		return FALSE;
	autodetect_account_udp_bytes(autodetect, 1000);
	if (!receive(autodetect, RDP_TRANSPORT_TCP, tcpStart, sizeof(tcpStart), 0, sizeof(tcpStart)) ||
	    !receive(autodetect, RDP_TRANSPORT_UDP_R, payload, sizeof(payload), 0, sizeof(payload)))
		return FALSE;
	/* Control elapsed time without sleeping or changing production timing. */
	autodetect->paddingA[1] = GetTickCount64() - 100;
	if (!expect(receive(autodetect, RDP_TRANSPORT_UDP_R, udpStop, sizeof(udpStop), 0,
	                    sizeof(udpStop)) && resultBytes == 1004 && resultMilliseconds >= 100 &&
	                resultCalls == 1 && writes == 1,
	            "UDP measurement counts and callback behavior are unchanged") ||
	    !expect(find_record("BWDIAG start transport=UDP_R seq=32 request=0x0114") &&
	                find_record("BWDIAG payload transport=UDP_R seq=34 payload_bytes=4 count_before=1000") &&
	                find_record("BWDIAG stop transport=UDP_R seq=35 request=0x0629") &&
	                find_record("bytes=1004 derived_kbps=") && find_record("math_valid=1") &&
	                find_record("BWDIAG result_tx transport=UDP_R seq=35 route=TCP sent=1"),
	            "diagnostics correlate the measurement and its actual TCP fallback route"))
		return FALSE;
	wStream wireBuffer = WINPR_C_ARRAY_INIT;
	wStream* wire = Stream_StaticConstInit(&wireBuffer, reply, sizeof(reply));
	UINT16 sequence, type;
	UINT32 milliseconds, bytes;
	Stream_Seek(wire, 2);
	Stream_Read_UINT16(wire, sequence);
	Stream_Read_UINT16(wire, type);
	Stream_Read_UINT32(wire, milliseconds);
	Stream_Read_UINT32(wire, bytes);
	if (!expect(reply[0] == 14 && reply[1] == 1 && sequence == 35 && type == 0x000B &&
	                milliseconds == resultMilliseconds && bytes == resultBytes,
	            "logged/callback measurement operands equal the actual wire response"))
		return FALSE;
	if (!receive(autodetect, RDP_TRANSPORT_TCP, payload, sizeof(payload), 0, sizeof(payload)))
		return FALSE;
	autodetect->bandwidthMeasureStartTime = GetTickCount64() - 200;
	if (!expect(receive(autodetect, RDP_TRANSPORT_TCP, tcpStop, sizeof(tcpStop), 0,
	                    sizeof(tcpStop)) && resultBytes == 4 && resultMilliseconds >= 200 &&
	                resultCalls == 2 && writes == 2,
	            "interleaved TCP and UDP measurements retain separate byte counts"))
		return FALSE;

	if (!receive(autodetect, RDP_TRANSPORT_UDP_R, udpStart, sizeof(udpStart), 0, sizeof(udpStart)))
		return FALSE;
	autodetect_account_udp_bytes(autodetect, UINT32_MAX);
	autodetect_account_udp_bytes(autodetect, 1);
	autodetect->paddingA[1] = GetTickCount64() + 60000;
	if (!expect(receive(autodetect, RDP_TRANSPORT_UDP_R, udpStop, sizeof(udpStop), 0,
	                    sizeof(udpStop)) && resultBytes == UINT32_MAX && resultMilliseconds == 0,
	            "zero-duration and saturated operands remain unchanged on the wire") ||
	    !expect(find_record("wire_elapsed_ms=0 bytes=4294967295 derived_kbps=0.000 math_valid=0") &&
	                find_record("clock_reversed=1 duration_clamped=0 bytes_at_limit=1"),
	            "diagnostics flag unusable durations and byte-count limits without division by zero"))
		return FALSE;
	return !logOverflow;
}

int TestAutodetectDiagnostics(WINPR_ATTR_UNUSED int argc, WINPR_ATTR_UNUSED char* argv[])
{
	int status = -1;
	freerdp* instance = freerdp_new();
	if (!instance)
		return -1;
	if (!freerdp_context_new(instance))
		goto fail;
	rdpAutoDetect* autodetect = instance->context->autodetect;
	wLogCallbacks callbacks = { .message = capture_log };
	if (!WLog_SetLogLevel(autodetect->log, WLOG_DEBUG) ||
	    !WLog_SetLogAppenderType(autodetect->log, WLOG_APPENDER_CALLBACK) ||
	    !WLog_ConfigureAppender(WLog_GetLogAppender(autodetect->log), "callbacks", &callbacks) ||
	    !WLog_OpenAppender(autodetect->log) ||
	    !freerdp_settings_set_bool(instance->context->settings, FreeRDP_NetworkAutoDetect, TRUE))
		goto fail;
	autodetect->NetworkCharacteristicsResult = capture_netchar;
	autodetect->ClientBandwidthMeasureResult = capture_result;
	instance->context->rdp->mcs->userId = 1001;
	instance->context->rdp->mcs->messageChannelId = 1004;
	rdpTransportIo io = *transport_get_io_callbacks(instance->context->rdp->transport);
	io.WritePdu = capture_write;
	if (!transport_set_io_callbacks(instance->context->rdp->transport, &io))
		goto fail;
	if (!test_netchar(autodetect) || !test_measurements(autodetect))
		goto fail;
	status = 0;
fail:
	if (instance->context)
		freerdp_context_free(instance);
	freerdp_free(instance);
	return status;
}

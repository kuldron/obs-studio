/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "rtmp-stream.h"
#include "rtmp-av1.h"
#include "rtmp-hevc.h"

#include <obs-avc.h>
#include <obs-hevc.h>

#include <jansson.h>

#ifdef _WIN32
#include <util/windows/win-version.h>
#endif

#ifndef SEC_TO_NSEC
#define SEC_TO_NSEC 1000000000ULL
#endif

#ifndef MSEC_TO_USEC
#define MSEC_TO_USEC 1000ULL
#endif

#ifndef MSEC_TO_NSEC
#define MSEC_TO_NSEC 1000000ULL
#endif

/* dynamic bitrate coefficients */
#define DBR_INC_TIMER (4ULL * SEC_TO_NSEC)
#define DBR_TRIGGER_USEC (200ULL * MSEC_TO_USEC)
#define MIN_ESTIMATE_DURATION_MS 1000
#define MAX_ESTIMATE_DURATION_MS 2000

static const char *rtmp_stream_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("RTMPStream");
}

static void log_rtmp(int level, const char *format, va_list args)
{
	if (level > RTMP_LOGWARNING)
		return;

	blogva(LOG_INFO, format, args);
}

static inline size_t num_buffered_packets(struct rtmp_stream *stream);

static inline void free_packets(struct rtmp_stream *stream)
{
	size_t num_packets;

	pthread_mutex_lock(&stream->packets_mutex);

	num_packets = num_buffered_packets(stream);
	if (num_packets)
		info("Freeing %d remaining packets", (int)num_packets);

	while (stream->packets.size) {
		struct encoder_packet packet;
		deque_pop_front(&stream->packets, &packet, sizeof(packet));
		obs_encoder_packet_release(&packet);
	}
	pthread_mutex_unlock(&stream->packets_mutex);
}

static inline bool stopping(struct rtmp_stream *stream)
{
	return os_event_try(stream->stop_event) != EAGAIN;
}

static inline bool connecting(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->connecting);
}

static inline bool active(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->active);
}

static inline bool disconnected(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->disconnected);
}

static void rtmp_stream_destroy(void *data)
{
	struct rtmp_stream *stream = data;

	if (stopping(stream) && !connecting(stream)) {
		pthread_join(stream->send_thread, NULL);

	} else if (connecting(stream) || active(stream)) {
		if (stream->connecting)
			pthread_join(stream->connect_thread, NULL);

		stream->stop_ts = 0;
		os_event_signal(stream->stop_event);

		if (active(stream)) {
			os_sem_post(stream->send_sem);
			obs_output_end_data_capture(stream->output);
			pthread_join(stream->send_thread, NULL);
		}
	}

	RTMP_TLS_Free(&stream->rtmp);
	/* The send thread has been joined above, so it has already run
	 * reconnect_abort(); this only covers a stream destroyed before it ever
	 * ran, where the context is zeroed and both calls are no-ops. */
	RTMP_TLS_Free(&stream->handoff_rtmp);
	free_packets(stream);
	dstr_free(&stream->path);
	dstr_free(&stream->reconnect_url);
	dstr_free(&stream->handoff_path);
	dstr_free(&stream->key);
	dstr_free(&stream->username);
	dstr_free(&stream->password);
	dstr_free(&stream->encoder_name);
	dstr_free(&stream->bind_ip);
	os_event_destroy(stream->stop_event);
	os_sem_destroy(stream->send_sem);
	pthread_mutex_destroy(&stream->packets_mutex);
	deque_free(&stream->packets);
#ifdef TEST_FRAMEDROPS
	deque_free(&stream->droptest_info);
#endif
	deque_free(&stream->dbr_frames);
	da_free(stream->dbr_interpolation_table);
	pthread_mutex_destroy(&stream->dbr_mutex);

	os_event_destroy(stream->buffer_space_available_event);
	os_event_destroy(stream->buffer_has_data_event);
	os_event_destroy(stream->socket_available_event);
	os_event_destroy(stream->send_thread_signaled_exit);
	pthread_mutex_destroy(&stream->write_buf_mutex);

	if (stream->write_buf)
		bfree(stream->write_buf);
	bfree(stream);
}

static void *rtmp_stream_create(obs_data_t *settings, obs_output_t *output)
{
	struct rtmp_stream *stream = bzalloc(sizeof(struct rtmp_stream));
	stream->output = output;
	pthread_mutex_init_value(&stream->packets_mutex);

	RTMP_LogSetCallback(log_rtmp);
	RTMP_LogSetLevel(RTMP_LOGWARNING);

	if (pthread_mutex_init(&stream->packets_mutex, NULL) != 0)
		goto fail;
	if (os_event_init(&stream->stop_event, OS_EVENT_TYPE_MANUAL) != 0)
		goto fail;

	if (pthread_mutex_init(&stream->write_buf_mutex, NULL) != 0) {
		warn("Failed to initialize write buffer mutex");
		goto fail;
	}

	if (pthread_mutex_init(&stream->dbr_mutex, NULL) != 0) {
		warn("Failed to initialize dbr mutex");
		goto fail;
	}

	if (os_event_init(&stream->buffer_space_available_event, OS_EVENT_TYPE_AUTO) != 0) {
		warn("Failed to initialize write buffer event");
		goto fail;
	}
	if (os_event_init(&stream->buffer_has_data_event, OS_EVENT_TYPE_AUTO) != 0) {
		warn("Failed to initialize data buffer event");
		goto fail;
	}
	if (os_event_init(&stream->socket_available_event, OS_EVENT_TYPE_AUTO) != 0) {
		warn("Failed to initialize socket buffer event");
		goto fail;
	}
	if (os_event_init(&stream->send_thread_signaled_exit, OS_EVENT_TYPE_MANUAL) != 0) {
		warn("Failed to initialize socket exit event");
		goto fail;
	}

	UNUSED_PARAMETER(settings);
	return stream;

fail:
	rtmp_stream_destroy(stream);
	return NULL;
}

static void rtmp_stream_stop(void *data, uint64_t ts)
{
	struct rtmp_stream *stream = data;

	if (stopping(stream) && ts != 0)
		return;

	if (connecting(stream))
		pthread_join(stream->connect_thread, NULL);

	stream->stop_ts = ts / 1000ULL;

	if (ts)
		stream->shutdown_timeout_ts = ts + (uint64_t)stream->max_shutdown_time_sec * 1000000000ULL;

	if (active(stream)) {
		os_event_signal(stream->stop_event);
		if (stream->stop_ts == 0)
			os_sem_post(stream->send_sem);
	} else {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_SUCCESS);
	}
}

static inline void set_rtmp_dstr(AVal *val, struct dstr *str)
{
	bool valid = !dstr_is_empty(str);
	val->av_val = valid ? str->array : NULL;
	val->av_len = valid ? (int)str->len : 0;
}

static inline bool get_next_packet(struct rtmp_stream *stream, struct encoder_packet *packet)
{
	bool new_packet = false;

	pthread_mutex_lock(&stream->packets_mutex);
	if (stream->packets.size) {
		deque_pop_front(&stream->packets, packet, sizeof(struct encoder_packet));
		new_packet = true;
	}
	pthread_mutex_unlock(&stream->packets_mutex);

	return new_packet;
}

/* ------------------------------------------------------------------------- */
/* Enhanced RTMP v2 "Reconnect Request", client side.                        */

static const AVal av_onStatus = AVC("onStatus");
static const AVal av_level = AVC("level");
static const AVal av_code = AVC("code");
static const AVal av_tcUrl = AVC("tcUrl");
static const AVal av_description = AVC("description");
static const AVal av_status = AVC("status");
static const AVal av_reconnect_request = AVC("NetConnection.Connect.ReconnectRequest");

/* How many refused requests get logged before the client goes quiet. A server
 * that keeps asking for a target we will not follow must not be able to fill
 * the user's log file. */
#define RECONNECT_REFUSAL_LOG_LIMIT 5

/*
 * Copies an AVal into a NUL-terminated buffer, refusing anything that does not
 * fit. An AVal points straight into the received packet body and carries no
 * terminator, so it must never reach a str*() function; every comparison against
 * one goes through AVMATCH, which is length-bounded.
 */
static bool aval_to_buf(const AVal *val, char *buf, size_t buf_size)
{
	if (val->av_len < 0 || (size_t)val->av_len >= buf_size)
		return false;
	if (val->av_len > 0) {
		if (!val->av_val)
			return false;
		memcpy(buf, val->av_val, (size_t)val->av_len);
	}
	buf[val->av_len] = '\0';
	return true;
}

/* Safe arguments for a "%.*s" of a wire-supplied AVal: never a NULL pointer,
 * and never more than `max` characters of a string whose length the server
 * chose. */
#define AVAL_FMT(v, max) ((v).av_len > (max) ? (max) : (v).av_len), ((v).av_val ? (v).av_val : "")

/*
 * Logs a refused reconnect request, up to a limit.
 *
 * How many of these arrive is the server's decision; how large the user's log
 * file gets should not be. Every refusal path goes through here so that none of
 * them can be used as an unbounded write.
 */
static void reconnect_refuse(struct rtmp_stream *stream, const char *target, const char *reason)
{
	if (stream->reconnect_refusals >= RECONNECT_REFUSAL_LOG_LIMIT)
		return;

	stream->reconnect_refusals++;
	if (target && target[0])
		warn("Refused a reconnect request to '%s': %s", target, reason);
	else
		warn("Refused a reconnect request: %s", reason);

	if (stream->reconnect_refusals == RECONNECT_REFUSAL_LOG_LIMIT)
		warn("Further refused reconnect requests will not be logged");
}

static void handle_reconnect_request(struct rtmp_stream *stream, AMFObject *info)
{
	AVal level = {0};
	AVal code = {0};
	AVal tc_url = {0};
	AVal description = {0};
	char requested[RTMP_RECONNECT_MAX_URL];
	char resolved[RTMP_RECONNECT_MAX_URL];

	AMFProp_GetString(AMF_GetProp(info, &av_code, -1), &code);

	/*
	 * `code` is the discriminator, and checking only `level` is not a
	 * shortcut for it: every RTMP server sends NetStream.Publish.Start with
	 * level "status" the instant publishing begins, so a level-only test
	 * fires against ordinary servers, once per publish and then once per
	 * keyframe forever after.
	 */
	if (!AVMATCH(&code, &av_reconnect_request))
		return;

	AMFProp_GetString(AMF_GetProp(info, &av_level, -1), &level);
	if (!AVMATCH(&level, &av_status)) {
		char note[128];
		snprintf(note, sizeof(note), "level is '%.*s' and the specification requires \"status\"",
			 AVAL_FMT(level, 32));
		reconnect_refuse(stream, NULL, note);
		return;
	}

	if (stream->handoff_state != HANDOFF_IDLE || !dstr_is_empty(&stream->reconnect_url)) {
		debug("Ignoring a reconnect request: one is already in progress");
		return;
	}

	if (stream->reconnect_count >= stream->reconnect_limit) {
		if (stream->reconnect_count == stream->reconnect_limit) {
			stream->reconnect_count++;
			warn("Refusing further reconnect requests: this session has already "
			     "followed %d, which is the configured limit",
			     stream->reconnect_limit);
		}
		return;
	}

	AMFProp_GetString(AMF_GetProp(info, &av_tcUrl, -1), &tc_url);
	if (!aval_to_buf(&tc_url, requested, sizeof(requested))) {
		char note[128];
		snprintf(note, sizeof(note), "the tcUrl is %d bytes, longer than the %d this client will accept",
			 tc_url.av_len, (int)sizeof(requested) - 1);
		reconnect_refuse(stream, NULL, note);
		return;
	}

	enum rtmp_reconnect_verdict verdict =
		rtmp_reconnect_resolve(stream->path.array, requested, resolved, sizeof(resolved));

	if (verdict != RTMP_RECONNECT_ACCEPT) {
		reconnect_refuse(stream, requested, rtmp_reconnect_verdict_str(verdict));
		return;
	}

	stream->reconnect_count++;
	dstr_copy(&stream->reconnect_url, resolved);

	/* Logged only once the request has been accepted, so the number of these
	 * lines is bounded by the per-session limit rather than by how often the
	 * server feels like asking. */
	AMFProp_GetString(AMF_GetProp(info, &av_description, -1), &description);
	if (description.av_len > 0)
		info("Server's reason for the reconnect: %.*s", AVAL_FMT(description, 200));

	info("Reconnect requested: moving from %s to %s at the next keyframe (%d of %d allowed this session)",
	     stream->path.array, resolved, stream->reconnect_count, stream->reconnect_limit);
}

static void process_invoke(struct rtmp_stream *stream, const RTMPPacket *packet)
{
	const char *body = packet->m_body;
	unsigned int size = packet->m_nBodySize;
	AMFObject invoke;
	AMFObject status_info;
	AVal method = {0};

	/* An AMF3 command message prefixes the AMF0 payload with one byte. */
	if (packet->m_packetType == RTMP_PACKET_TYPE_FLEX_MESSAGE) {
		if (size < 1)
			return;
		body++;
		size--;
	}

	/* Every command message starts with its name as an AMF0 string. */
	if (size < 1 || size > INT_MAX || body[0] != AMF_STRING)
		return;

	int decoded = AMF_Decode(&invoke, body, (int)size, FALSE);

	/* AMF_Decode populates the object as it goes and can still fail, so the
	 * reset below is unconditional. */
	if (decoded >= 0) {
		AMFProp_GetString(AMF_GetProp(&invoke, NULL, 0), &method);

		/* onStatus is (name, transaction id, null command object, info). */
		if (AVMATCH(&method, &av_onStatus) && AMF_CountProp(&invoke) > 3) {
			AMFProp_GetObject(AMF_GetProp(&invoke, NULL, 3), &status_info);
			handle_reconnect_request(stream, &status_info);
		}
	}

	AMF_Reset(&invoke);
}

static bool process_recv_data(struct rtmp_stream *stream, size_t size)
{
	UNUSED_PARAMETER(size);

	RTMP *rtmp = &stream->rtmp;
	RTMPPacket packet = {0};

	if (!RTMP_ReadPacket(rtmp, &packet)) {
#ifdef _WIN32
		int error = WSAGetLastError();
#else
		int error = errno;
#endif
		do_log(LOG_ERROR, "RTMP_ReadPacket error: %d", error);
		return false;
	}

	if (packet.m_body) {
		/*
		 * Only a fully reassembled message can be parsed. Until
		 * RTMPPacket_IsReady, m_nBodySize is the length the sender
		 * announced while only m_nBytesRead of the buffer have been
		 * filled, so decoding it would read uninitialized heap.
		 */
		if (RTMPPacket_IsReady(&packet) && (packet.m_packetType == RTMP_PACKET_TYPE_INVOKE ||
						    packet.m_packetType == RTMP_PACKET_TYPE_FLEX_MESSAGE))
			process_invoke(stream, &packet);

		RTMPPacket_Free(&packet);
	}
	return true;
}

#ifdef TEST_FRAMEDROPS
static void droptest_cap_data_rate(struct rtmp_stream *stream, size_t size)
{
	uint64_t ts = os_gettime_ns();
	struct droptest_info info;

#if defined(_WIN32) && defined(TEST_FRAMEDROPS_WITH_BITRATE_SHORTCUTS)
	uint64_t check_elapsed = ts - stream->droptest_last_key_check;

	if (check_elapsed > (200ULL * MSEC_TO_NSEC)) {
		size_t bitrate = 0;

		stream->droptest_last_key_check = ts;

		if (GetAsyncKeyState(VK_NUMPAD0) & 0x8000) {
			stream->droptest_max = 0;
		} else if (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) {
			bitrate = 1000;
		} else if (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) {
			bitrate = 2000;
		} else if (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) {
			bitrate = 3000;
		} else if (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) {
			bitrate = 4000;
		} else if (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) {
			bitrate = 5000;
		} else if (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) {
			bitrate = 6000;
		} else if (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) {
			bitrate = 7000;
		} else if (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) {
			bitrate = 8000;
		} else if (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) {
			bitrate = 9000;
		}

		if (bitrate) {
			stream->droptest_max = (bitrate * 1000 / 8);
		}
	}
	if (!stream->droptest_max) {
		return;
	}
#else
	if (!stream->droptest_max) {
		stream->droptest_max = DROPTEST_MAX_BYTES;
	}
#endif

	info.ts = ts;
	info.size = size;

	deque_push_back(&stream->droptest_info, &info, sizeof(info));
	stream->droptest_size += size;

	if (stream->droptest_info.size) {
		deque_peek_front(&stream->droptest_info, &info, sizeof(info));

		if (stream->droptest_size > stream->droptest_max) {
			uint64_t elapsed = ts - info.ts;

			if (elapsed < 1000000000ULL) {
				elapsed = 1000000000ULL - elapsed;
				os_sleepto_ns(ts + elapsed);
			}

			while (stream->droptest_size > stream->droptest_max) {
				deque_pop_front(&stream->droptest_info, &info, sizeof(info));
				stream->droptest_size -= info.size;
			}
		}
	}
}
#endif

#ifdef _WIN32
static int socket_queue_data(RTMPSockBuf *sb, const char *data, int len, void *arg)
{
	UNUSED_PARAMETER(sb);

	struct rtmp_stream *stream = arg;

retry_send:

	if (!RTMP_IsConnected(&stream->rtmp))
		return 0;

	pthread_mutex_lock(&stream->write_buf_mutex);

	if (stream->write_buf_len + len > stream->write_buf_size) {

		pthread_mutex_unlock(&stream->write_buf_mutex);

		if (os_event_wait(stream->buffer_space_available_event)) {
			return 0;
		}

		goto retry_send;
	}

	memcpy(stream->write_buf + stream->write_buf_len, data, len);
	stream->write_buf_len += len;

	pthread_mutex_unlock(&stream->write_buf_mutex);

	os_event_signal(stream->buffer_has_data_event);

	return len;
}
#endif // _WIN32

static int handle_socket_read(struct rtmp_stream *stream)
{
	int ret = 0;
	int recv_size = 0;
	if (!stream->new_socket_loop) {
#ifdef _WIN32
		ret = ioctlsocket(stream->rtmp.m_sb.sb_socket, FIONREAD, (u_long *)&recv_size);
#else
		ret = ioctl(stream->rtmp.m_sb.sb_socket, FIONREAD, &recv_size);
#endif

		if (ret >= 0 && recv_size > 0) {
			if (!process_recv_data(stream, (size_t)recv_size))
				return -1;
		}
	}
	return 0;
}

static int send_packet(struct rtmp_stream *stream, struct encoder_packet *packet, bool is_header)
{
	uint8_t *data;
	size_t size;
	int ret = 0;

	if (handle_socket_read(stream))
		return -1;

	flv_packet_mux(packet, is_header ? 0 : stream->start_dts_offset, &data, &size, is_header);

#ifdef TEST_FRAMEDROPS
	droptest_cap_data_rate(stream, size);
#endif

	ret = RTMP_Write(&stream->rtmp, (char *)data, (int)size, 0);
	bfree(data);

	if (is_header)
		bfree(packet->data);
	else
		obs_encoder_packet_release(packet);

	stream->total_bytes_sent += size;
	return ret;
}

static int send_packet_ex(struct rtmp_stream *stream, struct encoder_packet *packet, bool is_header, bool is_footer,
			  size_t idx)
{
	uint8_t *data;
	size_t size = 0;
	int ret = 0;

	if (handle_socket_read(stream))
		return -1;

	if (is_header) {
		flv_packet_start(packet, stream->video_codec[idx], &data, &size, idx);
	} else if (is_footer) {
		flv_packet_end(packet, stream->video_codec[idx], &data, &size, idx);
	} else {
		flv_packet_frames(packet, stream->video_codec[idx], stream->start_dts_offset, &data, &size, idx);
	}

#ifdef TEST_FRAMEDROPS
	droptest_cap_data_rate(stream, size);
#endif

	ret = RTMP_Write(&stream->rtmp, (char *)data, (int)size, 0);
	bfree(data);

	if (is_header || is_footer) // manually created packets
		bfree(packet->data);
	else
		obs_encoder_packet_release(packet);

	stream->total_bytes_sent += size;
	return ret;
}

static int send_audio_packet_ex(struct rtmp_stream *stream, struct encoder_packet *packet, bool is_header, size_t idx)
{
	uint8_t *data;
	size_t size = 0;
	int ret = 0;

	if (handle_socket_read(stream))
		return -1;

	if (is_header) {
		flv_packet_audio_start(packet, stream->audio_codec[idx], &data, &size, idx);
	} else {
		flv_packet_audio_frames(packet, stream->audio_codec[idx], stream->start_dts_offset, &data, &size, idx);
	}

	ret = RTMP_Write(&stream->rtmp, (char *)data, (int)size, 0);
	bfree(data);

	if (is_header)
		bfree(packet->data);
	else
		obs_encoder_packet_release(packet);

	return ret;
}

static inline bool send_headers(struct rtmp_stream *stream);
static inline bool send_footers(struct rtmp_stream *stream);
static void reconnect_pump(struct rtmp_stream *stream, const struct encoder_packet *packet);
static void reconnect_abort(struct rtmp_stream *stream);

static inline bool can_shutdown_stream(struct rtmp_stream *stream, struct encoder_packet *packet)
{
	uint64_t cur_time = os_gettime_ns();
	bool timeout = cur_time >= stream->shutdown_timeout_ts;

	if (timeout)
		info("Stream shutdown timeout reached (%d second(s))", stream->max_shutdown_time_sec);

	return timeout || packet->sys_dts_usec >= (int64_t)stream->stop_ts;
}

static void set_output_error(struct rtmp_stream *stream)
{
	const char *msg = NULL;
#ifdef _WIN32
	switch (stream->rtmp.last_error_code) {
	case WSAETIMEDOUT:
		msg = obs_module_text("ConnectionTimedOut");
		break;
	case WSAEACCES:
		msg = obs_module_text("PermissionDenied");
		break;
	case WSAECONNABORTED:
		msg = obs_module_text("ConnectionAborted");
		break;
	case WSAECONNRESET:
		msg = obs_module_text("ConnectionReset");
		break;
	case WSAHOST_NOT_FOUND:
		msg = obs_module_text("HostNotFound");
		break;
	case WSANO_DATA:
		msg = obs_module_text("NoData");
		break;
	case WSAEADDRNOTAVAIL:
		msg = obs_module_text("AddressNotAvailable");
		break;
	case WSAEINVAL:
		msg = obs_module_text("InvalidParameter");
		break;
	case WSAEHOSTUNREACH:
		msg = obs_module_text("NoRoute");
		break;
	}
#else
	switch (stream->rtmp.last_error_code) {
	case ETIMEDOUT:
		msg = obs_module_text("ConnectionTimedOut");
		break;
	case EACCES:
		msg = obs_module_text("PermissionDenied");
		break;
	case ECONNABORTED:
		msg = obs_module_text("ConnectionAborted");
		break;
	case ECONNRESET:
		msg = obs_module_text("ConnectionReset");
		break;
	case HOST_NOT_FOUND:
		msg = obs_module_text("HostNotFound");
		break;
	case NO_DATA:
		msg = obs_module_text("NoData");
		break;
	case EADDRNOTAVAIL:
		msg = obs_module_text("AddressNotAvailable");
		break;
	case EINVAL:
		msg = obs_module_text("InvalidParameter");
		break;
	case EHOSTUNREACH:
		msg = obs_module_text("NoRoute");
		break;
	}
#endif

	// non platform-specific errors
	if (!msg) {
		switch (stream->rtmp.last_error_code) {
		case -0x2700:
			msg = obs_module_text("SSLCertVerifyFailed");
			break;
		case -0x7680:
			msg = "Failed to load root certificates for a secure TLS connection."
#if defined(__linux__)
			      " Check you have an up to date root certificate bundle in /etc/ssl/certs."
#endif
				;
			break;
		}
	}

	if (msg)
		obs_output_set_last_error(stream->output, msg);
}

static void dbr_add_frame(struct rtmp_stream *stream, struct dbr_frame *back)
{
	struct dbr_frame front;
	uint64_t dur;

	deque_push_back(&stream->dbr_frames, back, sizeof(*back));
	deque_peek_front(&stream->dbr_frames, &front, sizeof(front));

	stream->dbr_data_size += back->size;

	dur = (back->send_end - front.send_beg) / 1000000;

	if (dur >= MAX_ESTIMATE_DURATION_MS) {
		stream->dbr_data_size -= front.size;
		deque_pop_front(&stream->dbr_frames, NULL, sizeof(front));
	}

	stream->dbr_est_bitrate = (dur >= MIN_ESTIMATE_DURATION_MS) ? (long)(stream->dbr_data_size * 1000 / dur) : 0;
	stream->dbr_est_bitrate *= 8;
	stream->dbr_est_bitrate /= 1000;

	if (stream->dbr_est_bitrate) {
		stream->dbr_est_bitrate -= stream->audio_bitrate;
		if (stream->dbr_est_bitrate < 50)
			stream->dbr_est_bitrate = 50;
	}
}

static void dbr_set_bitrate(struct rtmp_stream *stream);

#ifdef _WIN32
#define socklen_t int

static void log_sndbuf_size(struct rtmp_stream *stream)
{
	int cur_sendbuf_size;
	socklen_t int_size = sizeof(int);

	if (!getsockopt(stream->rtmp.m_sb.sb_socket, SOL_SOCKET, SO_SNDBUF, (char *)&cur_sendbuf_size, &int_size)) {
		info("Socket send buffer is %d bytes", cur_sendbuf_size);
	}
}
#endif

static void *send_thread(void *data)
{
	struct rtmp_stream *stream = data;

	os_set_thread_name("rtmp-stream: send_thread");

#ifdef _WIN32
	log_sndbuf_size(stream);
#endif

	while (os_sem_wait(stream->send_sem) == 0) {
		struct encoder_packet packet;
		struct dbr_frame dbr_frame;

		if (stopping(stream) && stream->stop_ts == 0) {
			break;
		}

		if (!get_next_packet(stream, &packet))
			continue;

		if (stopping(stream)) {
			if (can_shutdown_stream(stream, &packet)) {
				obs_encoder_packet_release(&packet);
				break;
			}
		}

		/*
		 * Enhanced RTMP v2 handoff. This sits between taking ownership of
		 * the packet and sending it, so that when the cut-over happens the
		 * packet in hand, the keyframe that triggered it, is simply
		 * sent on the new connection instead of the old one. Nothing about
		 * packet ownership changes: the packet is already a local, popped
		 * from the deque above, and it goes out exactly once either way.
		 */
		reconnect_pump(stream, &packet);

		if (!stream->sent_headers) {
			if (!send_headers(stream)) {
				os_atomic_set_bool(&stream->disconnected, true);
				break;
			}
		}

		if (stream->dbr_enabled) {
			dbr_frame.send_beg = os_gettime_ns();
			dbr_frame.size = packet.size;
		}

		int sent;
		if (packet.type == OBS_ENCODER_VIDEO &&
		    (stream->video_codec[packet.track_idx] != CODEC_H264 ||
		     (stream->video_codec[packet.track_idx] == CODEC_H264 && packet.track_idx != 0))) {
			sent = send_packet_ex(stream, &packet, false, false, packet.track_idx);
		} else if (packet.type == OBS_ENCODER_AUDIO && packet.track_idx != 0) {
			sent = send_audio_packet_ex(stream, &packet, false, packet.track_idx);
		} else {
			sent = send_packet(stream, &packet, false);
		}

		if (sent < 0) {
			os_atomic_set_bool(&stream->disconnected, true);
			break;
		}

		if (stream->dbr_enabled) {
			dbr_frame.send_end = os_gettime_ns();

			pthread_mutex_lock(&stream->dbr_mutex);
			dbr_add_frame(stream, &dbr_frame);
			pthread_mutex_unlock(&stream->dbr_mutex);
		}
	}

	/* Nothing below may run while the handoff thread is still writing into
	 * the stream, and any connection it opened has to be closed rather than
	 * leaked. This is the only place the send thread leaves the loop. */
	reconnect_abort(stream);

	bool encode_error = os_atomic_load_bool(&stream->encode_error);

	if (disconnected(stream)) {
		info("Disconnected from %s", stream->path.array);
	} else if (encode_error) {
		info("Encoder error, disconnecting");
		send_footers(stream); // Y2023 spec
	} else {
		info("User stopped the stream");
		send_footers(stream); // Y2023 spec
	}

#ifdef _WIN32
	log_sndbuf_size(stream);
#endif

	if (stream->new_socket_loop) {
		os_event_signal(stream->send_thread_signaled_exit);
		os_event_signal(stream->buffer_has_data_event);
		pthread_join(stream->socket_thread, NULL);
		stream->socket_thread_active = false;
		stream->rtmp.m_bCustomSend = false;
	}

	set_output_error(stream);

	RTMP_Close(&stream->rtmp);

	/* reset bitrate on stop */
	if (stream->dbr_enabled) {
		if (stream->dbr_cur_bitrate != stream->dbr_orig_bitrate) {
			stream->dbr_cur_bitrate = stream->dbr_orig_bitrate;
			dbr_set_bitrate(stream);
		}
	}

	if (!stopping(stream)) {
		pthread_detach(stream->send_thread);
		obs_output_signal_stop(stream->output, OBS_OUTPUT_DISCONNECTED);
	} else if (encode_error) {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_ENCODE_ERROR);
	} else {
		obs_output_end_data_capture(stream->output);
	}

	free_packets(stream);
	os_event_reset(stream->stop_event);
	os_atomic_set_bool(&stream->active, false);
	stream->sent_headers = false;

	return NULL;
}

/* `rtmp` is a parameter rather than stream->rtmp because a server-directed
 * handoff sends the metadata on the incoming connection while the outgoing one
 * is still the active. */
static bool send_meta_data(struct rtmp_stream *stream, RTMP *rtmp)
{
	uint8_t *meta_data;
	size_t meta_data_size;
	bool success = true;

	flv_meta_data(stream->output, &meta_data, &meta_data_size, false);
	success = RTMP_Write(rtmp, (char *)meta_data, (int)meta_data_size, 0) >= 0;
	bfree(meta_data);

	return success;
}

static bool send_audio_header(struct rtmp_stream *stream, size_t idx)
{
	obs_output_t *context = stream->output;
	obs_encoder_t *aencoder = obs_output_get_audio_encoder(context, idx);
	uint8_t *header;

	struct encoder_packet packet = {.type = OBS_ENCODER_AUDIO, .timebase_den = 1};

	if (!aencoder) {
		return true;
	}

	if (obs_encoder_get_extra_data(aencoder, &header, &packet.size)) {
		packet.data = bmemdup(header, packet.size);
		if (idx == 0) {
			return send_packet(stream, &packet, true) >= 0;
		} else {
			return send_audio_packet_ex(stream, &packet, true, idx) >= 0;
		}
	}
	return false;
}

static bool send_video_header(struct rtmp_stream *stream, size_t idx)
{
	obs_output_t *context = stream->output;
	obs_encoder_t *vencoder = obs_output_get_video_encoder2(context, idx);
	uint8_t *header;
	size_t size;

	struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO, .timebase_den = 1, .keyframe = true};

	if (!vencoder)
		return false;

	if (!obs_encoder_get_extra_data(vencoder, &header, &size))
		return false;

	switch (stream->video_codec[idx]) {
	case CODEC_NONE:
		do_log(LOG_ERROR, "Codec not initialized for track %zu while sending header", idx);
		return false;

	case CODEC_H264:
		packet.size = obs_parse_avc_header(&packet.data, header, size);
		// Always send H.264 on track 0 as old style for compatibility.
		if (idx == 0) {
			return send_packet(stream, &packet, true) >= 0;
		} else {
			return send_packet_ex(stream, &packet, true, false, idx) >= 0;
		}
	case CODEC_HEVC:
#ifdef ENABLE_HEVC
		packet.size = obs_parse_hevc_header(&packet.data, header, size);
		return send_packet_ex(stream, &packet, true, false, idx) >= 0;
#else
		return false;
#endif
	case CODEC_AV1:
		packet.size = obs_parse_av1_header(&packet.data, header, size);
		return send_packet_ex(stream, &packet, true, false, idx) >= 0;
	}

	return false;
}

// only returns false if there's an error, not if no metadata needs to be sent
static bool send_video_metadata(struct rtmp_stream *stream, size_t idx)
{
	// send metadata only if HDR
	obs_encoder_t *encoder = obs_output_get_video_encoder2(stream->output, idx);
	if (!encoder)
		return false;

	video_t *video = obs_encoder_video(encoder);
	if (!video)
		return false;

	const struct video_output_info *info = video_output_get_info(video);
	enum video_colorspace colorspace = info->colorspace;
	if (!(colorspace == VIDEO_CS_2100_PQ || colorspace == VIDEO_CS_2100_HLG))
		return true;

	if (handle_socket_read(stream))
		return false;

	// Y2023 spec
	if (stream->video_codec[idx] != CODEC_H264) {
		uint8_t *data;
		size_t size;

		video_t *video = obs_get_video();
		const struct video_output_info *info = video_output_get_info(video);
		enum video_format format = info->format;
		enum video_colorspace colorspace = info->colorspace;

		int bits_per_raw_sample;
		switch (format) {
		case VIDEO_FORMAT_I010:
		case VIDEO_FORMAT_P010:
		case VIDEO_FORMAT_I210:
			bits_per_raw_sample = 10;
			break;
		case VIDEO_FORMAT_I412:
		case VIDEO_FORMAT_YA2L:
			bits_per_raw_sample = 12;
			break;
		default:
			bits_per_raw_sample = 8;
		}

		int pri = 0, trc = 0, spc = 0;
		switch (colorspace) {
		case VIDEO_CS_601:
			pri = OBSCOL_PRI_SMPTE170M;
			trc = OBSCOL_PRI_SMPTE170M;
			spc = OBSCOL_PRI_SMPTE170M;
			break;
		case VIDEO_CS_DEFAULT:
		case VIDEO_CS_709:
			pri = OBSCOL_PRI_BT709;
			trc = OBSCOL_PRI_BT709;
			spc = OBSCOL_PRI_BT709;
			break;
		case VIDEO_CS_SRGB:
			pri = OBSCOL_PRI_BT709;
			trc = OBSCOL_TRC_IEC61966_2_1;
			spc = OBSCOL_PRI_BT709;
			break;
		case VIDEO_CS_2100_PQ:
			pri = OBSCOL_PRI_BT2020;
			trc = OBSCOL_TRC_SMPTE2084;
			spc = OBSCOL_SPC_BT2020_NCL;
			break;
		case VIDEO_CS_2100_HLG:
			pri = OBSCOL_PRI_BT2020;
			trc = OBSCOL_TRC_ARIB_STD_B67;
			spc = OBSCOL_SPC_BT2020_NCL;
		}

		int max_luminance = 0;
		if (trc == OBSCOL_TRC_ARIB_STD_B67)
			max_luminance = 1000;
		else if (trc == OBSCOL_TRC_SMPTE2084)
			max_luminance = (int)obs_get_video_hdr_nominal_peak_level();

		flv_packet_metadata(stream->video_codec[idx], &data, &size, bits_per_raw_sample, pri, trc, spc, 0,
				    max_luminance, idx);

		int ret = RTMP_Write(&stream->rtmp, (char *)data, (int)size, 0);
		bfree(data);

		stream->total_bytes_sent += size;
		return ret >= 0;
	}
	// legacy
	return true;
}

static bool send_video_footer(struct rtmp_stream *stream, size_t idx)
{
	struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO, .timebase_den = 1, .keyframe = false};
	packet.size = 0;

	return send_packet_ex(stream, &packet, false, true, idx) >= 0;
}

static inline bool send_headers(struct rtmp_stream *stream)
{
	stream->sent_headers = true;

	for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++) {
		obs_encoder_t *enc = obs_output_get_audio_encoder(stream->output, i);
		if (!enc)
			continue;

		if (!send_audio_header(stream, i))
			return false;
	}

	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(stream->output, i);
		if (!enc)
			continue;

		if (!send_video_metadata(stream, i) || !send_video_header(stream, i))
			return false;
	}

	return true;
}

static inline bool send_footers(struct rtmp_stream *stream)
{
	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		obs_encoder_t *encoder = obs_output_get_video_encoder2(stream->output, i);
		if (!encoder)
			continue;

		if (i == 0 && stream->video_codec[i] == CODEC_H264)
			continue;

		if (!send_video_footer(stream, i))
			return false;
	}

	return true;
}

static inline bool reset_semaphore(struct rtmp_stream *stream)
{
	os_sem_destroy(stream->send_sem);
	return os_sem_init(&stream->send_sem, 0) == 0;
}

static int init_send(struct rtmp_stream *stream)
{
	int ret;
	obs_output_t *context = stream->output;

	reset_semaphore(stream);

	ret = pthread_create(&stream->send_thread, NULL, send_thread, stream);
	if (ret != 0) {
		RTMP_Close(&stream->rtmp);
		warn("Failed to create send thread");
		return OBS_OUTPUT_ERROR;
	}

	if (stream->new_socket_loop) {
		int one = 1;
#ifdef _WIN32
		if (ioctlsocket(stream->rtmp.m_sb.sb_socket, FIONBIO, &one)) {
			stream->rtmp.last_error_code = WSAGetLastError();
#else
		if (ioctl(stream->rtmp.m_sb.sb_socket, FIONBIO, &one)) {
			stream->rtmp.last_error_code = errno;
#endif
			warn("Failed to set non-blocking socket");
			return OBS_OUTPUT_ERROR;
		}

		os_event_reset(stream->send_thread_signaled_exit);

		info("New socket loop enabled by user");
		if (stream->low_latency_mode)
			info("Low latency mode enabled by user");

		if (stream->write_buf)
			bfree(stream->write_buf);

		int total_bitrate = 0;

		for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
			obs_encoder_t *vencoder = obs_output_get_video_encoder2(context, i);
			if (!vencoder)
				continue;

			obs_data_t *params = obs_encoder_get_settings(vencoder);
			if (params) {
				int bitrate = obs_data_get_int(params, "bitrate");
				if (!bitrate) {
					warn("Video encoder didn't return a "
					     "valid bitrate, new network "
					     "code may function poorly. "
					     "Low latency mode disabled.");
					stream->low_latency_mode = false;
					bitrate = 10000;
				}
				total_bitrate += bitrate;
				obs_data_release(params);
			}
		}

		for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++) {
			obs_encoder_t *aencoder = obs_output_get_audio_encoder(context, 0);
			if (!aencoder)
				continue;

			obs_data_t *params = obs_encoder_get_settings(aencoder);
			if (params) {
				int bitrate = obs_data_get_int(params, "bitrate");
				if (!bitrate)
					bitrate = 160;
				total_bitrate += bitrate;
				obs_data_release(params);
			}
		}

		// to bytes/sec
		int ideal_buffer_size = total_bitrate * 128;

		if (ideal_buffer_size < 131072)
			ideal_buffer_size = 131072;

		stream->write_buf_size = ideal_buffer_size;
		stream->write_buf = bmalloc(ideal_buffer_size);

#ifndef _WIN32
		warn("New socket loop not supported on this platform");
		return OBS_OUTPUT_ERROR;
#else
		ret = pthread_create(&stream->socket_thread, NULL, socket_thread_windows, stream);

		if (ret != 0) {
			RTMP_Close(&stream->rtmp);
			warn("Failed to create socket thread");
			return OBS_OUTPUT_ERROR;
		}

		stream->socket_thread_active = true;
		stream->rtmp.m_bCustomSend = true;
		stream->rtmp.m_customSendFunc = socket_queue_data;
		stream->rtmp.m_customSendParam = stream;
#endif
	}

	os_atomic_set_bool(&stream->active, true);

	if (!send_meta_data(stream, &stream->rtmp)) {
		warn("Disconnected while attempting to send metadata");
		set_output_error(stream);
		return OBS_OUTPUT_DISCONNECTED;
	}

	obs_output_begin_data_capture(stream->output, 0);

	return OBS_OUTPUT_SUCCESS;
}

#ifdef _WIN32
static void win32_log_interface_type(struct rtmp_stream *stream, RTMP *rtmp)
{
	MIB_IPFORWARDROW route;
	uint32_t dest_addr, source_addr;
	char hostname[256];
	HOSTENT *h;

	if (rtmp->Link.hostname.av_len >= sizeof(hostname) - 1)
		return;

	strncpy(hostname, rtmp->Link.hostname.av_val, sizeof(hostname));
	hostname[rtmp->Link.hostname.av_len] = 0;

	h = gethostbyname(hostname);
	if (!h)
		return;

	dest_addr = *(uint32_t *)h->h_addr_list[0];

	if (rtmp->m_bindIP.addrLen == 0)
		source_addr = 0;
	else if (rtmp->m_bindIP.addr.ss_family == AF_INET)
		source_addr = (*(struct sockaddr_in *)&rtmp->m_bindIP.addr).sin_addr.S_un.S_addr;
	else
		return;

	if (!GetBestRoute(dest_addr, source_addr, &route)) {
		MIB_IF_ROW2 row;
		memset(&row, 0, sizeof(row));
		row.InterfaceIndex = route.dwForwardIfIndex;

		if (!GetIfEntry2(&row)) {
			uint32_t rxSpeed = row.ReceiveLinkSpeed / 1000000;
			uint32_t txSpeed = row.TransmitLinkSpeed / 1000000;
			char *type;
			struct dstr other = {0};

			switch (row.PhysicalMediumType) {
			case NdisPhysicalMedium802_3:
				type = "ethernet";
				break;
			case NdisPhysicalMediumWirelessLan:
			case NdisPhysicalMediumNative802_11:
				type = "802.11";
				break;
			default:
				dstr_printf(&other, "type %d", (int)row.PhysicalMediumType);
				type = other.array;
				break;
			}

			char *desc;
			os_wcs_to_utf8_ptr(row.Description, 0, &desc);

			info("Interface: %s (%s, %lu↓/%lu↑ mbps)", desc, type, rxSpeed, txSpeed);

			bfree(desc);

			if (row.InErrors || row.OutErrors) {
				warn("Interface has non-zero error counters (%" PRIu64 "/%" PRIu64 " errors, %" PRIu64
				     "/%" PRIu64 " discards)",
				     row.InErrors, row.OutErrors, row.InDiscards, row.OutDiscards);
			}

			dstr_free(&other);
		}
	}
}
#endif

/*
 * Brings `rtmp` all the way to "publishing to `path`", short of starting the
 * send machinery. Both the initial connect and a server-directed handoff go
 * through here so a handoff cannot negotiate the connection
 * slightly differently from the initial connect would be a second code path
 * that nothing exercises until a production redirect fires.
 *
 * `path` must outlive `rtmp`. RTMP_SetupURL does not copy the URL; it points
 * Link.hostname, Link.app and Link.tcUrl straight into the string it is given.
 */
static int connect_rtmp(struct rtmp_stream *stream, RTMP *rtmp, struct dstr *path)
{
	if (dstr_is_empty(path)) {
		warn("URL is empty");
		return OBS_OUTPUT_BAD_PATH;
	}

	info("Connecting to RTMP URL %s...", path->array);

	// free any existing RTMP TLS context
	RTMP_TLS_Free(rtmp);

	RTMP_Init(rtmp);

	if (!RTMP_SetupURL(rtmp, path->array))
		return OBS_OUTPUT_BAD_PATH;

	RTMP_EnableWrite(rtmp);

	/*
	 * Declare only what this client actually implements at the connection
	 * layer, and only when the user has turned it on. capsEx is a promise:
	 * a server that sees the Reconnect bit is entitled to hand us to another
	 * host, so advertising it unconditionally would opt every OBS user into
	 * being redirectable whether or not anything would honor it.
	 */
	if (stream->reconnect_request)
		RTMP_SetCapsEx(rtmp, RTMP_CAPS_EX_RECONNECT);

	dstr_copy(&stream->encoder_name, "FMLE/3.0 (compatible; FMSc/1.0)");

	set_rtmp_dstr(&rtmp->Link.pubUser, &stream->username);
	set_rtmp_dstr(&rtmp->Link.pubPasswd, &stream->password);
	set_rtmp_dstr(&rtmp->Link.flashVer, &stream->encoder_name);
	rtmp->Link.swfUrl = rtmp->Link.tcUrl;

	if (dstr_is_empty(&stream->bind_ip) || dstr_cmp(&stream->bind_ip, "default") == 0) {
		memset(&rtmp->m_bindIP, 0, sizeof(rtmp->m_bindIP));
	} else {
		bool success = netif_str_to_addr(&rtmp->m_bindIP.addr, &rtmp->m_bindIP.addrLen, stream->bind_ip.array);
		if (success) {
			int len = rtmp->m_bindIP.addrLen;
			bool ipv6 = len == sizeof(struct sockaddr_in6);
			info("Binding to IPv%d", ipv6 ? 6 : 4);
		}
	}

	// Only use the IPv4 / IPv6 hint if a binding address isn't specified.
	if (rtmp->m_bindIP.addrLen == 0)
		rtmp->m_bindIP.addrLen = stream->addrlen_hint;

	RTMP_AddStream(rtmp, stream->key.array);

	rtmp->m_outChunkSize = 4096;
	rtmp->m_bSendChunkSizeInfo = true;
	rtmp->m_bUseNagle = true;

#ifdef _WIN32
	win32_log_interface_type(stream, rtmp);
#endif

	/* The caller decides what a failure means. A failed initial connect is
	 * the user's problem and gets surfaced; a failed handoff is not, because
	 * the stream it would have replaced is still running. */
	if (!RTMP_Connect(rtmp, NULL))
		return OBS_OUTPUT_CONNECT_FAILED;

	if (!RTMP_ConnectStream(rtmp, 0))
		return OBS_OUTPUT_INVALID_STREAM;

	char ip_address[INET6_ADDRSTRLEN] = {0};
	netif_addr_to_str(&rtmp->m_sb.sb_addr, ip_address, INET6_ADDRSTRLEN);
	info("Connection to %s (%s) successful", path->array, ip_address);

	return OBS_OUTPUT_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Server-directed handoff: connect the new server, then drop the old one.   */

/*
 * Relocates an RTMP context by value.
 *
 * This is safe because m_sb.sb_start is the only pointer an RTMP holds into
 * itself: it indexes m_sb.sb_buf, and RTMPSockBuf_Fill only ever resets it to
 * sb_buf or advances it within sb_buf. Everything else it owns is either heap
 * (m_read.buf, m_write.m_body, m_vecChannelsIn/Out, m_methodCalls, the TLS
 * context, Link.streams[].playpath) or points at caller-owned strings that
 * outlive the move (Link.hostname/app/tcUrl into the path dstr, Link.flashVer
 * and the publish credentials into the stream's own dstrs).
 *
 * `src` is left zeroed and disconnected, so closing or freeing it afterwards is
 * a no-op rather than a double free.
 */
static void rtmp_move(RTMP *dst, RTMP *src)
{
	ptrdiff_t sb_offset = src->m_sb.sb_start ? (src->m_sb.sb_start - src->m_sb.sb_buf) : 0;

	*dst = *src;
	dst->m_sb.sb_start = dst->m_sb.sb_buf + sb_offset;

	memset(src, 0, sizeof(*src));
	src->m_sb.sb_socket = -1;
}

static void *reconnect_handoff_thread(void *data)
{
	struct rtmp_stream *stream = data;

	os_set_thread_name("rtmp-stream: reconnect");

	int ret = connect_rtmp(stream, &stream->handoff_rtmp, &stream->handoff_path);

	/* Publishes handoff_rtmp and handoff_path to the send thread, which
	 * joins this thread before reading either. */
	os_atomic_set_long(&stream->handoff_result, ret == OBS_OUTPUT_SUCCESS ? 1 : -1);
	return NULL;
}

/* Discards an incoming connection that will not be used, and returns the
 * handoff machinery to idle. Send thread only, and only once the handoff
 * thread has been joined. */
static void reconnect_discard(struct rtmp_stream *stream)
{
	RTMP_Close(&stream->handoff_rtmp);
	RTMP_TLS_Free(&stream->handoff_rtmp);
	memset(&stream->handoff_rtmp, 0, sizeof(stream->handoff_rtmp));
	stream->handoff_rtmp.m_sb.sb_socket = -1;

	dstr_free(&stream->handoff_path);
	dstr_free(&stream->reconnect_url);
	os_atomic_set_long(&stream->handoff_result, 0);
	stream->handoff_state = HANDOFF_IDLE;
}

/* Joins an in-flight handoff and throws the result away. Used when the stream
 * is going down for any reason; the send thread must not exit while another
 * thread is still writing into the stream. */
static void reconnect_abort(struct rtmp_stream *stream)
{
	if (stream->handoff_state == HANDOFF_CONNECTING)
		pthread_join(stream->handoff_thread, NULL);

	if (stream->handoff_state != HANDOFF_IDLE)
		reconnect_discard(stream);
	else
		dstr_free(&stream->reconnect_url);
}

static void reconnect_start(struct rtmp_stream *stream)
{
	dstr_copy_dstr(&stream->handoff_path, &stream->reconnect_url);
	os_atomic_set_long(&stream->handoff_result, 0);

	if (pthread_create(&stream->handoff_thread, NULL, reconnect_handoff_thread, stream) != 0) {
		warn("Could not start the reconnect thread; staying on %s", stream->path.array);
		dstr_free(&stream->handoff_path);
		dstr_free(&stream->reconnect_url);
		return;
	}

	stream->handoff_state = HANDOFF_CONNECTING;
	info("Reconnect: establishing %s while %s keeps carrying the stream", stream->handoff_path.array,
	     stream->path.array);
}

/*
 * Cuts over to the connection the handoff thread established.
 *
 * The order here is the point of the whole feature: the new server is already
 * publishing before anything is said to the old one, and the old connection is
 * closed only after the new one has accepted the stream metadata. If the new
 * server rejects us at that last moment, nothing has been lost: the outgoing
 * connection is still open and still the active one.
 */
static void reconnect_commit(struct rtmp_stream *stream)
{
	RTMP outgoing;
	struct dstr outgoing_path;

	/* Metadata is what init_send() puts on the wire first, so the incoming
	 * connection gets it first too, while it is still expendable. */
	if (!send_meta_data(stream, &stream->handoff_rtmp)) {
		warn("Reconnect to %s failed while sending metadata; staying on %s", stream->handoff_path.array,
		     stream->path.array);
		reconnect_discard(stream);
		return;
	}

	rtmp_move(&outgoing, &stream->rtmp);
	rtmp_move(&stream->rtmp, &stream->handoff_rtmp);

	/* The path dstr backs the RTMP's Link strings, so it moves with it
	 * rather than being copied over. dstr_init here means "this handle no
	 * longer owns the buffer", not "free it"; ownership went to
	 * stream->path on the line above. */
	outgoing_path = stream->path;
	stream->path = stream->handoff_path;
	dstr_init(&stream->handoff_path);

	/* Sequence headers are per-connection. send_thread re-sends them,
	 * along with the keyframe it is holding, on the connection that is now
	 * current. start_dts_offset does not move: the media
	 * timeline is continuous across the handoff, which is what a downstream
	 * packager needs in order to treat this as one broadcast. */
	stream->sent_headers = false;

	dstr_free(&stream->reconnect_url);
	os_atomic_set_long(&stream->handoff_result, 0);
	stream->handoff_state = HANDOFF_IDLE;

	info("Reconnect complete: now publishing to %s; closing %s", stream->path.array, outgoing_path.array);

	RTMP_Close(&outgoing);
	RTMP_TLS_Free(&outgoing);
	dstr_free(&outgoing_path);
}

/* A video keyframe on the first video track is the media boundary the spec
 * asks the client to wait for. Every other video track keyframes with it, so
 * cutting over here keeps each GOP whole on one server. */
static inline bool is_media_boundary(const struct encoder_packet *packet)
{
	return packet->type == OBS_ENCODER_VIDEO && packet->keyframe && packet->track_idx == 0;
}

/*
 * Advances the handoff one step, if there is one. Called from the send thread
 * with the packet that is about to go out, which is also the packet that will
 * be the first thing the new server sees if this is the boundary.
 */
static void reconnect_pump(struct rtmp_stream *stream, const struct encoder_packet *packet)
{
	switch (stream->handoff_state) {
	case HANDOFF_IDLE:
		if (dstr_is_empty(&stream->reconnect_url))
			return;
		reconnect_start(stream);
		return;

	case HANDOFF_CONNECTING: {
		long result = os_atomic_load_long(&stream->handoff_result);
		if (result == 0)
			return;

		pthread_join(stream->handoff_thread, NULL);

		if (result < 0) {
			warn("Reconnect to %s failed; staying on %s", stream->handoff_path.array, stream->path.array);
			reconnect_discard(stream);
			return;
		}

		stream->handoff_state = HANDOFF_ARMED;
	}
		/* fallthrough: this packet may already be the boundary */

	case HANDOFF_ARMED:
		if (is_media_boundary(packet))
			reconnect_commit(stream);
		return;
	}
}

static int try_connect(struct rtmp_stream *stream)
{
	int ret = connect_rtmp(stream, &stream->rtmp, &stream->path);
	if (ret != OBS_OUTPUT_SUCCESS) {
		if (ret == OBS_OUTPUT_CONNECT_FAILED)
			set_output_error(stream);
		return ret;
	}

	return init_send(stream);
}

/*
 * Builds the DBR interpolation table from a JSON string.
 *
 * The input JSON is structured as [encoder_index][quality_level], e.g. for a 5-encoder config
 * (2560x1440 HEVC, 1920x1080, 1280x720, 852x480, 640x360):
 *   [
 *     [0, 5940, 7200, 9000],   // encoder 0 (2560x1440)
 *     [0, 4950, 6000, 7500],   // encoder 1 (1920x1080)
 *     [0, 2800, 3500, 3500],   // encoder 2 (1280x720)
 *     [0, 1000, 1000, 1000],   // encoder 3 (852x480)
 *     [0,  500,  500,  500]    // encoder 4 (640x360)
 *   ]
 *
 * This is transposed into the interpolation table where each entry represents a quality level
 * containing per-encoder bitrates:
 *   table[0].bitrates[] = {0,    0,    0,    0,    0}     // level 0 (minimum)
 *   table[1].bitrates[] = {5940, 4950, 2800, 1000, 500}   // level 1
 *   table[2].bitrates[] = {7200, 6000, 3500, 1000, 500}   // level 2
 *   table[3].bitrates[] = {9000, 7500, 3500, 1000, 500}   // level 3 (maximum)
 *
 * The encoder index must match the output encoder index used with obs_output_get_video_encoder2().
 * All encoder arrays must be the same size.
 *
 * dbr_set_bitrate() walks this table to find the appropriate quality level based on the current
 * aggregate bitrate and applies per-encoder bitrates via linear interpolation between adjacent levels.
 */
static bool build_dbr_interpolation_table(struct rtmp_stream *stream, const char *interpolation_data)
{
	json_error_t error;
	json_t *root = json_loads(interpolation_data, JSON_REJECT_DUPLICATES, &error);
	if (!root) {
		warn("loading dbr interpolation table failed: %s", error.text);
		return false;
	}

	size_t all_array_size = 0;
	bool malformed_data = false;
	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; ++i) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(stream->output, i);
		if (!enc)
			continue;

		const char *name = obs_encoder_get_name(enc);
		const json_t *array = json_array_get(root, i);
		if (!array) {
			warn("missing bitrate interpolation data for encoder '%s'", name);
			malformed_data = true;
			continue;
		}

		size_t size = json_array_size(array);
		if (!all_array_size) {
			all_array_size = size;
			continue;
		}

		if (all_array_size != size) {
			warn("size %zu of bitrate interpolation data for encoder '%s' does not match overall size %zu",
			     size, name, all_array_size);
			malformed_data = true;
		}
	}

	if (malformed_data)
		goto error;

	da_clear(stream->dbr_interpolation_table);
	da_reserve(stream->dbr_interpolation_table, all_array_size);
	for (size_t i = 0; i < all_array_size; ++i) {
		struct dbr_interpolation_point *dbr_point = da_push_back_new(stream->dbr_interpolation_table);
		for (size_t j = 0; j < MAX_OUTPUT_VIDEO_ENCODERS; ++j) {
			const json_t *array = json_array_get(root, j);
			if (!array)
				continue;

			const json_t *entry = json_array_get(array, i);
			if (!json_is_integer(entry)) {
				warn("interpolation entry %zu for track %zu is not an integer (type: %d)", i, j,
				     json_typeof(entry));
				malformed_data = true;
				continue;
			}
			dbr_point->bitrates[j] = (long)json_integer_value(entry);
		}
	}

	json_decref(root);
	return true;

error:
	json_decref(root);
	return false;
}

static bool init_connect(struct rtmp_stream *stream)
{
	obs_service_t *service;
	obs_data_t *settings;
	const char *bind_ip;
	const char *ip_family;
	int64_t drop_p;
	int64_t drop_b;

	if (stopping(stream)) {
		pthread_join(stream->send_thread, NULL);
	}

	free_packets(stream);

	service = obs_output_get_service(stream->output);
	if (!service)
		return false;

	os_atomic_set_bool(&stream->disconnected, false);
	os_atomic_set_bool(&stream->encode_error, false);
	stream->total_bytes_sent = 0;
	stream->dropped_frames = 0;
	stream->min_priority = 0;
	stream->got_first_packet = false;

	settings = obs_output_get_settings(stream->output);
	dstr_copy(&stream->path, obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL));
	dstr_copy(&stream->key, obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY));
	dstr_copy(&stream->username, obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_USERNAME));
	dstr_copy(&stream->password, obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_PASSWORD));
	dstr_depad(&stream->path);
	dstr_depad(&stream->key);

	dstr_free(&stream->reconnect_url);
	stream->reconnect_count = 0;
	stream->reconnect_refusals = 0;
	stream->handoff_state = HANDOFF_IDLE;
	os_atomic_set_long(&stream->handoff_result, 0);

	stream->reconnect_request = obs_data_get_bool(settings, OPT_RECONNECT_REQUEST);

	stream->reconnect_limit = (int)obs_data_get_int(settings, OPT_RECONNECT_REQUEST_LIMIT);
	if (stream->reconnect_limit < 0)
		stream->reconnect_limit = 0;
	if (stream->reconnect_limit > RECONNECT_REQUEST_LIMIT_MAX)
		stream->reconnect_limit = RECONNECT_REQUEST_LIMIT_MAX;

	drop_b = (int64_t)obs_data_get_int(settings, OPT_DROP_THRESHOLD);
	drop_p = (int64_t)obs_data_get_int(settings, OPT_PFRAME_DROP_THRESHOLD);
	stream->max_shutdown_time_sec = (int)obs_data_get_int(settings, OPT_MAX_SHUTDOWN_TIME_SEC);

	long long overall_audio_bitrate = 0;
	long long overall_video_bitrate = 0;
	bool dbr_capable = true;
	for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++) {
		obs_encoder_t *enc = obs_output_get_audio_encoder(stream->output, i);
		if (!enc)
			continue;

		const char *codec = obs_encoder_get_codec(enc);
		stream->audio_codec[i] = to_audio_type(codec);

		obs_data_t *settings = obs_encoder_get_settings(enc);
		overall_audio_bitrate += obs_data_get_int(settings, "bitrate");
		obs_data_release(settings);
	}

	da_reserve(stream->dbr_interpolation_table, 2);
	da_push_back_new(stream->dbr_interpolation_table);
	struct dbr_interpolation_point *dbr_point = da_push_back_new(stream->dbr_interpolation_table);

	/* Determine if the stream is multitrack by checking for any encoder beyond index 0 */
	bool is_multitrack = false;
	for (size_t j = 1; j < MAX_OUTPUT_VIDEO_ENCODERS; ++j) {
		if (obs_output_get_video_encoder2(stream->output, j) != NULL) {
			is_multitrack = true;
			break;
		}
	}

	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; ++i) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(stream->output, i);
		if (!enc)
			continue;

		const char *codec = obs_encoder_get_codec(enc);
		stream->video_codec[i] = to_video_type(codec);

		uint32_t caps = obs_encoder_get_caps(enc);
		bool has_dbr_cap = (caps & OBS_ENCODER_CAP_DYN_BITRATE) != 0;
		bool has_multitrack_dbr_cap = (caps & OBS_ENCODER_CAP_MULTITRACK_DYN_BITRATE) != 0;

		// For multitrack, all encoders must support multitrack Dynamic Bitrate (DBR). For single track,
		// encoder must support standard Dynamic Bitrate.
		bool dbr_supported = is_multitrack ? has_multitrack_dbr_cap : has_dbr_cap;

		if (!dbr_supported) {
			dbr_capable = false;
			if (!has_dbr_cap) {
				info("Dynamic bitrate disabled. "
				     "The encoder '%s' does not support on-the-fly bitrate reconfiguration.",
				     obs_encoder_get_name(enc));
			} else if (is_multitrack && !has_multitrack_dbr_cap) {
				info("Dynamic bitrate disabled. "
				     "The encoder '%s' does not support on-the-fly multitrack bitrate reconfiguration.",
				     obs_encoder_get_name(enc));
			}
		}

		obs_data_t *settings = obs_encoder_get_settings(enc);
		long long bitrate = obs_data_get_int(settings, "bitrate");
		overall_video_bitrate += bitrate;
		dbr_point->bitrates[i] = (long)bitrate;
		obs_data_release(settings);
	}

	deque_free(&stream->dbr_frames);
	stream->audio_bitrate = (long)overall_audio_bitrate;
	stream->dbr_data_size = 0;
	stream->dbr_orig_bitrate = (long)overall_video_bitrate;
	stream->dbr_cur_bitrate = stream->dbr_orig_bitrate;
	stream->dbr_est_bitrate = 0;
	stream->dbr_inc_bitrate = stream->dbr_orig_bitrate / 10;
	stream->dbr_inc_timeout = 0;
	stream->dbr_enabled = dbr_capable && obs_data_get_bool(settings, OPT_DYN_BITRATE);

	if (obs_output_get_delay(stream->output) != 0) {
		info("Dynamic bitrate disabled. Stream delay and dynamic bitrate are incompatible.");
		stream->dbr_enabled = false;
	}

	if (stream->dbr_enabled && !obs_data_has_user_value(settings, OPT_DYN_BITRATE_INTERPOLATION_TABLE_DATA)) {
		info("Dynamic bitrate using default interpolation");
	} else if (obs_data_has_user_value(settings, OPT_DYN_BITRATE_INTERPOLATION_TABLE_DATA)) {
		const char *dbr_interpolation_data =
			obs_data_get_string(settings, OPT_DYN_BITRATE_INTERPOLATION_TABLE_DATA);
		if (!build_dbr_interpolation_table(stream, dbr_interpolation_data)) {
			warn("Loading interpolation settings failed, disabling dynamic bitrate");
			stream->dbr_enabled = false;
		} else {
			info("Successfully loaded dynamic bitrate interpolation settings");
		}
	}

	if (stream->dbr_enabled) {
		info("Dynamic bitrate enabled.  Dropped frames begone!");
	}

	if (drop_p < (drop_b + 200))
		drop_p = drop_b + 200;

	stream->drop_threshold_usec = 1000 * drop_b;
	stream->pframe_drop_threshold_usec = 1000 * drop_p;

	bind_ip = obs_data_get_string(settings, OPT_BIND_IP);
	dstr_copy(&stream->bind_ip, bind_ip);

	// Check that we have an IP Family set and that the setting length
	// is 4 characters long so we don't capture ie. IPv4+IPv6
	ip_family = obs_data_get_string(settings, OPT_IP_FAMILY);
	if (ip_family != NULL && strlen(ip_family) == 4) {
		socklen_t len = 0;
		if (strncmp(ip_family, "IPv6", 4) == 0)
			len = sizeof(struct sockaddr_in6);
		else if (strncmp(ip_family, "IPv4", 4) == 0)
			len = sizeof(struct sockaddr_in);
		stream->addrlen_hint = len;
	}

#ifdef _WIN32
	stream->new_socket_loop = obs_data_get_bool(settings, OPT_NEWSOCKETLOOP_ENABLED);
	stream->low_latency_mode = obs_data_get_bool(settings, OPT_LOWLATENCY_ENABLED);

	// ugly hack for now, can be removed once new loop is reworked
	if (stream->new_socket_loop && !strncmp(stream->path.array, "rtmps://", 8)) {
		warn("Disabling network optimizations, not compatible with RTMPS");
		stream->new_socket_loop = false;
	}
#else
	stream->new_socket_loop = false;
	stream->low_latency_mode = false;
#endif

	/*
	 * The Windows socket loop runs on its own thread and holds the active
	 * socket and send buffer directly (rtmp-windows.c), so the send thread
	 * cannot substitute one connection for another underneath it. Rather
	 * than advertise a capability we would not honor, the feature turns
	 * itself off and says so.
	 */
	if (stream->new_socket_loop && stream->reconnect_request) {
		warn("Server-directed reconnect disabled: it is not compatible with the new socket loop");
		stream->reconnect_request = false;
	}

	if (stream->reconnect_request)
		info("Server-directed reconnect enabled (at most %d per session)", stream->reconnect_limit);

	obs_data_release(settings);
	return true;
}

static void *connect_thread(void *data)
{
	struct rtmp_stream *stream = data;
	int ret;

	os_set_thread_name("rtmp-stream: connect_thread");

	if (!init_connect(stream)) {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_BAD_PATH);
		return NULL;
	}

	// HDR streaming disabled for AV1
	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		if (stream->video_codec[i] && stream->video_codec[i] != CODEC_H264 &&
		    stream->video_codec[i] != CODEC_HEVC) {
			obs_encoder_t *enc = obs_output_get_video_encoder2(stream->output, i);
			video_t *video = obs_encoder_video(enc);
			const struct video_output_info *info = video_output_get_info(video);

			if (info->colorspace == VIDEO_CS_2100_HLG || info->colorspace == VIDEO_CS_2100_PQ) {
				obs_output_signal_stop(stream->output, OBS_OUTPUT_HDR_DISABLED);
				return NULL;
			}
		}
	}

	ret = try_connect(stream);

	if (ret != OBS_OUTPUT_SUCCESS) {
		obs_output_signal_stop(stream->output, ret);
		info("Connection to %s failed: %d", stream->path.array, ret);
	}

	if (!stopping(stream))
		pthread_detach(stream->connect_thread);

	os_atomic_set_bool(&stream->connecting, false);
	return NULL;
}

static bool rtmp_stream_start(void *data)
{
	struct rtmp_stream *stream = data;

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	os_atomic_set_bool(&stream->connecting, true);
	return pthread_create(&stream->connect_thread, NULL, connect_thread, stream) == 0;
}

static inline bool add_packet(struct rtmp_stream *stream, struct encoder_packet *packet)
{
	deque_push_back(&stream->packets, packet, sizeof(struct encoder_packet));
	return true;
}

static inline size_t num_buffered_packets(struct rtmp_stream *stream)
{
	return stream->packets.size / sizeof(struct encoder_packet);
}

static void drop_frames(struct rtmp_stream *stream, const char *name, int highest_priority, bool pframes)
{
	UNUSED_PARAMETER(pframes);

	struct deque new_buf = {0};
	int num_frames_dropped = 0;

#ifdef _DEBUG
	int start_packets = (int)num_buffered_packets(stream);
#else
	UNUSED_PARAMETER(name);
#endif

	deque_reserve(&new_buf, sizeof(struct encoder_packet) * 8);

	while (stream->packets.size) {
		struct encoder_packet packet;
		deque_pop_front(&stream->packets, &packet, sizeof(packet));

		/* do not drop audio data or video keyframes */
		if (packet.type == OBS_ENCODER_AUDIO || packet.drop_priority >= highest_priority) {
			deque_push_back(&new_buf, &packet, sizeof(packet));

		} else {
			num_frames_dropped++;
			obs_encoder_packet_release(&packet);
		}
	}

	deque_free(&stream->packets);
	stream->packets = new_buf;

	if (stream->min_priority < highest_priority)
		stream->min_priority = highest_priority;
	if (!num_frames_dropped)
		return;

	stream->dropped_frames += num_frames_dropped;
#ifdef _DEBUG
	debug("Dropped %s, prev packet count: %d, new packet count: %d", name, start_packets,
	      (int)num_buffered_packets(stream));
#endif
}

static bool find_first_video_packet(struct rtmp_stream *stream, struct encoder_packet *first)
{
	size_t count = stream->packets.size / sizeof(*first);

	for (size_t i = 0; i < count; i++) {
		struct encoder_packet *cur = deque_data(&stream->packets, i * sizeof(*first));
		if (cur->type == OBS_ENCODER_VIDEO && !cur->keyframe) {
			*first = *cur;
			return true;
		}
	}

	return false;
}

static bool dbr_bitrate_lowered(struct rtmp_stream *stream)
{
	long prev_bitrate = stream->dbr_prev_bitrate;
	long est_bitrate = 0;
	long new_bitrate;

	if (stream->dbr_est_bitrate && stream->dbr_est_bitrate < stream->dbr_cur_bitrate) {
		stream->dbr_data_size = 0;
		deque_pop_front(&stream->dbr_frames, NULL, stream->dbr_frames.size);
		est_bitrate = stream->dbr_est_bitrate / 100 * 100;
		if (est_bitrate < 50) {
			est_bitrate = 50;
		}
	}

#if 0
	if (prev_bitrate && est_bitrate) {
		if (prev_bitrate < est_bitrate) {
			blog(LOG_INFO, "going back to prev bitrate: "
					"prev_bitrate (%d) < est_bitrate (%d)",
					prev_bitrate,
					est_bitrate);
			new_bitrate = prev_bitrate;
		} else {
			new_bitrate = est_bitrate;
		}
		new_bitrate = est_bitrate;
	} else if (prev_bitrate) {
		new_bitrate = prev_bitrate;
		info("going back to prev bitrate");

	} else if (est_bitrate) {
		new_bitrate = est_bitrate;

	} else {
		return false;
	}
#else
	if (est_bitrate) {
		new_bitrate = est_bitrate;

	} else if (prev_bitrate) {
		new_bitrate = prev_bitrate;
		info("going back to prev bitrate");

	} else {
		return false;
	}

	if (new_bitrate == stream->dbr_cur_bitrate) {
		return false;
	}
#endif

	stream->dbr_prev_bitrate = 0;
	stream->dbr_cur_bitrate = new_bitrate;
	stream->dbr_inc_timeout = os_gettime_ns() + DBR_INC_TIMER;
	info("bitrate decreased to: %ld", stream->dbr_cur_bitrate);
	return true;
}

static void dbr_set_bitrate(struct rtmp_stream *stream)
{
	if (stream->dbr_interpolation_table.array == NULL || stream->dbr_interpolation_table.num == 0)
		return;

	size_t dbr_base_column = stream->dbr_interpolation_table.num - 1;
	for (size_t column = 0; column < stream->dbr_interpolation_table.num; ++column) {
		struct dbr_interpolation_point *dbr_point = &stream->dbr_interpolation_table.array[column];
		long column_bitrate = 0;
		for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; ++i) {
			column_bitrate += dbr_point->bitrates[i];
		}
		if (column_bitrate > stream->dbr_cur_bitrate)
			break;

		dbr_base_column = column;
	}

	size_t dbr_upper_column = dbr_base_column + 1;
	if (dbr_upper_column >= stream->dbr_interpolation_table.num)
		dbr_upper_column = stream->dbr_interpolation_table.num - 1;

	struct dbr_interpolation_point *dbr_base_point = &stream->dbr_interpolation_table.array[dbr_base_column];
	struct dbr_interpolation_point *dbr_upper_point = &stream->dbr_interpolation_table.array[dbr_upper_column];
	long deltas[MAX_OUTPUT_VIDEO_ENCODERS] = {0};
	long remaining_bitrate = stream->dbr_cur_bitrate;
	long overall_delta = 0;
	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; ++i) {
		deltas[i] = dbr_upper_point->bitrates[i] - dbr_base_point->bitrates[i];
		overall_delta += deltas[i];
		remaining_bitrate -= dbr_base_point->bitrates[i];
	}

	if (overall_delta < 1)
		overall_delta = 1;

	double ratio = remaining_bitrate / (double)overall_delta;
	double delta_scale = ratio < 1.0 ? ratio : 1.0;
	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; ++i) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(stream->output, i);
		if (!enc)
			continue;

		long bitrate = dbr_base_point->bitrates[i] + (long)(deltas[i] * delta_scale);

		obs_data_t *settings = obs_encoder_get_settings(enc);
		if (obs_data_get_int(settings, "bitrate") != bitrate) {
			obs_data_set_int(settings, "bitrate", bitrate);
			obs_encoder_update(enc, settings);
		}
		obs_data_release(settings);
	}
}

static void dbr_inc_bitrate(struct rtmp_stream *stream)
{
	stream->dbr_prev_bitrate = stream->dbr_cur_bitrate;
	stream->dbr_cur_bitrate += stream->dbr_inc_bitrate;

	if (stream->dbr_cur_bitrate >= stream->dbr_orig_bitrate) {
		stream->dbr_cur_bitrate = stream->dbr_orig_bitrate;
		info("bitrate increased to: %ld, done", stream->dbr_cur_bitrate);
	} else if (stream->dbr_cur_bitrate < stream->dbr_orig_bitrate) {
		stream->dbr_inc_timeout = os_gettime_ns() + DBR_INC_TIMER;
		info("bitrate increased to: %ld, waiting", stream->dbr_cur_bitrate);
	}
}

static void check_to_drop_frames(struct rtmp_stream *stream, bool pframes)
{
	struct encoder_packet first;
	int64_t buffer_duration_usec;
	size_t num_packets = num_buffered_packets(stream);
	const char *name = pframes ? "p-frames" : "b-frames";
	int priority = pframes ? OBS_NAL_PRIORITY_HIGHEST : OBS_NAL_PRIORITY_HIGH;
	int64_t drop_threshold = pframes ? stream->pframe_drop_threshold_usec : stream->drop_threshold_usec;

	if (!pframes && stream->dbr_enabled) {
		if (stream->dbr_inc_timeout) {
			uint64_t t = os_gettime_ns();

			if (t >= stream->dbr_inc_timeout) {
				stream->dbr_inc_timeout = 0;
				dbr_inc_bitrate(stream);
				dbr_set_bitrate(stream);
			}
		}
	}

	if (num_packets < 5) {
		if (!pframes)
			stream->congestion = 0.0f;
		return;
	}

	if (!find_first_video_packet(stream, &first))
		return;

	/* if the amount of time stored in the buffered packets waiting to be
	 * sent is higher than threshold, drop frames */
	buffer_duration_usec = stream->last_dts_usec - first.dts_usec;

	if (!pframes) {
		stream->congestion = (float)buffer_duration_usec / (float)drop_threshold;
	}

	/* alternatively, drop only pframes:
	 * (!pframes && stream->dbr_enabled)
	 * but let's test without dropping frames
	 * at all first */
	if (stream->dbr_enabled) {
		bool bitrate_changed = false;

		if (pframes) {
			return;
		}

		if ((uint64_t)buffer_duration_usec >= DBR_TRIGGER_USEC) {
			pthread_mutex_lock(&stream->dbr_mutex);
			bitrate_changed = dbr_bitrate_lowered(stream);
			pthread_mutex_unlock(&stream->dbr_mutex);
		}

		if (bitrate_changed) {
			debug("buffer_duration_msec: %" PRId64, buffer_duration_usec / 1000);
			dbr_set_bitrate(stream);
		}
		return;
	}

	if (buffer_duration_usec > drop_threshold) {
		debug("buffer_duration_usec: %" PRId64, buffer_duration_usec);
		drop_frames(stream, name, priority, pframes);
	}
}

static bool add_video_packet(struct rtmp_stream *stream, struct encoder_packet *packet)
{
	check_to_drop_frames(stream, false);
	check_to_drop_frames(stream, true);

	/* if currently dropping frames, drop packets until it reaches the
	 * desired priority */
	if (packet->drop_priority < stream->min_priority) {
		stream->dropped_frames++;
		return false;
	} else {
		stream->min_priority = 0;
	}

	stream->last_dts_usec = packet->dts_usec;
	return add_packet(stream, packet);
}

static void rtmp_stream_data(void *data, struct encoder_packet *packet)
{
	struct rtmp_stream *stream = data;
	struct encoder_packet new_packet;
	bool added_packet = false;

	if (disconnected(stream) || !active(stream))
		return;

	/* encoder fail */
	if (!packet) {
		os_atomic_set_bool(&stream->encode_error, true);
		os_sem_post(stream->send_sem);
		return;
	}

	if (packet->type == OBS_ENCODER_VIDEO) {
		if (!stream->got_first_packet) {
			stream->start_dts_offset = get_ms_time(packet, packet->dts);
			stream->got_first_packet = true;
		}

		switch (stream->video_codec[packet->track_idx]) {
		case CODEC_NONE:
			do_log(LOG_ERROR, "Codec not initialized for track %zu", packet->track_idx);
			return;

		case CODEC_H264:
			obs_parse_avc_packet(&new_packet, packet);
			break;
		case CODEC_HEVC:
#ifdef ENABLE_HEVC
			obs_parse_hevc_packet(&new_packet, packet);
			break;
#else
			return;
#endif
		case CODEC_AV1:
			obs_parse_av1_packet(&new_packet, packet);
			break;
		}
	} else {
		if (!stream->got_first_packet) {
			stream->start_dts_offset = get_ms_time(packet, packet->dts);
			stream->got_first_packet = true;
		}

		obs_encoder_packet_ref(&new_packet, packet);
	}

	pthread_mutex_lock(&stream->packets_mutex);

	if (!disconnected(stream)) {
		added_packet = (packet->type == OBS_ENCODER_VIDEO) ? add_video_packet(stream, &new_packet)
								   : add_packet(stream, &new_packet);
	}

	pthread_mutex_unlock(&stream->packets_mutex);

	if (added_packet)
		os_sem_post(stream->send_sem);
	else
		obs_encoder_packet_release(&new_packet);
}

static void rtmp_stream_defaults(obs_data_t *defaults)
{
	obs_data_set_default_int(defaults, OPT_DROP_THRESHOLD, 700);
	obs_data_set_default_int(defaults, OPT_PFRAME_DROP_THRESHOLD, 900);
	obs_data_set_default_int(defaults, OPT_MAX_SHUTDOWN_TIME_SEC, 30);
	obs_data_set_default_string(defaults, OPT_BIND_IP, "default");
	/* Off unless a front-end or the user turns it on: honoring a redirect
	 * sends the stream key to a host the user did not choose. */
	obs_data_set_default_bool(defaults, OPT_RECONNECT_REQUEST, false);
	obs_data_set_default_int(defaults, OPT_RECONNECT_REQUEST_LIMIT, 5);
#ifdef _WIN32
	obs_data_set_default_bool(defaults, OPT_NEWSOCKETLOOP_ENABLED, false);
	obs_data_set_default_bool(defaults, OPT_LOWLATENCY_ENABLED, false);
#endif
}

static obs_properties_t *rtmp_stream_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	struct netif_saddr_data addrs = {0};
	obs_property_t *p;

	p = obs_properties_add_int(props, OPT_DROP_THRESHOLD, obs_module_text("RTMPStream.DropThreshold"), 200, 10000,
				   100);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(props, OPT_IP_FAMILY, obs_module_text("IPFamily"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, obs_module_text("IPFamily.Both"), "IPv4+IPv6");
	obs_property_list_add_string(p, obs_module_text("IPFamily.V4Only"), "IPv4");
	obs_property_list_add_string(p, obs_module_text("IPFamily.V6Only"), "IPv6");

	p = obs_properties_add_list(props, OPT_BIND_IP, obs_module_text("RTMPStream.BindIP"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, obs_module_text("Default"), "default");

	netif_get_addrs(&addrs);
	for (size_t i = 0; i < addrs.addrs.num; i++) {
		struct netif_saddr_item item = addrs.addrs.array[i];
		obs_property_list_add_string(p, item.name, item.addr);
	}
	netif_saddr_data_free(&addrs);

	p = obs_properties_add_bool(props, OPT_RECONNECT_REQUEST, obs_module_text("RTMPStream.ReconnectRequest"));
	obs_property_set_long_description(p, obs_module_text("RTMPStream.ReconnectRequest.Description"));

	obs_properties_add_int(props, OPT_RECONNECT_REQUEST_LIMIT, obs_module_text("RTMPStream.ReconnectRequest.Limit"),
			       0, RECONNECT_REQUEST_LIMIT_MAX, 1);

#ifdef _WIN32
	obs_properties_add_bool(props, OPT_NEWSOCKETLOOP_ENABLED, obs_module_text("RTMPStream.NewSocketLoop"));
	obs_properties_add_bool(props, OPT_LOWLATENCY_ENABLED, obs_module_text("RTMPStream.LowLatencyMode"));
#endif

	return props;
}

static uint64_t rtmp_stream_total_bytes_sent(void *data)
{
	struct rtmp_stream *stream = data;
	return stream->total_bytes_sent;
}

static int rtmp_stream_dropped_frames(void *data)
{
	struct rtmp_stream *stream = data;
	return stream->dropped_frames;
}

static float rtmp_stream_congestion(void *data)
{
	struct rtmp_stream *stream = data;

	if (stream->new_socket_loop)
		return (float)stream->write_buf_len / (float)stream->write_buf_size;
	else
		return stream->min_priority > 0 ? 1.0f : stream->congestion;
}

static int rtmp_stream_connect_time(void *data)
{
	struct rtmp_stream *stream = data;
	return stream->rtmp.connect_time_ms;
}

struct obs_output_info rtmp_output_info = {
	.id = "rtmp_output",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE | OBS_OUTPUT_MULTI_TRACK_AV,
#ifdef NO_CRYPTO
	.protocols = "RTMP",
#else
	.protocols = "RTMP;RTMPS",
#endif
#ifdef ENABLE_HEVC
	.encoded_video_codecs = "h264;hevc;av1",
#else
	.encoded_video_codecs = "h264;av1",
#endif
	.encoded_audio_codecs = "aac",
	.get_name = rtmp_stream_getname,
	.create = rtmp_stream_create,
	.destroy = rtmp_stream_destroy,
	.start = rtmp_stream_start,
	.stop = rtmp_stream_stop,
	.encoded_packet = rtmp_stream_data,
	.get_defaults = rtmp_stream_defaults,
	.get_properties = rtmp_stream_properties,
	.get_total_bytes = rtmp_stream_total_bytes_sent,
	.get_congestion = rtmp_stream_congestion,
	.get_connect_time_ms = rtmp_stream_connect_time,
	.get_dropped_frames = rtmp_stream_dropped_frames,
};

/*
 * Headless libobs driver for exercising the RTMP output.
 *
 * Not part of the OBS patch: this is the acceptance harness for the Enhanced
 * RTMP v2 reconnect work. It brings up libobs with a synthetic video source and
 * silent audio, points the rtmp_output at a server, streams for a fixed number
 * of seconds, and reports how the output ended.
 *
 * Usage:
 *   obs-rtmp-driver <server-url> <stream-key> <seconds> [enable] [limit]
 *
 * [enable] is "disabled" (the default) to leave server-directed reconnect off,
 * or any other value to turn it on. The feature is a plain on/off switch; the
 * client does not police which host a server may name, so there is nothing
 * finer to select here.
 *
 * Exit codes:
 *   0  streamed for the requested duration and stopped cleanly
 *   1  setup failed
 *   2  the output stopped on its own before the duration elapsed
 */

#include <obs.h>
#include <util/platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile bool stopped_early;
static int stop_code = -1;
static char stop_reason[512];

static void on_stop(void *data, calldata_t *cd)
{
	(void)data;
	stop_code = (int)calldata_int(cd, "code");
	const char *last = calldata_string(cd, "last_error");
	snprintf(stop_reason, sizeof(stop_reason), "%s", last ? last : "");
	stopped_early = true;
}

static void log_handler(int level, const char *format, va_list args, void *param)
{
	(void)param;
	const char *tag = level <= LOG_ERROR    ? "ERROR"
			  : level <= LOG_WARNING ? "WARN "
			  : level <= LOG_INFO    ? "INFO "
						 : "DEBUG";
	fprintf(stderr, "[obs %s] ", tag);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	fflush(stderr);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <server-url> <stream-key> <seconds> [enable] [limit]\n", argv[0]);
		return 1;
	}

	const char *server = argv[1];
	const char *key = argv[2];
	int seconds = atoi(argv[3]);
	const char *enable_arg = argc > 4 ? argv[4] : "disabled";
	bool reconnect = strcmp(enable_arg, "disabled") != 0;
	int limit = argc > 5 ? atoi(argv[5]) : 5;

	base_set_log_handler(log_handler, NULL);

	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "DRIVER: obs_startup failed\n");
		return 1;
	}

	struct obs_video_info ovi = {
		.graphics_module = "libobs-opengl",
		.fps_num = 30,
		.fps_den = 1,
		.base_width = 640,
		.base_height = 360,
		.output_width = 640,
		.output_height = 360,
		.output_format = VIDEO_FORMAT_NV12,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	int video_ret = obs_reset_video(&ovi);
	if (video_ret != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "DRIVER: obs_reset_video failed: %d\n", video_ret);
		return 1;
	}

	struct obs_audio_info oai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (!obs_reset_audio(&oai)) {
		fprintf(stderr, "DRIVER: obs_reset_audio failed\n");
		return 1;
	}

	const char *plugin_bin = getenv("OBS_PLUGIN_BIN");
	const char *plugin_data = getenv("OBS_PLUGIN_DATA");
	obs_add_module_path(plugin_bin ? plugin_bin : "obs-plugins/64bit",
			    plugin_data ? plugin_data : "data/obs-plugins/%module%");
	obs_load_all_modules();
	obs_post_load_modules();

	/* A moving source: the encoder must produce real keyframes, and the
	 * cut-over is defined in terms of them. */
	obs_data_t *source_settings = obs_data_create();
	obs_data_set_int(source_settings, "width", 640);
	obs_data_set_int(source_settings, "height", 360);
	obs_data_set_int(source_settings, "color", 0xFF3080C0);
	obs_source_t *source = obs_source_create("color_source", "colour", source_settings, NULL);
	obs_data_release(source_settings);
	if (!source) {
		fprintf(stderr, "DRIVER: could not create the colour source\n");
		return 1;
	}
	obs_set_output_source(0, source);

	obs_data_t *venc_settings = obs_data_create();
	obs_data_set_int(venc_settings, "bitrate", 1200);
	obs_data_set_int(venc_settings, "keyint_sec", 2);
	obs_data_set_string(venc_settings, "preset", "ultrafast");
	obs_data_set_string(venc_settings, "profile", "baseline");
	obs_data_set_string(venc_settings, "rate_control", "CBR");
	obs_encoder_t *venc = obs_video_encoder_create("obs_x264", "video", venc_settings, NULL);
	obs_data_release(venc_settings);

	obs_data_t *aenc_settings = obs_data_create();
	obs_data_set_int(aenc_settings, "bitrate", 96);
	obs_encoder_t *aenc = obs_audio_encoder_create("ffmpeg_aac", "audio", aenc_settings, 0, NULL);
	obs_data_release(aenc_settings);

	if (!venc || !aenc) {
		fprintf(stderr, "DRIVER: could not create encoders (video=%p audio=%p)\n", (void *)venc, (void *)aenc);
		return 1;
	}

	obs_encoder_set_video(venc, obs_get_video());
	obs_encoder_set_audio(aenc, obs_get_audio());

	obs_data_t *service_settings = obs_data_create();
	obs_data_set_string(service_settings, "server", server);
	obs_data_set_string(service_settings, "key", key);
	obs_service_t *service = obs_service_create("rtmp_custom", "service", service_settings, NULL);
	obs_data_release(service_settings);
	if (!service) {
		fprintf(stderr, "DRIVER: could not create the service\n");
		return 1;
	}

	obs_data_t *output_settings = obs_data_create();
	obs_data_set_bool(output_settings, "ertmp_reconnect_request", reconnect);
	obs_data_set_int(output_settings, "ertmp_reconnect_request_limit", limit);
	obs_output_t *output = obs_output_create("rtmp_output", "stream", output_settings, NULL);
	obs_data_release(output_settings);
	if (!output) {
		fprintf(stderr, "DRIVER: could not create the output\n");
		return 1;
	}

	obs_output_set_video_encoder(output, venc);
	obs_output_set_audio_encoder(output, aenc, 0);
	obs_output_set_service(output, service);

	signal_handler_connect(obs_output_get_signal_handler(output), "stop", on_stop, NULL);

	/* The harness drives reconnects itself; libobs retrying underneath
	 * would make "did it stay up" impossible to read. */
	obs_output_set_reconnect_settings(output, 0, 1);

	if (!obs_output_start(output)) {
		fprintf(stderr, "DRIVER: obs_output_start failed: %s\n", obs_output_get_last_error(output));
		return 1;
	}

	fprintf(stderr, "DRIVER: streaming to %s for %d seconds (reconnect=%s limit=%d)\n", server, seconds, enable_arg,
		limit);

	uint64_t deadline = os_gettime_ns() + (uint64_t)seconds * 1000000000ULL;
	while (os_gettime_ns() < deadline && !stopped_early)
		os_sleep_ms(100);

	int rc = 0;
	if (stopped_early) {
		fprintf(stderr, "DRIVER: RESULT stopped_early code=%d reason='%s' bytes=%llu frames_dropped=%d\n",
			stop_code, stop_reason, (unsigned long long)obs_output_get_total_bytes(output),
			obs_output_get_frames_dropped(output));
		rc = 2;
	} else {
		fprintf(stderr, "DRIVER: RESULT ran_to_completion bytes=%llu frames_dropped=%d\n",
			(unsigned long long)obs_output_get_total_bytes(output), obs_output_get_frames_dropped(output));
		obs_output_stop(output);
		for (int i = 0; i < 100 && obs_output_active(output); i++)
			os_sleep_ms(100);
	}

	obs_output_release(output);
	obs_service_release(service);
	obs_encoder_release(venc);
	obs_encoder_release(aenc);
	obs_set_output_source(0, NULL);
	obs_source_release(source);
	obs_shutdown();
	return rc;
}

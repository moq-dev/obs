#include <obs.hpp>

#include "moq-output.h"
#include "util/util_uint64.h"

extern "C" {
#include "moq.h"
}

MoQOutput::MoQOutput(obs_data_t *, obs_output_t *output)
	: output(output),
	  server_url(),
	  path(),
	  total_bytes_sent(0),
	  connect_time_ms(0),
	  origin(moq_origin_create()),
	  session(0),
	  broadcast(moq_publish_create()),
	  broadcast_published(false)
{
}

bool MoQOutput::PublishBroadcast()
{
	if (broadcast_published) {
		return true;
	}

	LOG_INFO("Publishing broadcast: %s", path.c_str());

	auto result = moq_origin_publish(origin, path.data(), path.size(), broadcast);
	if (result < 0) {
		LOG_ERROR("Failed to publish broadcast to session: %d", result);
		return false;
	}

	broadcast_published = true;
	return true;
}

MoQOutput::~MoQOutput()
{
	moq_publish_close(broadcast);
	moq_origin_close(origin);

	Stop();
}

bool MoQOutput::Start()
{
	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		LOG_ERROR("Failed to get service from output");
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}

	if (!obs_output_can_begin_data_capture(output, 0)) {
		LOG_ERROR("Cannot begin data capture");
		return false;
	}

	if (!obs_output_initialize_encoders(output, 0)) {
		LOG_ERROR("Failed to initialize encoders");
		return false;
	}

	const char *server_value = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	server_url = server_value ? server_value : "";
	if (server_url.empty()) {
		LOG_ERROR("Server URL is empty");
		obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	// Path (broadcast name) is optional; an empty string publishes to the unnamed broadcast.
	const char *path_value = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	path = path_value ? path_value : "";

	bool found_encoder = false;
	for (uint32_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		if (obs_output_get_video_encoder2(output, idx)) {
			found_encoder = true;
			break;
		}
	}

	if (!found_encoder) {
		LOG_ERROR("Failed to get video encoder");
		return false;
	}

	LOG_INFO("Connecting to MoQ server: %s", server_url.c_str());

	connect_start = std::chrono::steady_clock::now();

	session_connected = false;

	// The status callback convention differs across libmoq versions:
	// - 0.2.x: 0 = connected, nonzero = closed/error.
	// - 0.3.x: positive = connection epoch (1 = first connect, >1 = reconnect),
	//   0 = terminal clean close, negative = terminal error.
	// Treat the FIRST non-negative callback as "connected" so the broadcast is
	// published at connect time under either convention; afterwards, 0 means a
	// clean close (0.3.x) and negative codes are errors in both.
	auto session_connect_callback = [](void *user_data, int error_code) {
		auto self = static_cast<MoQOutput *>(user_data);

		if (error_code >= 0 && !self->session_connected) {
			self->session_connected = true;

			if (!self->PublishBroadcast()) {
				obs_output_signal_stop(self->output, OBS_OUTPUT_ERROR);
				return;
			}

			auto elapsed = std::chrono::steady_clock::now() - self->connect_start;
			self->connect_time_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
			LOG_INFO("MoQ session connected (status %d, %d ms): %s", error_code, self->connect_time_ms,
				 self->server_url.c_str());
		} else if (error_code > 0) {
			// 0.3.x reconnect epoch; the broadcast publish is latched, so just log.
			LOG_INFO("MoQ session reconnected (epoch %d): %s", error_code, self->server_url.c_str());
		} else if (error_code == 0) {
			LOG_INFO("MoQ session closed cleanly: %s", self->server_url.c_str());
		} else {
			LOG_INFO("MoQ session failed (%d): %s", error_code, self->server_url.c_str());
		}
	};

	// Start establishing a session with the MoQ server
	// NOTE: You could publish the same broadcasts to multiple sessions if you want (redundant ingest).
	session = moq_session_connect(server_url.data(), server_url.size(), origin, 0, session_connect_callback, this);
	if (session < 0) {
		LOG_ERROR("Failed to initialize MoQ server: %d", session);
		return false;
	}

	obs_output_begin_data_capture(output, 0);

	return true;
}

void MoQOutput::Stop(bool signal)
{
	// Close the session
	if (session > 0) {
		moq_session_close(session);
		session = 0;
	}

	for (auto &[encoder, handle] : video_tracks) {
		if (handle > 0)
			moq_publish_media_close(handle);
	}
	video_tracks.clear();
	video_init_attempts.clear();

	for (auto &[encoder, handle] : audio_tracks) {
		if (handle > 0)
			moq_publish_media_close(handle);
	}
	audio_tracks.clear();

	if (signal) {
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);
	}

	return;
}

void MoQOutput::Data(struct encoder_packet *packet)
{
	if (!packet) {
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (packet->type == OBS_ENCODER_AUDIO) {
		AudioData(packet);
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		VideoData(packet);
	}
}

void MoQOutput::AudioData(struct encoder_packet *packet)
{
	obs_encoder_t *encoder = packet->encoder;

	auto it = audio_tracks.find(encoder);
	if (it == audio_tracks.end()) {
		AudioInit(encoder);
		it = audio_tracks.find(encoder);
	}
	if (it == audio_tracks.end() || it->second < 0) {
		// We failed to initialize the audio track, so we can't write any data.
		return;
	}
	int handle = it->second;

	// Add ~1 second offset to handle negative PTS from audio priming frames.
	// TODO: This is slightly wrong when den is not evenly divisible by num, but close enough.
	int64_t pts = packet->pts + packet->timebase_den / packet->timebase_num;
	if (pts < 0) {
		LOG_WARNING("Dropping audio frame with negative PTS: %lld", (long long)packet->pts);
		return;
	}

	auto pts_us = util_mul_div64(pts, 1000000ULL * packet->timebase_num, packet->timebase_den);

	auto result = moq_publish_media_frame(handle, packet->data, packet->size, pts_us);
	if (result < 0) {
		LOG_ERROR("Failed to write audio frame: %d", result);
		return;
	}

	total_bytes_sent += packet->size;
}

void MoQOutput::VideoData(struct encoder_packet *packet)
{
	obs_encoder_t *encoder = packet->encoder;

	auto it = video_tracks.find(encoder);
	if (it == video_tracks.end()) {
		VideoInit(encoder);
		it = video_tracks.find(encoder);
	}
	if (it == video_tracks.end() || it->second < 0)
		return;
	int handle = it->second;

	// Add ~1 second offset to match audio for A/V sync.
	// TODO: This is slightly wrong when den is not evenly divisible by num, but close enough.
	int64_t pts = packet->pts + packet->timebase_den / packet->timebase_num;
	if (pts < 0) {
		LOG_WARNING("Dropping video frame with negative PTS: %lld", (long long)packet->pts);
		return;
	}

	auto pts_us = util_mul_div64(pts, 1000000ULL * packet->timebase_num, packet->timebase_den);

	auto result = moq_publish_media_frame(handle, packet->data, packet->size, pts_us);
	if (result < 0) {
		LOG_ERROR("Failed to write video frame: %d", result);
		return;
	}

	total_bytes_sent += packet->size;
}

void MoQOutput::VideoInit(obs_encoder_t *encoder)
{
	if (!encoder) {
		LOG_ERROR("Failed to get video encoder");
		return;
	}

	// TODO Pass these along to the video catalog somehow.
	/*
	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	if (!settings) {
		LOG_ERROR("Failed to get video encoder settings");
		return;
	}

	auto video_bitrate = (int)obs_data_get_int(settings, "bitrate");
	auto video_width = obs_encoder_get_width(encoder);
	auto video_height = obs_encoder_get_height(encoder);
	*/

	const char *codec = obs_encoder_get_codec(encoder);
	if (!codec) {
		LOG_ERROR("Failed to get video codec");
		return;
	}

	uint8_t *extra_data = nullptr;
	size_t extra_size = 0;
	obs_encoder_get_extra_data(encoder, &extra_data, &extra_size);

	// H.264/H.265 carry their parameter sets (SPS/PPS) out-of-band in the encoder's
	// "extra data", which is only populated after the first frame has been encoded.
	// These become the decoder `description` in the catalog; without them the browser's
	// VideoDecoder rejects every frame ("a key frame is required after configure()...
	// you must fill out the description field in the VideoDecoderConfig"). If they
	// aren't ready yet, leave the track uninitialized so VideoData retries on the next
	// packet, once the encoder has produced them. (AV1 is excluded: it carries its
	// sequence header in-band and doesn't depend on this out-of-band extra data.)
	bool needs_headers = (strcmp(codec, "h264") == 0) || (strcmp(codec, "hevc") == 0);
	if (needs_headers && extra_size == 0) {
		// Bound the retry so a genuinely broken encoder surfaces an error instead of
		// silently dropping video forever.
		int attempts = ++video_init_attempts[encoder];
		const int max_attempts = 30; // ~1s at 30fps; headers normally arrive within 1-2 frames
		if (attempts <= max_attempts) {
			LOG_WARNING("Video codec headers (SPS/PPS) not ready yet; deferring track init (attempt %d)", attempts);
			return;
		}
		LOG_ERROR("Video codec headers (SPS/PPS) still missing after %d attempts; publishing track without them (video may fail to decode)",
			  attempts);
	}
	video_init_attempts.erase(encoder);

	// Transform codec string for MoQ
	const char *moq_codec = codec;
	if (strcmp(codec, "h264") == 0) {
		// H.264 with inline SPS/PPS
		moq_codec = "avc3";
	} else if (strcmp(codec, "hevc") == 0) {
		// H.265 with inline VPS/SPS/PPS
		moq_codec = "hev1";
	}

	// Intialize the media import module with the codec and initialization data.
	int handle = moq_publish_media_ordered(broadcast, moq_codec, strlen(moq_codec), extra_data, extra_size);
	video_tracks[encoder] = handle;
	if (handle < 0) {
		LOG_ERROR("Failed to initialize video track: %d", handle);
		return;
	}

	LOG_INFO("Video track initialized successfully");
}

void MoQOutput::AudioInit(obs_encoder_t *encoder)
{
	if (!encoder) {
		LOG_ERROR("Failed to get audio encoder");
		return;
	}

	// TODO Pass these along to the audio catalog somehow.
	/*
	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	if (!settings) {
		LOG_ERROR("Failed to get audio encoder settings");
		return;
	}

	auto audio_bitrate = (int)obs_data_get_int(settings, "bitrate");
	*/

	uint8_t *extra_data = nullptr;
	size_t extra_size = 0;

	// obs_encoder_get_extra_data may only return data after the first frame has been encoded.
	// For AAC, this returns 2 bytes containing the profile and the sample rate.
	if (!obs_encoder_get_extra_data(encoder, &extra_data, &extra_size)) {
		LOG_WARNING("Failed to get extra data");
	}

	const char *codec = obs_encoder_get_codec(encoder);

	int handle = moq_publish_media_ordered(broadcast, codec, strlen(codec), extra_data, extra_size);
	audio_tracks[encoder] = handle;
	if (handle < 0) {
		LOG_ERROR("Failed to initialize audio track: %d", handle);
		return;
	}

	LOG_INFO("Audio track initialized successfully");
}

void register_moq_output()
{
	const uint32_t base_flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE | OBS_OUTPUT_MULTI_TRACK_VIDEO |
				    OBS_OUTPUT_MULTI_TRACK_AUDIO;

	const char *audio_codecs = "aac;opus";
	const char *video_codecs = "h264;hevc;av1";

	struct obs_output_info info = {};
	info.id = "moq_output";
	info.flags = OBS_OUTPUT_AV | base_flags;
	info.get_name = [](void *) -> const char * {
		return "MoQ Output";
	};
	info.create = [](obs_data_t *settings, obs_output_t *output) -> void * {
		return new MoQOutput(settings, output);
	};
	info.destroy = [](void *priv_data) {
		delete static_cast<MoQOutput *>(priv_data);
	};
	info.start = [](void *priv_data) -> bool {
		return static_cast<MoQOutput *>(priv_data)->Start();
	};
	info.stop = [](void *priv_data, uint64_t) {
		static_cast<MoQOutput *>(priv_data)->Stop();
	};
	info.encoded_packet = [](void *priv_data, struct encoder_packet *packet) {
		static_cast<MoQOutput *>(priv_data)->Data(packet);
	};
	info.get_total_bytes = [](void *priv_data) -> uint64_t {
		return (uint64_t)static_cast<MoQOutput *>(priv_data)->GetTotalBytes();
	};
	info.get_connect_time_ms = [](void *priv_data) -> int {
		return static_cast<MoQOutput *>(priv_data)->GetConnectTime();
	};
	info.encoded_video_codecs = video_codecs;
	info.encoded_audio_codecs = audio_codecs;
	info.protocols = "MoQ";

	obs_register_output(&info);

	info.id = "moq_output_video";
	info.flags = OBS_OUTPUT_VIDEO | base_flags;
	info.encoded_audio_codecs = nullptr;
	obs_register_output(&info);

	info.id = "moq_output_audio";
	info.flags = OBS_OUTPUT_AUDIO | base_flags;
	info.encoded_video_codecs = nullptr;
	info.encoded_audio_codecs = audio_codecs;
	obs_register_output(&info);
}

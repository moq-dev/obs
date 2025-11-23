#include <iostream>

#include "moq-output.h"

#include <obs.hpp>

extern "C" {
    extern void hang_start_from_c();
    extern void hang_stop_from_c();
    extern void hang_write_video_packet_from_c(void *data, size_t size, int keyframe, long dts);
}

MoQOutput::MoQOutput(obs_data_t *, obs_output_t *output)
	: output(output),
	  server_url(),
	  token(),
	  stream_name(),
	  total_bytes_sent(0),
	  connect_time_ms(0)
{
}

MoQOutput::~MoQOutput()
{
	Stop();
}

bool MoQOutput::Start()
{
	if (!obs_output_can_begin_data_capture(output, 0))
		return false;

	if (!obs_output_initialize_encoders(output, 0))
		return false;

	const obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0);

	if (!encoder)
		return false;

    // std::cout << "joy: " << obs_encoder_get_width(encoder) << std::endl;

    obs_data_t *encoder_settings = obs_encoder_get_settings(encoder);

    // const char *profile_str = obs_data_get_string(encoder_settings, "profile");

    // std::cout << "joy: " << profile_str << std::endl;

    std::cout << "json: " << obs_data_get_json_pretty(encoder_settings) << std::endl;

    obs_data_release(encoder_settings);
    
    hang_start_from_c();
    
	obs_output_begin_data_capture(output, 0);

	return true;
}

void MoQOutput::Stop(bool signal)
{
	if (signal) {
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);

        hang_stop_from_c();
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
		// TODO: Handle Audio packet
		return;
	} else if (packet->type == OBS_ENCODER_VIDEO) {
        hang_write_video_packet_from_c(packet->data, packet->size, packet->keyframe, packet->dts_usec);
		return;
	}
}

void register_moq_output()
{
	const uint32_t base_flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;

	const char *audio_codecs = "aac";
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

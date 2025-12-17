/*
NVDEC Hardware Video Decoder for OBS Hang Source
Copyright (C) 2024 OBS Plugin Template

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include "logger-c.h"
#include <util/threading.h>
#include <graphics/graphics.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

// FFmpeg headers for hardware acceleration
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#ifdef HAVE_NVDEC
#include <libavutil/hwcontext_cuda.h>
#endif

#include "hang-source.h"

// Function declarations
static bool nvdec_init_cuda_decoder(struct nvdec_decoder *decoder);
static bool nvdec_decode_frame(struct nvdec_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context);
static bool software_decode_frame(struct nvdec_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context);
static void store_decoded_frame(struct hang_source *context, uint8_t *data, size_t size, uint32_t width, uint32_t height);
static bool convert_mp4_nal_units_to_annex_b(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_size);

struct nvdec_decoder {
	// FFmpeg hardware acceleration context
	AVBufferRef *hw_device_ctx;
	AVCodecContext *codec_ctx;
	struct SwsContext *sws_ctx;

	// Video format information
	uint32_t width;
	uint32_t height;
	enum AVPixelFormat pix_fmt;

	// Reference to parent context for frame storage
	struct hang_source *context;
};

bool nvdec_decoder_init(struct hang_source *context)
{
	struct nvdec_decoder *decoder = bzalloc(sizeof(struct nvdec_decoder));
	decoder->context = context;
	context->nvdec_context = decoder;

	// Try to initialize CUDA hardware acceleration with FFmpeg
	if (!nvdec_init_cuda_decoder(decoder)) {
		CLOG_WARNING( "CUDA hardware acceleration initialization failed, falling back to software decoding");
		goto software_fallback;
	}

	CLOG_INFO( "CUDA hardware accelerated decoder initialized successfully");
	return true;

software_fallback:
	// Fallback to software decoding
	CLOG_INFO( "Using software decoding fallback");

	// Initialize FFmpeg software decoder as fallback
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		CLOG_ERROR( "H.264 codec not found");
		bfree(decoder);
		context->nvdec_context = NULL;
		return false;
	}

	decoder->codec_ctx = avcodec_alloc_context3(codec);
	if (!decoder->codec_ctx) {
		CLOG_ERROR( "Failed to allocate codec context");
		bfree(decoder);
		context->nvdec_context = NULL;
		return false;
	}

	if (avcodec_open2(decoder->codec_ctx, codec, NULL) < 0) {
		CLOG_ERROR( "Failed to open codec");
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		context->nvdec_context = NULL;
		return false;
	}

	decoder->pix_fmt = AV_PIX_FMT_YUV420P; // Default format
	CLOG_INFO( "FFmpeg software decoder initialized as fallback");
	return true;
}

void nvdec_decoder_destroy(struct hang_source *context)
{
	struct nvdec_decoder *decoder = context->nvdec_context;
	if (!decoder) {
		return;
	}

	if (decoder->codec_ctx) {
		avcodec_free_context(&decoder->codec_ctx);
		decoder->codec_ctx = NULL;
	}

	if (decoder->hw_device_ctx) {
		av_buffer_unref(&decoder->hw_device_ctx);
		decoder->hw_device_ctx = NULL;
	}

	if (decoder->sws_ctx) {
		sws_freeContext(decoder->sws_ctx);
		decoder->sws_ctx = NULL;
	}

	bfree(decoder);
	context->nvdec_context = NULL;
}

bool nvdec_decoder_decode(struct hang_source *context, const uint8_t *data, size_t size, uint64_t pts, bool keyframe)
{
	UNUSED_PARAMETER(keyframe);
	struct nvdec_decoder *decoder = context->nvdec_context;
	if (!decoder) {
		return false;
	}

	// Try CUDA hardware acceleration first, fallback to software
	if (decoder->hw_device_ctx && decoder->codec_ctx) {
		return nvdec_decode_frame(decoder, data, size, pts, context);
	} else if (decoder->codec_ctx) {
		return software_decode_frame(decoder, data, size, pts, context);
	} else {
		return false;
	}
}

// Initialize CUDA hardware acceleration with FFmpeg
#ifdef HAVE_NVDEC
static bool nvdec_init_cuda_decoder(struct nvdec_decoder *decoder)
{
	int ret;

	// Create CUDA device context
	ret = av_hwdevice_ctx_create(&decoder->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0);
	if (ret < 0) {
		CLOG_DEBUG( "Failed to create CUDA device context: %s", av_err2str(ret));
		return false;
	}

	// Get the codec
	const AVCodec *codec = avcodec_find_decoder_by_name("h264_cuvid");
	if (!codec) {
		CLOG_DEBUG( "CUDA H.264 decoder not found, trying generic hardware decoder");
		codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) {
			CLOG_ERROR( "H.264 codec not found");
			av_buffer_unref(&decoder->hw_device_ctx);
			return false;
		}
	}

	// Create codec context
	decoder->codec_ctx = avcodec_alloc_context3(codec);
	if (!decoder->codec_ctx) {
		CLOG_ERROR( "Failed to allocate codec context");
		av_buffer_unref(&decoder->hw_device_ctx);
		return false;
	}

	// Set hardware device context
	decoder->codec_ctx->hw_device_ctx = av_buffer_ref(decoder->hw_device_ctx);

	// Configure codec for hardware acceleration
	if (strcmp(codec->name, "h264_cuvid") == 0) {
		// NVIDIA CUDA decoder
		decoder->codec_ctx->extra_hw_frames = 1;
	}

	// Open the codec
	ret = avcodec_open2(decoder->codec_ctx, codec, NULL);
	if (ret < 0) {
		CLOG_ERROR( "Failed to open CUDA codec: %s", av_err2str(ret));
		avcodec_free_context(&decoder->codec_ctx);
		av_buffer_unref(&decoder->hw_device_ctx);
		return false;
	}

	decoder->pix_fmt = AV_PIX_FMT_CUDA; // Hardware format
	CLOG_INFO( "CUDA hardware decoder initialized with codec: %s", codec->name);
	return true;
}
#else
// CUDA not available, always return false
static bool nvdec_init_cuda_decoder(struct nvdec_decoder *decoder)
{
	UNUSED_PARAMETER(decoder);
	return false;
}
#endif

// Decode frame using CUDA hardware acceleration
#ifdef HAVE_NVDEC
static bool nvdec_decode_frame(struct nvdec_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context)
{
	// Convert MP4 NAL units to Annex B format
	uint8_t *converted_data = NULL;
	size_t converted_size = 0;

	if (!convert_mp4_nal_units_to_annex_b(data, size, &converted_data, &converted_size)) {
		CLOG_ERROR( "Failed to convert NAL units");
		return false;
	}

	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		CLOG_ERROR( "Failed to allocate AVPacket");
		bfree(converted_data);
		return false;
	}

	packet->data = converted_data;
	packet->size = converted_size;
	packet->pts = pts;

	int ret = avcodec_send_packet(decoder->codec_ctx, packet);
	av_packet_free(&packet);

	if (ret < 0) {
		CLOG_ERROR( "Error sending packet to CUDA decoder: %s", av_err2str(ret));
		bfree(converted_data);
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		CLOG_ERROR( "Failed to allocate AVFrame");
		bfree(converted_data);
		return false;
	}

	ret = avcodec_receive_frame(decoder->codec_ctx, frame);
	if (ret < 0) {
		if (ret != AVERROR(EAGAIN)) {
			CLOG_ERROR( "Error receiving frame from CUDA decoder: %s", av_err2str(ret));
		}
		av_frame_free(&frame);
		bfree(converted_data);
		return false;
	}

	// Check if frame is in hardware memory
	if (frame->format == AV_PIX_FMT_CUDA) {
		// Transfer from GPU to CPU
		AVFrame *sw_frame = av_frame_alloc();
		if (!sw_frame) {
			CLOG_ERROR( "Failed to allocate software frame");
			av_frame_free(&frame);
			bfree(converted_data);
			return false;
		}

		sw_frame->format = AV_PIX_FMT_NV12; // Intermediate format
		ret = av_hwframe_transfer_data(sw_frame, frame, 0);
		av_frame_free(&frame);
		frame = sw_frame;

		if (ret < 0) {
			CLOG_ERROR( "Failed to transfer frame from GPU to CPU: %s", av_err2str(ret));
			av_frame_free(&frame);
			bfree(converted_data);
			return false;
		}
	}

	// Convert to RGBA for OBS
	if (!decoder->sws_ctx) {
		decoder->sws_ctx = sws_getContext(
			frame->width, frame->height, (enum AVPixelFormat)frame->format,
			frame->width, frame->height, AV_PIX_FMT_RGBA,
			SWS_BILINEAR | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
		if (!decoder->sws_ctx) {
			CLOG_ERROR( "Failed to create SWS context");
			av_frame_free(&frame);
			bfree(converted_data);
			return false;
		}
	}

	// Allocate buffer for RGBA data
	size_t rgba_size = frame->width * frame->height * 4;
	uint8_t *rgba_data = bzalloc(rgba_size);
	if (!rgba_data) {
		CLOG_ERROR( "Failed to allocate RGBA buffer");
		av_frame_free(&frame);
		bfree(converted_data);
		return false;
	}

	// Convert frame to RGBA
	uint8_t *dst_data[4] = {rgba_data, NULL, NULL, NULL};
	int dst_linesize[4] = {frame->width * 4, 0, 0, 0};

	int scale_ret = sws_scale(decoder->sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
	          0, frame->height, dst_data, dst_linesize);

	if (scale_ret < 0) {
		CLOG_ERROR( "sws_scale failed: %s", av_err2str(ret));
		bfree(rgba_data);
		av_frame_free(&frame);
		bfree(converted_data);
		return false;
	}

	// Store the decoded frame
	store_decoded_frame(context, rgba_data, rgba_size, frame->width, frame->height);

	av_frame_free(&frame);
	bfree(converted_data);
	return true;
}
#else
// CUDA not available, fallback to software decoding
static bool nvdec_decode_frame(struct nvdec_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context)
{
	UNUSED_PARAMETER(decoder);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(pts);
	UNUSED_PARAMETER(context);
	return false;
}
#endif

static bool convert_mp4_nal_units_to_annex_b(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_size)
{
	// Estimate output size (add 4 bytes for each start code, remove 4 bytes for each length)
	size_t estimated_size = size + 1024; // Add some padding
	uint8_t *buffer = bzalloc(estimated_size);
	if (!buffer) {
		return false;
	}

	size_t out_pos = 0;
	size_t pos = 0;

	while (pos + 4 <= size) {
		// Read 4-byte length
		uint32_t nal_length = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
		pos += 4;

		if (pos + nal_length > size) {
			CLOG_ERROR( "Invalid NAL length: %u (pos=%zu, size=%zu)", nal_length, pos, size);
			bfree(buffer);
			return false;
		}

		// Check if we need more space
		if (out_pos + 4 + nal_length > estimated_size) {
			estimated_size = out_pos + 4 + nal_length + 1024;
			uint8_t *new_buffer = brealloc(buffer, estimated_size);
			if (!new_buffer) {
				bfree(buffer);
				return false;
			}
			buffer = new_buffer;
		}

		// Write start code
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x01;

		// Copy NAL data
		memcpy(buffer + out_pos, data + pos, nal_length);
		out_pos += nal_length;
		pos += nal_length;
	}

	*out_data = buffer;
	*out_size = out_pos;
	return true;
}

static bool software_decode_frame(struct nvdec_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context)
{

	// For MP4 H.264 (avc1), convert length-prefixed NAL units to start-code format
	uint8_t *converted_data = NULL;
	size_t converted_size = 0;

	if (!convert_mp4_nal_units_to_annex_b(data, size, &converted_data, &converted_size)) {
		CLOG_ERROR( "Failed to convert NAL units");
		return false;
	}

	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		CLOG_ERROR( "Failed to allocate AVPacket");
		bfree(converted_data);
		return false;
	}

	packet->data = converted_data;
	packet->size = converted_size;
	packet->pts = pts;

	int ret = avcodec_send_packet(decoder->codec_ctx, packet);
	av_packet_free(&packet);
	packet = NULL; // Set to NULL after freeing to prevent double free

	if (ret < 0) {
		CLOG_ERROR( "Error sending packet to decoder: %s", av_err2str(ret));
		bfree(converted_data);
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		CLOG_ERROR( "Failed to allocate AVFrame");
		bfree(converted_data);
		return false;
	}

	ret = avcodec_receive_frame(decoder->codec_ctx, frame);
	if (ret < 0) {
		if (ret != AVERROR(EAGAIN)) {
			CLOG_ERROR( "Error receiving frame from decoder: %s", av_err2str(ret));
		}
		av_frame_free(&frame);
		bfree(converted_data);
		return false;
	}

	// Convert frame to RGBA for OBS
	if (!decoder->sws_ctx) {
		decoder->sws_ctx = sws_getContext(
			frame->width, frame->height, (enum AVPixelFormat)frame->format,
			frame->width, frame->height, AV_PIX_FMT_RGBA,
			SWS_BILINEAR | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
		if (!decoder->sws_ctx) {
			CLOG_ERROR( "Failed to create SWS context");
			av_frame_free(&frame);
			return false;
		}
	}

	// Allocate buffer for RGBA data
	size_t rgba_size = frame->width * frame->height * 4; // 4 bytes per pixel for RGBA
	uint8_t *rgba_data = bzalloc(rgba_size);
	if (!rgba_data) {
		CLOG_ERROR( "Failed to allocate RGBA buffer");
		av_frame_free(&frame);
		bfree(converted_data);
		return false;
	}

	// Convert frame to RGBA
	uint8_t *dst_data[4] = {rgba_data, NULL, NULL, NULL};
	int dst_linesize[4] = {frame->width * 4, 0, 0, 0};

	int scale_ret = sws_scale(decoder->sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
	          0, frame->height, dst_data, dst_linesize);

	if (scale_ret < 0) {
		CLOG_ERROR( "sws_scale failed: %s", av_err2str(ret));
		bfree(rgba_data);
		av_frame_free(&frame);
		bfree(converted_data);
		return false;
	}

	// Store the decoded frame
	store_decoded_frame(context, rgba_data, rgba_size, frame->width, frame->height);

	av_frame_free(&frame);
	bfree(converted_data);
	return true;
}

static void store_decoded_frame(struct hang_source *context, uint8_t *data, size_t size, uint32_t width, uint32_t height)
{
	if (!context || !data) {
		return;
	}

	pthread_mutex_lock(&context->frame_mutex);

	// Check if source is still active before storing frame
	// This prevents storing frames after deactivation has started cleanup
	if (!context->active) {
		pthread_mutex_unlock(&context->frame_mutex);
		bfree(data); // Free the data we allocated since we're not using it
		return;
	}

	// Free previous frame data
	if (context->current_frame_data) {
		bfree(context->current_frame_data);
	}

	// Store new frame data
	context->current_frame_data = data;
	context->current_frame_size = size;
	context->current_frame_width = width;
	context->current_frame_height = height;

	pthread_mutex_unlock(&context->frame_mutex);
}



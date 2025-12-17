/*
Hang MoQ Source for OBS
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
#include <util/platform.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <string.h>

// Include the moq library header
#include <moq.h>

// Include our headers
#include "hang-source.h"
#include "nvdec-decoder.h"
#include "audio-decoder.h"

static const char *hang_source_get_name(void *type_data);
static void *hang_source_create(obs_data_t *settings, obs_source_t *source);
static void hang_source_destroy(void *data);
static void hang_source_update(void *data, obs_data_t *settings);
static void hang_source_activate(void *data);
static void hang_source_deactivate(void *data);
static void hang_source_video_render(void *data, gs_effect_t *effect);
static uint32_t hang_source_get_width(void *data);
static uint32_t hang_source_get_height(void *data);
static obs_properties_t *hang_source_get_properties(void *data);
static void hang_source_get_defaults(obs_data_t *settings);

// FFmpeg audio decoder functions (declared in audio-decoder.h)

// MoQ callback functions (new API)
static void on_session_status(void *user_data, int32_t code);
static void on_catalog(void *user_data, int32_t catalog_id);
static void on_video_frame(void *user_data, int32_t frame_id);
static void on_audio_frame(void *user_data, int32_t frame_id);


struct obs_source_info hang_source_info = {
	.id = "hang_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = hang_source_get_name,
	.create = hang_source_create,
	.destroy = hang_source_destroy,
	.update = hang_source_update,
	.activate = hang_source_activate,
	.deactivate = hang_source_deactivate,
	.video_render = hang_source_video_render,
	.get_width = hang_source_get_width,
	.get_height = hang_source_get_height,
	.get_properties = hang_source_get_properties,
	.get_defaults = hang_source_get_defaults,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};

static const char *hang_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("HangSource");
}

static void *hang_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct hang_source *context = bzalloc(sizeof(struct hang_source));
	context->source = source;

	// Initialize threading primitives
	pthread_mutex_init(&context->frame_mutex, NULL);
	pthread_cond_init(&context->frame_cond, NULL);
	pthread_mutex_init(&context->audio_mutex, NULL);
	pthread_cond_init(&context->audio_cond, NULL);
	pthread_mutex_init(&context->decoder_mutex, NULL);

	// Initialize frame storage
	context->current_frame_data = NULL;
	context->current_frame_size = 0;
	context->current_frame_width = 0;
	context->current_frame_height = 0;

	// Initialize queues
	context->frame_queue_cap = 16;
	context->frame_queue = bzalloc(sizeof(struct obs_source_frame *) * context->frame_queue_cap);
	context->audio_queue_cap = 16;
	context->audio_queue = bzalloc(sizeof(struct obs_source_audio *) * context->audio_queue_cap);

	hang_source_update(context, settings);
	return context;
}

static void hang_source_destroy(void *data)
{
	struct hang_source *context = data;

	// Stop the source first (this will close all MoQ resources and destroy decoders)
	hang_source_deactivate(context);

	// Clean up MoQ resources (should already be closed by deactivate, but check to be safe)
	if (context->audio_track_id > 0) {
		moq_consume_audio_track_close(context->audio_track_id);
		context->audio_track_id = 0;
	}
	if (context->video_track_id > 0) {
		moq_consume_video_track_close(context->video_track_id);
		context->video_track_id = 0;
	}
	if (context->catalog_consumer_id > 0) {
		moq_consume_catalog_close(context->catalog_consumer_id);
		context->catalog_consumer_id = 0;
	}
	if (context->broadcast_id > 0) {
		moq_consume_close(context->broadcast_id);
		context->broadcast_id = 0;
	}
	if (context->session_id > 0) {
		moq_session_close(context->session_id);
		context->session_id = 0;
	}
	if (context->origin_id > 0) {
		moq_origin_close(context->origin_id);
		context->origin_id = 0;
	}

	// Clean up decoders (should already be destroyed by deactivate, but check to be safe)
	pthread_mutex_lock(&context->decoder_mutex);
	nvdec_decoder_destroy(context);
	audio_decoder_destroy(context);
	pthread_mutex_unlock(&context->decoder_mutex);

	// Clean up video resources
	if (context->texture) {
		gs_texture_destroy(context->texture);
		context->texture = NULL;
	}

	// Clean up frame data (should already be cleaned by deactivate, but check to be safe)
	pthread_mutex_lock(&context->frame_mutex);
	if (context->current_frame_data) {
		bfree(context->current_frame_data);
		context->current_frame_data = NULL;
	}
	pthread_mutex_unlock(&context->frame_mutex);

	// Clean up queues (should already be cleaned by deactivate, but check to be safe)
	pthread_mutex_lock(&context->frame_mutex);
	for (size_t i = 0; i < context->frame_queue_len; i++) {
		obs_source_frame_free(context->frame_queue[i]);
	}
	context->frame_queue_len = 0;
	pthread_mutex_unlock(&context->frame_mutex);

	pthread_mutex_lock(&context->audio_mutex);
	for (size_t i = 0; i < context->audio_queue_len; i++) {
		// Free the audio data channels
		struct obs_source_audio *audio = context->audio_queue[i];
		for (int ch = 0; ch < MAX_AV_PLANES && audio->data[ch]; ch++) {
			bfree((void *)audio->data[ch]);
		}
		bfree(audio);
	}
	context->audio_queue_len = 0;
	pthread_mutex_unlock(&context->audio_mutex);

	bfree(context->frame_queue);
	bfree(context->audio_queue);

	// Clean up threading primitives
	pthread_mutex_destroy(&context->frame_mutex);
	pthread_cond_destroy(&context->frame_cond);
	pthread_mutex_destroy(&context->audio_mutex);
	pthread_cond_destroy(&context->audio_cond);
	pthread_mutex_destroy(&context->decoder_mutex);

	// Clean up strings
	bfree(context->url);
	bfree(context->broadcast_path);

	bfree(context);
}

static void hang_source_update(void *data, obs_data_t *settings)
{
	struct hang_source *context = data;

	const char *url = obs_data_get_string(settings, "url");
	const char *broadcast_path = obs_data_get_string(settings, "broadcast");

	// Check if settings changed
	bool url_changed = !context->url || strcmp(context->url, url) != 0;
	bool broadcast_changed = !context->broadcast_path || strcmp(context->broadcast_path, broadcast_path) != 0;

	if (!url_changed && !broadcast_changed) {
		return;
	}

	// Stop current connection
	hang_source_deactivate(context);

	// Update settings
	bfree(context->url);
	bfree(context->broadcast_path);
	context->url = bstrdup(url);
	context->broadcast_path = bstrdup(broadcast_path);

	// Reconnect if we have valid settings
	if (url_changed || broadcast_changed) {
		if (context->url && context->broadcast_path && strlen(context->url) > 0 && strlen(context->broadcast_path) > 0) {
			hang_source_activate(context);
		}
	}
}

static void hang_source_activate(void *data)
{
	struct hang_source *context = data;

	if (context->active || !context->url || !context->broadcast_path ||
	    strlen(context->url) == 0 || strlen(context->broadcast_path) == 0) {
		return;
	}

	// Basic URL validation - ensure URL has at least scheme://host
	if (strstr(context->url, "://") == NULL) {
		CLOG_ERROR("Invalid URL format: %s (must include scheme like https://)", context->url);
		return;
	}

	const char *host_start = strstr(context->url, "://");
	if (host_start && strlen(host_start + 3) == 0) {
		CLOG_ERROR( "Invalid URL: %s (missing host)", context->url);
		return;
	}

	CLOG_INFO( "Activating hang source with URL: %s, broadcast: %s", context->url, context->broadcast_path);

	// Initialize decoders first (local operation, doesn't need network)
	if (!nvdec_decoder_init(context)) {
		CLOG_ERROR( "Failed to initialize video decoder");
		return;
	}

	if (!audio_decoder_init(context)) {
		CLOG_ERROR( "Failed to initialize audio decoder");
		nvdec_decoder_destroy(context);
		return;
	}

	// 1. Create origin for consumption
	context->origin_id = moq_origin_create();
	if (context->origin_id <= 0) {
		CLOG_ERROR( "Failed to create MoQ origin");
		goto cleanup;
	}

	// 2. Connect session with origin for consumption
	// The on_session_status callback will be called when connected,
	// and will then subscribe to the broadcast and catalog
	context->session_id = moq_session_connect(
		context->url,
		strlen(context->url),
		0,                    // no publish origin
		context->origin_id,   // consume origin
		on_session_status,
		context
	);
	if (context->session_id <= 0) {
		CLOG_ERROR( "Failed to create MoQ session");
		goto cleanup;
	}

	// Mark as active - broadcast/catalog subscription happens in on_session_status
	context->active = true;
	CLOG_INFO( "Hang source activated, waiting for session connection...");
	return;

cleanup:
	if (context->session_id > 0) {
		moq_session_close(context->session_id);
		context->session_id = 0;
	}
	if (context->origin_id > 0) {
		moq_origin_close(context->origin_id);
		context->origin_id = 0;
	}
	nvdec_decoder_destroy(context);
	audio_decoder_destroy(context);
}

static void hang_source_deactivate(void *data)
{
	struct hang_source *context = data;

	if (!context->active) {
		return;
	}

	CLOG_INFO( "Deactivating hang source");

	// Set active to false FIRST to prevent callbacks from processing new data
	context->active = false;

	// Close MoQ resources in reverse order to stop new callbacks
	// 1. Close track subscriptions first
	if (context->audio_track_id > 0) {
		moq_consume_audio_track_close(context->audio_track_id);
		context->audio_track_id = 0;
	}
	if (context->video_track_id > 0) {
		moq_consume_video_track_close(context->video_track_id);
		context->video_track_id = 0;
	}

	// 2. Close catalog consumer
	if (context->catalog_consumer_id > 0) {
		moq_consume_catalog_close(context->catalog_consumer_id);
		context->catalog_consumer_id = 0;
	}

	// 3. Close broadcast consumer
	if (context->broadcast_id > 0) {
		moq_consume_close(context->broadcast_id);
		context->broadcast_id = 0;
	}

	// 4. Close session
	if (context->session_id > 0) {
		moq_session_close(context->session_id);
		context->session_id = 0;
	}

	// 5. Close origin
	if (context->origin_id > 0) {
		moq_origin_close(context->origin_id);
		context->origin_id = 0;
	}

	// Clear current frame and queues BEFORE destroying decoders
	// This prevents callbacks from accessing freed decoder resources
	pthread_mutex_lock(&context->frame_mutex);
	if (context->current_frame_data) {
		bfree(context->current_frame_data);
		context->current_frame_data = NULL;
		context->current_frame_size = 0;
		context->current_frame_width = 0;
		context->current_frame_height = 0;
	}

	// Clear queues
	for (size_t i = 0; i < context->frame_queue_len; i++) {
		obs_source_frame_free(context->frame_queue[i]);
	}
	context->frame_queue_len = 0;
	pthread_mutex_unlock(&context->frame_mutex);

	pthread_mutex_lock(&context->audio_mutex);
	for (size_t i = 0; i < context->audio_queue_len; i++) {
		// Free the audio data channels
		struct obs_source_audio *audio = context->audio_queue[i];
		for (int ch = 0; ch < MAX_AV_PLANES && audio->data[ch]; ch++) {
			bfree((void *)audio->data[ch]);
		}
		bfree(audio);
	}
	context->audio_queue_len = 0;
	pthread_mutex_unlock(&context->audio_mutex);

	// Now safe to destroy decoders - hold mutex to ensure no callbacks are in progress
	// Any callback that passed the initial active check will be waiting on this mutex,
	// and will see active=false when they acquire it
	pthread_mutex_lock(&context->decoder_mutex);
	nvdec_decoder_destroy(context);
	audio_decoder_destroy(context);
	pthread_mutex_unlock(&context->decoder_mutex);

	CLOG_INFO( "Hang source deactivated");
}

static obs_properties_t *hang_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "url", obs_module_text("URL"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "broadcast", obs_module_text("Broadcast"), OBS_TEXT_DEFAULT);

	return props;
}

static void hang_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_string(settings, "broadcast", "");
}

static void hang_source_video_render(void *data, gs_effect_t *effect)
{
	struct hang_source *context = data;

	if (!context->active) {
		return;
	}

	// Get the current frame data
	pthread_mutex_lock(&context->frame_mutex);
	if (context->current_frame_data && context->current_frame_width > 0 && context->current_frame_height > 0) {
		uint32_t width = context->current_frame_width;
		uint32_t height = context->current_frame_height;

		// Create or update texture if needed
		if (!context->texture || context->width != width || context->height != height) {
			if (context->texture) {
				gs_texture_destroy(context->texture);
			}
			context->texture = gs_texture_create(width, height, GS_RGBA, 1, NULL, GS_DYNAMIC);
			context->width = width;
			context->height = height;
		}

		if (context->texture) {
			// Validate RGBA data (check if it's not all black)
			bool has_data = false;
			for (size_t i = 0; i < context->current_frame_size && i < 10000; i += 4) {
				if (context->current_frame_data[i] > 0 || context->current_frame_data[i+1] > 0 ||
				    context->current_frame_data[i+2] > 0) {
					has_data = true;
					break;
				}
			}

			// Upload frame data to texture
			gs_texture_set_image(context->texture, context->current_frame_data, width * 4, false);

			// Render the texture
			gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
			if (param) {
				gs_effect_set_texture(param, context->texture);
				gs_draw_sprite(context->texture, 0, width, height);
			} else {
				CLOG_ERROR( "Effect parameter 'image' not found");
			}
		} else {
			CLOG_ERROR( "No texture available for rendering");
		}
	}
	pthread_mutex_unlock(&context->frame_mutex);
}

static uint32_t hang_source_get_width(void *data)
{
	struct hang_source *context = data;
	return context->current_frame_width > 0 ? context->current_frame_width : 1920;
}

static uint32_t hang_source_get_height(void *data)
{
	struct hang_source *context = data;
	return context->current_frame_height > 0 ? context->current_frame_height : 1080;
}

// MoQ callback implementations (new API)
static void on_session_status(void *user_data, int32_t code)
{
	struct hang_source *context = user_data;

	if (!context) {
		return;
	}

	if (code == 0) {
		CLOG_INFO( "MoQ session connected, subscribing to broadcast...");

		// Now that session is connected, subscribe to the broadcast
		context->broadcast_id = moq_origin_consume(context->origin_id, context->broadcast_path, strlen(context->broadcast_path));
		if (context->broadcast_id <= 0) {
			CLOG_ERROR( "Failed to consume broadcast: %s (error %d)", 
				context->broadcast_path, context->broadcast_id);
			context->active = false;
			return;
		}
		CLOG_INFO( "Subscribed to broadcast: %s (id %d)", 
			context->broadcast_path, context->broadcast_id);

		// Subscribe to catalog updates
		context->catalog_consumer_id = moq_consume_catalog(
			context->broadcast_id,
			on_catalog,
			context
		);
		if (context->catalog_consumer_id <= 0) {
			CLOG_ERROR( "Failed to subscribe to catalog: %d", context->catalog_consumer_id);
			moq_consume_close(context->broadcast_id);
			context->broadcast_id = 0;
			context->active = false;
			return;
		}
		CLOG_INFO( "Subscribed to catalog (id %d)", context->catalog_consumer_id);

	} else if (code < 0) {
		CLOG_ERROR( "MoQ session error: %d", code);
		// Session failed - mark as inactive
		context->active = false;
	}
}

static void on_catalog(void *user_data, int32_t catalog_id)
{
	struct hang_source *context = user_data;

	if (!context || !context->active) {
		return;
	}

	if (catalog_id <= 0) {
		CLOG_ERROR( "Catalog error: %d", catalog_id);
		return;
	}

	CLOG_INFO( "Received catalog update: %d", catalog_id);

	// Close existing track subscriptions if any
	if (context->video_track_id > 0) {
		moq_consume_video_track_close(context->video_track_id);
		context->video_track_id = 0;
	}
	if (context->audio_track_id > 0) {
		moq_consume_audio_track_close(context->audio_track_id);
		context->audio_track_id = 0;
	}

	// Subscribe to first video track (index 0) with 100ms latency
	context->video_track_id = moq_consume_video_track(
		context->broadcast_id,
		0,     // first track
		100,   // 100ms latency
		on_video_frame,
		context
	);
	if (context->video_track_id <= 0) {
		CLOG_WARNING( "Failed to subscribe to video track: %d", context->video_track_id);
	} else {
		CLOG_INFO( "Subscribed to video track: %d", context->video_track_id);
	}

	// Subscribe to first audio track (index 0) with 100ms latency
	context->audio_track_id = moq_consume_audio_track(
		context->broadcast_id,
		0,     // first track
		100,   // 100ms latency
		on_audio_frame,
		context
	);
	if (context->audio_track_id <= 0) {
		CLOG_WARNING( "Failed to subscribe to audio track: %d", context->audio_track_id);
	} else {
		CLOG_INFO( "Subscribed to audio track: %d", context->audio_track_id);
	}
}

static void on_video_frame(void *user_data, int32_t frame_id)
{
	struct hang_source *context = user_data;

	// Quick check before acquiring lock (optimization)
	if (!context || !context->active) {
		if (frame_id > 0) {
			moq_consume_frame_close(frame_id);
		}
		return;
	}

	if (frame_id <= 0) {
		CLOG_ERROR( "Video frame error: %d", frame_id);
		return;
	}

	// Get frame data from libmoq
	moq_frame frame = {0};
	int32_t result = moq_consume_frame_chunk(frame_id, 0, &frame);
	if (result < 0) {
		CLOG_ERROR( "Failed to get video frame chunk: %d", result);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Lock decoder mutex to prevent race with decoder destruction
	pthread_mutex_lock(&context->decoder_mutex);

	// Re-check active state and decoder availability while holding lock
	if (!context->active || !context->nvdec_context) {
		pthread_mutex_unlock(&context->decoder_mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Decode video frame using software decoder (or NVDEC on Linux)
	if (nvdec_decoder_decode(context, frame.payload, frame.payload_size, frame.timestamp_us, frame.keyframe)) {
		// Frame was decoded and queued
	}

	pthread_mutex_unlock(&context->decoder_mutex);

	// Release the frame
	moq_consume_frame_close(frame_id);
}

static void on_audio_frame(void *user_data, int32_t frame_id)
{
	struct hang_source *context = user_data;

	// Quick check before acquiring lock (optimization)
	if (!context || !context->active) {
		if (frame_id > 0) {
			moq_consume_frame_close(frame_id);
		}
		return;
	}

	if (frame_id <= 0) {
		CLOG_ERROR( "Audio frame error: %d", frame_id);
		return;
	}

	// Get frame data from libmoq
	moq_frame frame = {0};
	int32_t result = moq_consume_frame_chunk(frame_id, 0, &frame);
	if (result < 0) {
		CLOG_ERROR( "Failed to get audio frame chunk: %d", result);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Lock decoder mutex to prevent race with decoder destruction
	pthread_mutex_lock(&context->decoder_mutex);

	// Re-check active state and decoder availability while holding lock
	if (!context->active || !context->audio_decoder_context) {
		pthread_mutex_unlock(&context->decoder_mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Decode audio frame using FFmpeg
	if (audio_decoder_decode(context, frame.payload, frame.payload_size, frame.timestamp_us)) {
		// Audio was decoded and queued
	}

	pthread_mutex_unlock(&context->decoder_mutex);

	// Release the frame
	moq_consume_frame_close(frame_id);
}


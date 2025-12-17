/*
FFmpeg Audio Decoder for OBS Hang Source
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

#include "hang-source.h"

struct audio_decoder {
	// Placeholder for future FFmpeg audio decoder implementation
	int placeholder;
};

bool audio_decoder_init(struct hang_source *context)
{
	struct audio_decoder *decoder = bzalloc(sizeof(struct audio_decoder));
	context->audio_decoder_context = decoder;

	decoder->placeholder = 0;

	CLOG_INFO( "Audio decoder placeholder initialized");
	return true;
}

void audio_decoder_destroy(struct hang_source *context)
{
	struct audio_decoder *decoder = context->audio_decoder_context;
	if (!decoder) {
		return;
	}

	bfree(decoder);
	context->audio_decoder_context = NULL;
}

bool audio_decoder_decode(struct hang_source *context, const uint8_t *data, size_t size, uint64_t pts)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(pts);

	// TODO: Implement FFmpeg audio decoding
	return false;
}

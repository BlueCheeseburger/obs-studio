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

#include "obs.h"
#include "obs-internal.h"

bool obs_view_init(struct obs_view *view, enum view_type type)
{
	if (!view)
		return false;

	pthread_mutex_init_value(&view->channels_mutex);

	if (pthread_mutex_init(&view->channels_mutex, NULL) != 0) {
		blog(LOG_ERROR, "obs_view_init: Failed to create mutex");
		return false;
	}

	view->type = type;
	return true;
}

obs_view_t *obs_view_create(void)
{
	struct obs_view *view = bzalloc(sizeof(struct obs_view));

	if (!obs_view_init(view, AUX_VIEW)) {
		bfree(view);
		view = NULL;
	}

	return view;
}

void obs_view_free(struct obs_view *view)
{
	if (!view)
		return;

	for (size_t i = 0; i < MAX_CHANNELS; i++) {
		struct obs_source *source = view->channels[i];
		if (source) {
			obs_source_deactivate(source, view->type);
			obs_source_release(source);
		}
	}

	memset(view->channels, 0, sizeof(view->channels));
	pthread_mutex_destroy(&view->channels_mutex);
}

void obs_view_destroy(obs_view_t *view)
{
	if (view) {
		obs_view_free(view);
		bfree(view);
	}
}

obs_source_t *obs_view_get_source(obs_view_t *view, uint32_t channel)
{
	obs_source_t *source;
	assert(channel < MAX_CHANNELS);

	if (!view)
		return NULL;
	if (channel >= MAX_CHANNELS)
		return NULL;

	pthread_mutex_lock(&view->channels_mutex);
	source = obs_source_get_ref(view->channels[channel]);
	pthread_mutex_unlock(&view->channels_mutex);

	return source;
}

void obs_view_set_source(obs_view_t *view, uint32_t channel, obs_source_t *source)
{
	struct obs_source *prev_source;

	assert(channel < MAX_CHANNELS);

	if (!view)
		return;
	if (channel >= MAX_CHANNELS)
		return;

	pthread_mutex_lock(&view->channels_mutex);
	source = obs_source_get_ref(source);
	prev_source = view->channels[channel];
	view->channels[channel] = source;

	pthread_mutex_unlock(&view->channels_mutex);

	if (source)
		obs_source_activate(source, view->type);

	if (prev_source) {
		obs_source_deactivate(prev_source, view->type);
		obs_source_release(prev_source);
	}
}

void obs_view_render(obs_view_t *view)
{
	if (!view)
		return;

	pthread_mutex_lock(&view->channels_mutex);

	for (size_t i = 0; i < MAX_CHANNELS; i++) {
		struct obs_source *source;

		source = view->channels[i];

		if (source) {
			if (source->removed) {
				obs_source_release(source);
				view->channels[i] = NULL;
			} else {
				obs_source_video_render(source);
			}
		}
	}

	pthread_mutex_unlock(&view->channels_mutex);
}

video_t *obs_view_add(obs_view_t *view)
{
	if (!obs->data.main_canvas->mix)
		return NULL;
	return obs_view_add2(view, &obs->data.main_canvas->mix->ovi);
}

video_t *obs_view_add2(obs_view_t *view, struct obs_video_info *ovi)
{
	if (!view || !ovi)
		return NULL;

	struct obs_core_video_mix *mix = obs_create_video_mix(ovi);
	if (!mix) {
		return NULL;
	}
	mix->view = view;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &mix);
	pthread_mutex_unlock(&obs->video.mixes_mutex);

	return mix->video;
}

void obs_view_remove(obs_view_t *view)
{
	if (!view)
		return;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		if (obs->video.mixes.array[i]->view == view)
			obs->video.mixes.array[i]->view = NULL;
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

video_t *obs_add_output_filtered_mix(uint32_t render_output_filter)
{
	obs_canvas_t *canvas = obs->data.main_canvas;
	if (!canvas || !canvas->mix)
		return NULL;

	struct obs_core_video_mix *mix = obs_create_video_mix(&canvas->mix->ovi);
	if (!mix)
		return NULL;

	mix->view = &canvas->view;
	mix->render_output_filter = render_output_filter;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &mix);
	pthread_mutex_unlock(&obs->video.mixes_mutex);

	return mix->video;
}

video_t *obs_add_cropped_scaled_mix(uint32_t out_width, uint32_t out_height, uint32_t render_output_filter)
{
	obs_canvas_t *canvas = obs->data.main_canvas;
	if (!canvas || !canvas->mix)
		return NULL;
	if (!out_width || !out_height)
		return NULL;

	struct obs_video_info ovi = canvas->mix->ovi;
	uint32_t src_w = ovi.base_width;
	uint32_t src_h = ovi.base_height;
	if (!src_w || !src_h)
		return NULL;

	double src_aspect = (double)src_w / (double)src_h;
	double dst_aspect = (double)out_width / (double)out_height;

	uint32_t crop_w = src_w;
	uint32_t crop_h = src_h;
	float crop_x0 = 0.0f;
	float crop_y0 = 0.0f;

	if (dst_aspect < src_aspect) {
		/* destination is relatively narrower/taller: crop width, keep
		 * full height (e.g. portrait destination from a landscape
		 * canvas) */
		crop_w = (uint32_t)((double)src_h * dst_aspect + 0.5);
		if (crop_w < 2)
			crop_w = 2;
		crop_w &= ~1u;
		if (crop_w > src_w)
			crop_w = src_w & ~1u;
		crop_x0 = (float)(src_w - crop_w) / 2.0f;
	} else if (dst_aspect > src_aspect) {
		/* destination is relatively wider: crop height, keep full
		 * width */
		crop_h = (uint32_t)((double)src_w / dst_aspect + 0.5);
		if (crop_h < 2)
			crop_h = 2;
		crop_h &= ~1u;
		if (crop_h > src_h)
			crop_h = src_h & ~1u;
		crop_y0 = (float)(src_h - crop_h) / 2.0f;
	}

	ovi.base_width = crop_w;
	ovi.base_height = crop_h;
	ovi.output_width = out_width;
	ovi.output_height = out_height;

	blog(LOG_INFO,
	     "obs_add_cropped_scaled_mix: src=%ux%u crop=%ux%u+%.0f,%.0f -> out=%ux%u",
	     src_w, src_h, crop_w, crop_h, crop_x0, crop_y0, out_width, out_height);

	struct obs_core_video_mix *mix = obs_create_video_mix(&ovi);
	if (!mix) {
		blog(LOG_WARNING, "obs_add_cropped_scaled_mix: obs_create_video_mix failed");
		return NULL;
	}

	mix->view = &canvas->view;
	mix->render_output_filter = render_output_filter;
	mix->has_crop = true;
	mix->crop_x0 = crop_x0;
	mix->crop_y0 = crop_y0;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &mix);
	pthread_mutex_unlock(&obs->video.mixes_mutex);

	return mix->video;
}

void obs_remove_video_mix(video_t *video)
{
	if (!video)
		return;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		struct obs_core_video_mix *mix = obs->video.mixes.array[i];
		if (mix->video == video) {
			mix->view = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

long obs_output_filtered_source_count(void)
{
	return os_atomic_load_long(&obs->video.output_filtered_count);
}

void obs_view_enum_video_info(obs_view_t *view, bool (*enum_proc)(void *, struct obs_video_info *), void *param)
{
	pthread_mutex_lock(&obs->video.mixes_mutex);

	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		struct obs_core_video_mix *mix = obs->video.mixes.array[i];
		if (mix->view != view)
			continue;
		if (!enum_proc(param, &mix->ovi))
			break;
	}

	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

/*****************************************************************************
Copyright (C) 2024 by the OBS Project

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
*****************************************************************************/

#include <obs-module.h>
#include <graphics/vec2.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* clang-format off */

#define SETTING_EXPOSURE     "exposure"
#define SETTING_CONTRAST     "contrast"
#define SETTING_HIGHLIGHTS   "highlights"
#define SETTING_SHADOWS      "shadows"
#define SETTING_TEMPERATURE  "temperature"
#define SETTING_TINT         "tint"
#define SETTING_HUE          "hue"
#define SETTING_SATURATION   "saturation"
#define SETTING_VIBRANCE     "vibrance"
#define SETTING_SHARPNESS    "sharpness"
#define SETTING_VIGNETTE     "vignette"

#define SETTING_GROUP_LIGHT   "group_light"
#define SETTING_GROUP_COLOR   "group_color"
#define SETTING_GROUP_DETAIL  "group_detail"
#define SETTING_GROUP_EFFECTS "group_effects"

#define TEXT_EXPOSURE      obs_module_text("Adjust.Exposure")
#define TEXT_CONTRAST      obs_module_text("Adjust.Contrast")
#define TEXT_HIGHLIGHTS    obs_module_text("Adjust.Highlights")
#define TEXT_SHADOWS       obs_module_text("Adjust.Shadows")
#define TEXT_TEMPERATURE   obs_module_text("Adjust.Temperature")
#define TEXT_TINT          obs_module_text("Adjust.Tint")
#define TEXT_HUE           obs_module_text("Adjust.Hue")
#define TEXT_SATURATION    obs_module_text("Adjust.Saturation")
#define TEXT_VIBRANCE      obs_module_text("Adjust.Vibrance")
#define TEXT_SHARPNESS     obs_module_text("Adjust.Sharpness")
#define TEXT_VIGNETTE      obs_module_text("Adjust.Vignette")

#define TEXT_GROUP_LIGHT   obs_module_text("Adjust.Group.Light")
#define TEXT_GROUP_COLOR   obs_module_text("Adjust.Group.Color")
#define TEXT_GROUP_DETAIL  obs_module_text("Adjust.Group.Detail")
#define TEXT_GROUP_EFFECTS obs_module_text("Adjust.Group.Effects")
#define TEXT_SDR_ONLY_INFO obs_module_text("SdrOnlyInfo")

/* clang-format on */

struct adjustments_data {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *exposure_param;
	gs_eparam_t *contrast_param;
	gs_eparam_t *highlights_param;
	gs_eparam_t *shadows_param;
	gs_eparam_t *temperature_param;
	gs_eparam_t *tint_param;
	gs_eparam_t *hue_param;
	gs_eparam_t *saturation_param;
	gs_eparam_t *vibrance_param;
	gs_eparam_t *sharpness_param;
	gs_eparam_t *texel_param;
	gs_eparam_t *vignette_param;

	float exposure;
	float contrast;
	float highlights;
	float shadows;
	float temperature;
	float tint;
	float hue;
	float saturation;
	float vibrance;
	float sharpness;
	float vignette;
};

static const char *adjustments_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AdjustmentsFilter");
}

static void adjustments_filter_update(void *data, obs_data_t *settings)
{
	struct adjustments_data *filter = data;

	/* Exposure in stops -> linear multiplier. */
	double stops = obs_data_get_double(settings, SETTING_EXPOSURE);
	filter->exposure = (float)pow(2.0, stops);

	/* Contrast -1..1 -> factor around 1.0. */
	filter->contrast = (float)obs_data_get_double(settings, SETTING_CONTRAST) + 1.0f;

	/* Highlights / shadows kept gentle. */
	filter->highlights = (float)obs_data_get_double(settings, SETTING_HIGHLIGHTS) * 0.5f;
	filter->shadows = (float)obs_data_get_double(settings, SETTING_SHADOWS) * 0.5f;

	/* White balance scaled into a subtle channel offset. */
	filter->temperature = (float)obs_data_get_double(settings, SETTING_TEMPERATURE) * 0.3f;
	filter->tint = (float)obs_data_get_double(settings, SETTING_TINT) * 0.3f;

	/* Hue in degrees -> radians. */
	filter->hue = (float)(obs_data_get_double(settings, SETTING_HUE) * (M_PI / 180.0));

	filter->saturation = (float)obs_data_get_double(settings, SETTING_SATURATION);
	filter->vibrance = (float)obs_data_get_double(settings, SETTING_VIBRANCE);
	filter->sharpness = (float)obs_data_get_double(settings, SETTING_SHARPNESS);
	filter->vignette = (float)obs_data_get_double(settings, SETTING_VIGNETTE);
}

static void adjustments_filter_destroy(void *data)
{
	struct adjustments_data *filter = data;

	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		obs_leave_graphics();
	}

	bfree(data);
}

static void *adjustments_filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct adjustments_data *filter = bzalloc(sizeof(struct adjustments_data));
	char *effect_path = obs_module_file("adjustments_filter.effect");

	filter->context = context;

	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	if (filter->effect) {
		filter->exposure_param = gs_effect_get_param_by_name(filter->effect, SETTING_EXPOSURE);
		filter->contrast_param = gs_effect_get_param_by_name(filter->effect, SETTING_CONTRAST);
		filter->highlights_param = gs_effect_get_param_by_name(filter->effect, SETTING_HIGHLIGHTS);
		filter->shadows_param = gs_effect_get_param_by_name(filter->effect, SETTING_SHADOWS);
		filter->temperature_param = gs_effect_get_param_by_name(filter->effect, SETTING_TEMPERATURE);
		filter->tint_param = gs_effect_get_param_by_name(filter->effect, SETTING_TINT);
		filter->hue_param = gs_effect_get_param_by_name(filter->effect, SETTING_HUE);
		filter->saturation_param = gs_effect_get_param_by_name(filter->effect, SETTING_SATURATION);
		filter->vibrance_param = gs_effect_get_param_by_name(filter->effect, SETTING_VIBRANCE);
		filter->sharpness_param = gs_effect_get_param_by_name(filter->effect, SETTING_SHARPNESS);
		filter->texel_param = gs_effect_get_param_by_name(filter->effect, "texel");
		filter->vignette_param = gs_effect_get_param_by_name(filter->effect, SETTING_VIGNETTE);
	}
	obs_leave_graphics();

	bfree(effect_path);

	if (!filter->effect) {
		adjustments_filter_destroy(filter);
		return NULL;
	}

	adjustments_filter_update(filter, settings);
	return filter;
}

static void adjustments_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct adjustments_data *filter = data;

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const enum gs_color_space source_space = obs_source_get_color_space(
		obs_filter_get_target(filter->context), OBS_COUNTOF(preferred_spaces), preferred_spaces);

	/* Adjustments operate on linear-ish color and never hard-clamp the
	 * output to 1.0, so they apply correctly to HDR (extended-range) sources
	 * as well as SDR. */
	const enum gs_color_format format = gs_get_format_from_space(source_space);
	if (!obs_source_process_filter_begin_with_color_space(filter->context, format, source_space,
							      OBS_ALLOW_DIRECT_RENDERING))
		return;

	const uint32_t width = obs_source_get_width(obs_filter_get_target(filter->context));
	const uint32_t height = obs_source_get_height(obs_filter_get_target(filter->context));
	struct vec2 texel;
	vec2_set(&texel, width ? 1.0f / (float)width : 0.0f, height ? 1.0f / (float)height : 0.0f);

	gs_effect_set_float(filter->exposure_param, filter->exposure);
	gs_effect_set_float(filter->contrast_param, filter->contrast);
	gs_effect_set_float(filter->highlights_param, filter->highlights);
	gs_effect_set_float(filter->shadows_param, filter->shadows);
	gs_effect_set_float(filter->temperature_param, filter->temperature);
	gs_effect_set_float(filter->tint_param, filter->tint);
	gs_effect_set_float(filter->hue_param, filter->hue);
	gs_effect_set_float(filter->saturation_param, filter->saturation);
	gs_effect_set_float(filter->vibrance_param, filter->vibrance);
	gs_effect_set_float(filter->sharpness_param, filter->sharpness);
	gs_effect_set_vec2(filter->texel_param, &texel);
	gs_effect_set_float(filter->vignette_param, filter->vignette);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	obs_source_process_filter_end(filter->context, filter->effect, 0, 0);

	gs_blend_state_pop();
}

static obs_properties_t *adjustments_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *light = obs_properties_create();
	obs_properties_add_float_slider(light, SETTING_EXPOSURE, TEXT_EXPOSURE, -2.0, 2.0, 0.01);
	obs_properties_add_float_slider(light, SETTING_CONTRAST, TEXT_CONTRAST, -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(light, SETTING_HIGHLIGHTS, TEXT_HIGHLIGHTS, -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(light, SETTING_SHADOWS, TEXT_SHADOWS, -1.0, 1.0, 0.01);
	obs_properties_add_group(props, SETTING_GROUP_LIGHT, TEXT_GROUP_LIGHT, OBS_GROUP_NORMAL, light);

	obs_properties_t *color = obs_properties_create();
	obs_properties_add_float_slider(color, SETTING_TEMPERATURE, TEXT_TEMPERATURE, -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(color, SETTING_TINT, TEXT_TINT, -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(color, SETTING_HUE, TEXT_HUE, -180.0, 180.0, 1.0);
	obs_properties_add_float_slider(color, SETTING_SATURATION, TEXT_SATURATION, 0.0, 2.0, 0.01);
	obs_properties_add_float_slider(color, SETTING_VIBRANCE, TEXT_VIBRANCE, -1.0, 1.0, 0.01);
	obs_properties_add_group(props, SETTING_GROUP_COLOR, TEXT_GROUP_COLOR, OBS_GROUP_NORMAL, color);

	obs_properties_t *detail = obs_properties_create();
	obs_properties_add_float_slider(detail, SETTING_SHARPNESS, TEXT_SHARPNESS, 0.0, 1.0, 0.01);
	obs_properties_add_group(props, SETTING_GROUP_DETAIL, TEXT_GROUP_DETAIL, OBS_GROUP_NORMAL, detail);

	obs_properties_t *effects = obs_properties_create();
	obs_properties_add_float_slider(effects, SETTING_VIGNETTE, TEXT_VIGNETTE, 0.0, 1.0, 0.01);
	obs_properties_add_group(props, SETTING_GROUP_EFFECTS, TEXT_GROUP_EFFECTS, OBS_GROUP_NORMAL, effects);

	UNUSED_PARAMETER(data);
	return props;
}

static void adjustments_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, SETTING_EXPOSURE, 0.0);
	obs_data_set_default_double(settings, SETTING_CONTRAST, 0.0);
	obs_data_set_default_double(settings, SETTING_HIGHLIGHTS, 0.0);
	obs_data_set_default_double(settings, SETTING_SHADOWS, 0.0);
	obs_data_set_default_double(settings, SETTING_TEMPERATURE, 0.0);
	obs_data_set_default_double(settings, SETTING_TINT, 0.0);
	obs_data_set_default_double(settings, SETTING_HUE, 0.0);
	obs_data_set_default_double(settings, SETTING_SATURATION, 1.0);
	obs_data_set_default_double(settings, SETTING_VIBRANCE, 0.0);
	obs_data_set_default_double(settings, SETTING_SHARPNESS, 0.0);
	obs_data_set_default_double(settings, SETTING_VIGNETTE, 0.0);
}

static enum gs_color_space adjustments_filter_get_color_space(void *data, size_t count,
							      const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	const enum gs_color_space potential_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	struct adjustments_data *const filter = data;
	return obs_source_get_color_space(obs_filter_get_target(filter->context), OBS_COUNTOF(potential_spaces),
					  potential_spaces);
}

struct obs_source_info adjustments_filter = {
	.id = "adjustments_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = adjustments_filter_name,
	.create = adjustments_filter_create,
	.destroy = adjustments_filter_destroy,
	.video_render = adjustments_filter_render,
	.update = adjustments_filter_update,
	.get_properties = adjustments_filter_properties,
	.get_defaults = adjustments_filter_defaults,
	.video_get_color_space = adjustments_filter_get_color_space,
};

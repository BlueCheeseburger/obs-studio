/******************************************************************************
    Voice Level Match filter

    Slowly matches the *typical* speaking level of this (microphone) source to
    the typical level of voices detected in a reference source (e.g. Desktop
    Audio carrying a voice call).

    Both sides run the same lightweight vocal-isolation chain: a 150 Hz –
    4 kHz speech band-pass feeding an energy/ratio voice-activity detector,
    whose voiced blocks drive a streaming *median* estimator of speech level.
    Because the median moves a fixed small step per block regardless of how
    far a block deviates, shouting and whispering are outliers that barely
    move the baseline — the applied gain tracks how loud the user's *normal*
    talking is, so intentional dynamics survive intact. The gain itself slews
    at a configurable dB/s rate, making it an equalizing trim, not a
    compressor.
******************************************************************************/

#include <obs-module.h>
#include <media-io/audio-math.h>
#include <util/platform.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

#define S_REF_SOURCE "reference_source"
#define S_TARGET_OFFSET "target_offset"
#define S_MAX_GAIN "max_gain"
#define S_ADAPT_SPEED "adapt_speed"

#define MT_ obs_module_text
#define TEXT_REF_SOURCE MT_("VoiceMatch.ReferenceSource")
#define TEXT_TARGET_OFFSET MT_("VoiceMatch.TargetOffset")
#define TEXT_MAX_GAIN MT_("VoiceMatch.MaxGain")
#define TEXT_ADAPT_SPEED MT_("VoiceMatch.AdaptSpeed")

/* VAD tuning (dB scale is 20*log10 of speech-band RMS) */
#define VAD_SNR_DB 10.0f         /* band level must exceed noise floor by this */
#define VAD_ABS_MIN_DB -50.0f    /* absolute minimum band level */
#define VAD_BAND_RATIO 0.25f     /* band RMS vs full RMS */
#define NOISE_RISE_DB 0.04f      /* noise floor recovery per block */
#define MEDIAN_STEP_WARM 0.05f   /* dB per voiced block once warmed up */
#define MEDIAN_STEP_COLD 0.40f   /* dB per voiced block during warm-up */
#define WARM_BLOCKS 140          /* voiced blocks (~3 s) before trusted */

struct biquad {
	float b0, b1, b2, a1, a2;
	float z1, z2;
};

/* One analysis chain (mic or reference) */
struct vm_side {
	struct biquad hp;
	struct biquad lp;
	float noise_floor_db;
	float speech_db;   /* streaming median of voiced band level */
	uint32_t voiced_blocks;
	bool vad;
	float last_db;
};

struct voice_match_data {
	obs_source_t *context;

	size_t num_channels;
	uint32_t sample_rate;

	/* settings */
	float target_offset_db;
	float max_gain_db;
	float adapt_speed_db_s;

	/* mic side (filter audio thread only) */
	struct vm_side mic;

	/* reference side (capture callback), guarded by ref_mutex */
	pthread_mutex_t ref_mutex;
	struct vm_side ref;

	/* current applied gain */
	float cur_gain_db;
	float prev_mul;

	/* mono scratch */
	float *mono_buf;
	size_t mono_len;

	/* reference source management (compressor sidechain pattern) */
	pthread_mutex_t ref_update_mutex;
	uint64_t ref_check_time;
	obs_weak_source_t *weak_ref;
	char *ref_name;
};

/* ------------------------------------------------------------------------- */

static void biquad_set_hpf(struct biquad *f, float fc, float rate)
{
	const float Q = 0.7071f;
	float w = 2.0f * (float)M_PI * fc / rate;
	float cw = cosf(w);
	float sw = sinf(w);
	float alpha = sw / (2.0f * Q);
	float a0 = 1.0f + alpha;

	f->b0 = (1.0f + cw) / 2.0f / a0;
	f->b1 = -(1.0f + cw) / a0;
	f->b2 = (1.0f + cw) / 2.0f / a0;
	f->a1 = (-2.0f * cw) / a0;
	f->a2 = (1.0f - alpha) / a0;
	f->z1 = f->z2 = 0.0f;
}

static void biquad_set_lpf(struct biquad *f, float fc, float rate)
{
	const float Q = 0.7071f;
	float w = 2.0f * (float)M_PI * fc / rate;
	float cw = cosf(w);
	float sw = sinf(w);
	float alpha = sw / (2.0f * Q);
	float a0 = 1.0f + alpha;

	f->b0 = (1.0f - cw) / 2.0f / a0;
	f->b1 = (1.0f - cw) / a0;
	f->b2 = (1.0f - cw) / 2.0f / a0;
	f->a1 = (-2.0f * cw) / a0;
	f->a2 = (1.0f - alpha) / a0;
	f->z1 = f->z2 = 0.0f;
}

static inline float biquad_run(struct biquad *f, float in)
{
	float out = in * f->b0 + f->z1;
	f->z1 = in * f->b1 + f->z2 - f->a1 * out;
	f->z2 = in * f->b2 - f->a2 * out;
	return out;
}

static void vm_side_init(struct vm_side *s, uint32_t rate)
{
	biquad_set_hpf(&s->hp, 150.0f, (float)rate);
	biquad_set_lpf(&s->lp, 4000.0f, (float)rate);
	s->noise_floor_db = -60.0f;
	s->speech_db = -30.0f;
	s->voiced_blocks = 0;
	s->vad = false;
	s->last_db = -90.0f;
}

/* Analyze one block of mono audio; updates VAD + median speech level. */
static void vm_side_analyze(struct vm_side *s, const float *mono, uint32_t frames)
{
	if (!frames)
		return;

	float full_sq = 0.0f;
	float band_sq = 0.0f;

	for (uint32_t i = 0; i < frames; i++) {
		float x = mono[i];
		full_sq += x * x;
		float b = biquad_run(&s->lp, biquad_run(&s->hp, x));
		band_sq += b * b;
	}

	float full_rms = sqrtf(full_sq / (float)frames);
	float band_rms = sqrtf(band_sq / (float)frames);
	float band_db = mul_to_db(band_rms);
	if (!isfinite(band_db))
		band_db = -90.0f;

	s->last_db = band_db;

	/* min-tracking noise floor */
	if (band_db < s->noise_floor_db)
		s->noise_floor_db = band_db;
	else
		s->noise_floor_db += NOISE_RISE_DB;
	if (s->noise_floor_db < -90.0f)
		s->noise_floor_db = -90.0f;

	bool voiced = band_db > s->noise_floor_db + VAD_SNR_DB && band_db > VAD_ABS_MIN_DB &&
		      band_rms > VAD_BAND_RATIO * full_rms;
	s->vad = voiced;

	if (voiced) {
		float step = (s->voiced_blocks >= WARM_BLOCKS) ? MEDIAN_STEP_WARM : MEDIAN_STEP_COLD;
		s->speech_db += (band_db > s->speech_db) ? step : -step;
		if (s->voiced_blocks < UINT32_MAX)
			s->voiced_blocks++;
	}
}

/* ------------------------------------------------------------------------- */

static const char *voice_match_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return MT_("VoiceMatch");
}

static inline obs_source_t *get_ref_source(struct voice_match_data *vm)
{
	if (vm->weak_ref)
		return obs_weak_source_get_source(vm->weak_ref);
	return NULL;
}

static void ref_capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	struct voice_match_data *vm = param;

	UNUSED_PARAMETER(source);

	if (muted || !audio_data->frames)
		return;

	uint32_t frames = audio_data->frames;

	/* average available channels to mono on the stack in chunks */
	float chunk[256];
	const float *ch0 = (const float *)audio_data->data[0];
	const float *ch1 = (const float *)audio_data->data[1];
	if (!ch0)
		return;

	pthread_mutex_lock(&vm->ref_mutex);
	uint32_t pos = 0;
	while (pos < frames) {
		uint32_t n = frames - pos;
		if (n > 256)
			n = 256;
		if (ch1) {
			for (uint32_t i = 0; i < n; i++)
				chunk[i] = 0.5f * (ch0[pos + i] + ch1[pos + i]);
		} else {
			memcpy(chunk, ch0 + pos, n * sizeof(float));
		}
		vm_side_analyze(&vm->ref, chunk, n);
		pos += n;
	}
	pthread_mutex_unlock(&vm->ref_mutex);
}

static void voice_match_update(void *data, obs_data_t *s)
{
	struct voice_match_data *vm = data;

	vm->sample_rate = audio_output_get_sample_rate(obs_get_audio());
	vm->num_channels = audio_output_get_channels(obs_get_audio());
	vm->target_offset_db = (float)obs_data_get_double(s, S_TARGET_OFFSET);
	vm->max_gain_db = (float)obs_data_get_double(s, S_MAX_GAIN);
	vm->adapt_speed_db_s = (float)obs_data_get_double(s, S_ADAPT_SPEED);

	const char *ref_name = obs_data_get_string(s, S_REF_SOURCE);
	bool valid_ref = *ref_name && strcmp(ref_name, "none") != 0;
	obs_weak_source_t *old_weak = NULL;

	pthread_mutex_lock(&vm->ref_update_mutex);

	if (!valid_ref) {
		if (vm->weak_ref) {
			old_weak = vm->weak_ref;
			vm->weak_ref = NULL;
		}
		bfree(vm->ref_name);
		vm->ref_name = NULL;
	} else if (!vm->ref_name || strcmp(vm->ref_name, ref_name) != 0) {
		if (vm->weak_ref) {
			old_weak = vm->weak_ref;
			vm->weak_ref = NULL;
		}
		bfree(vm->ref_name);
		vm->ref_name = bstrdup(ref_name);
		vm->ref_check_time = os_gettime_ns() - 3000000000;

		/* new reference: restart its estimator */
		pthread_mutex_lock(&vm->ref_mutex);
		vm_side_init(&vm->ref, vm->sample_rate);
		pthread_mutex_unlock(&vm->ref_mutex);
	}

	pthread_mutex_unlock(&vm->ref_update_mutex);

	if (old_weak) {
		obs_source_t *old = obs_weak_source_get_source(old_weak);
		if (old) {
			obs_source_remove_audio_capture_callback(old, ref_capture, vm);
			obs_source_release(old);
		}
		obs_weak_source_release(old_weak);
	}
}

static void voice_match_stats_proc(void *data, calldata_t *cd)
{
	struct voice_match_data *vm = data;

	calldata_set_float(cd, "mic_db", vm->mic.speech_db);
	calldata_set_bool(cd, "mic_vad", vm->mic.vad);
	calldata_set_bool(cd, "mic_warm", vm->mic.voiced_blocks >= WARM_BLOCKS);
	calldata_set_float(cd, "mic_now_db", vm->mic.last_db);

	pthread_mutex_lock(&vm->ref_mutex);
	calldata_set_float(cd, "ref_db", vm->ref.speech_db);
	calldata_set_bool(cd, "ref_vad", vm->ref.vad);
	calldata_set_bool(cd, "ref_warm", vm->ref.voiced_blocks >= WARM_BLOCKS);
	calldata_set_float(cd, "ref_now_db", vm->ref.last_db);
	pthread_mutex_unlock(&vm->ref_mutex);

	pthread_mutex_lock(&vm->ref_update_mutex);
	calldata_set_bool(cd, "has_ref", vm->weak_ref != NULL);
	pthread_mutex_unlock(&vm->ref_update_mutex);

	calldata_set_float(cd, "gain_db", vm->cur_gain_db);
}

static void *voice_match_create(obs_data_t *settings, obs_source_t *filter)
{
	struct voice_match_data *vm = bzalloc(sizeof(struct voice_match_data));
	vm->context = filter;

	if (pthread_mutex_init(&vm->ref_mutex, NULL) != 0) {
		bfree(vm);
		return NULL;
	}
	if (pthread_mutex_init(&vm->ref_update_mutex, NULL) != 0) {
		pthread_mutex_destroy(&vm->ref_mutex);
		bfree(vm);
		return NULL;
	}

	vm->sample_rate = audio_output_get_sample_rate(obs_get_audio());
	vm_side_init(&vm->mic, vm->sample_rate);
	vm_side_init(&vm->ref, vm->sample_rate);
	vm->cur_gain_db = 0.0f;
	vm->prev_mul = 1.0f;

	proc_handler_t *ph = obs_source_get_proc_handler(filter);
	proc_handler_add(ph, "void get_voice_match_stats()", voice_match_stats_proc, vm);

	voice_match_update(vm, settings);
	return vm;
}

static void voice_match_destroy(void *data)
{
	struct voice_match_data *vm = data;

	if (vm->weak_ref) {
		obs_source_t *ref = get_ref_source(vm);
		if (ref) {
			obs_source_remove_audio_capture_callback(ref, ref_capture, vm);
			obs_source_release(ref);
		}
		obs_weak_source_release(vm->weak_ref);
	}

	pthread_mutex_destroy(&vm->ref_mutex);
	pthread_mutex_destroy(&vm->ref_update_mutex);
	bfree(vm->ref_name);
	bfree(vm->mono_buf);
	bfree(vm);
}

/* Reconnect to the reference source by name when it appears (same pattern as
 * the compressor's sidechain). */
static void voice_match_tick(void *data, float seconds)
{
	struct voice_match_data *vm = data;
	char *new_name = NULL;

	pthread_mutex_lock(&vm->ref_update_mutex);

	if (vm->ref_name && !vm->weak_ref) {
		uint64_t t = os_gettime_ns();
		if (t - vm->ref_check_time > 3000000000) {
			new_name = bstrdup(vm->ref_name);
			vm->ref_check_time = t;
		}
	}

	pthread_mutex_unlock(&vm->ref_update_mutex);

	if (new_name) {
		obs_source_t *ref = *new_name ? obs_get_source_by_name(new_name) : NULL;
		obs_weak_source_t *weak = ref ? obs_source_get_weak_source(ref) : NULL;

		pthread_mutex_lock(&vm->ref_update_mutex);
		if (vm->ref_name && strcmp(vm->ref_name, new_name) == 0) {
			vm->weak_ref = weak;
			weak = NULL;
		}
		pthread_mutex_unlock(&vm->ref_update_mutex);

		if (ref) {
			obs_source_add_audio_capture_callback(ref, ref_capture, vm);
			obs_weak_source_release(weak);
			obs_source_release(ref);
		}

		bfree(new_name);
	}

	UNUSED_PARAMETER(seconds);
}

static struct obs_audio_data *voice_match_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct voice_match_data *vm = data;
	const uint32_t frames = audio->frames;
	float **samples = (float **)audio->data;

	if (!frames || !samples[0])
		return audio;

	/* mono mix for analysis */
	if (vm->mono_len < frames) {
		vm->mono_buf = brealloc(vm->mono_buf, frames * sizeof(float));
		vm->mono_len = frames;
	}

	size_t active_ch = 0;
	for (size_t c = 0; c < vm->num_channels && c < MAX_AV_PLANES; c++) {
		if (!samples[c])
			break;
		active_ch++;
	}
	if (!active_ch)
		return audio;

	for (uint32_t i = 0; i < frames; i++) {
		float acc = 0.0f;
		for (size_t c = 0; c < active_ch; c++)
			acc += samples[c][i];
		vm->mono_buf[i] = acc / (float)active_ch;
	}

	vm_side_analyze(&vm->mic, vm->mono_buf, frames);

	/* decide target gain */
	bool mic_warm = vm->mic.voiced_blocks >= WARM_BLOCKS;

	pthread_mutex_lock(&vm->ref_mutex);
	bool ref_warm = vm->ref.voiced_blocks >= WARM_BLOCKS;
	float ref_speech = vm->ref.speech_db;
	pthread_mutex_unlock(&vm->ref_mutex);

	pthread_mutex_lock(&vm->ref_update_mutex);
	bool has_ref = vm->weak_ref != NULL;
	pthread_mutex_unlock(&vm->ref_update_mutex);

	float target = vm->cur_gain_db;
	if (has_ref && mic_warm && ref_warm) {
		target = ref_speech + vm->target_offset_db - vm->mic.speech_db;
		if (target > vm->max_gain_db)
			target = vm->max_gain_db;
		if (target < -vm->max_gain_db)
			target = -vm->max_gain_db;
	}

	/* slew toward target */
	float block_secs = (float)frames / (float)vm->sample_rate;
	float max_delta = vm->adapt_speed_db_s * block_secs;
	float delta = target - vm->cur_gain_db;
	if (delta > max_delta)
		delta = max_delta;
	if (delta < -max_delta)
		delta = -max_delta;
	vm->cur_gain_db += delta;

	/* apply with per-sample ramp to avoid zipper noise */
	float new_mul = db_to_mul(vm->cur_gain_db);
	float mul = vm->prev_mul;
	float mul_step = (new_mul - mul) / (float)frames;

	for (size_t c = 0; c < active_ch; c++) {
		float m = mul;
		float *chan = samples[c];
		for (uint32_t i = 0; i < frames; i++) {
			chan[i] *= m;
			m += mul_step;
		}
	}
	vm->prev_mul = new_mul;

	return audio;
}

/* ------------------------------------------------------------------------- */

struct ref_prop_info {
	obs_property_t *sources;
	obs_source_t *parent;
};

static bool add_ref_sources(void *data, obs_source_t *source)
{
	struct ref_prop_info *info = data;
	uint32_t caps = obs_source_get_output_flags(source);

	if (source == info->parent)
		return true;
	if ((caps & OBS_SOURCE_AUDIO) == 0)
		return true;

	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(info->sources, name, name);
	return true;
}

static obs_properties_t *voice_match_properties(void *data)
{
	struct voice_match_data *vm = data;
	obs_properties_t *props = obs_properties_create();
	obs_source_t *parent = NULL;
	obs_property_t *p;

	if (vm)
		parent = obs_filter_get_parent(vm->context);

	obs_property_t *sources = obs_properties_add_list(props, S_REF_SOURCE, TEXT_REF_SOURCE, OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources, obs_module_text("None"), "none");

	struct ref_prop_info info = {sources, parent};
	obs_enum_sources(add_ref_sources, &info);

	p = obs_properties_add_float_slider(props, S_TARGET_OFFSET, TEXT_TARGET_OFFSET, -10.0, 10.0, 0.1);
	obs_property_float_set_suffix(p, " dB");
	p = obs_properties_add_float_slider(props, S_MAX_GAIN, TEXT_MAX_GAIN, 3.0, 24.0, 0.5);
	obs_property_float_set_suffix(p, " dB");
	p = obs_properties_add_float_slider(props, S_ADAPT_SPEED, TEXT_ADAPT_SPEED, 0.1, 6.0, 0.1);
	obs_property_float_set_suffix(p, " dB/s");

	return props;
}

static void voice_match_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, S_REF_SOURCE, "none");
	obs_data_set_default_double(s, S_TARGET_OFFSET, 0.0);
	obs_data_set_default_double(s, S_MAX_GAIN, 12.0);
	obs_data_set_default_double(s, S_ADAPT_SPEED, 1.0);
}

struct obs_source_info voice_match_filter = {
	.id = "voice_match_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = voice_match_name,
	.create = voice_match_create,
	.destroy = voice_match_destroy,
	.update = voice_match_update,
	.filter_audio = voice_match_filter_audio,
	.video_tick = voice_match_tick,
	.get_defaults = voice_match_defaults,
	.get_properties = voice_match_properties,
};

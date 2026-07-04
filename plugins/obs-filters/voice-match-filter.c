/******************************************************************************
    Voice Level Match filter

    Slowly matches the *typical* speaking level of this (microphone) source to
    the typical level of voices detected in one or more reference sources
    (Desktop Audio, Discord/application audio captures, party-chat inputs).

    All sides run the same lightweight vocal-isolation chain:
      - 150 Hz – 4 kHz speech band-pass, split into low (150–1000 Hz) and
        high (1–4 kHz) sub-bands
      - a multi-feature voice-activity detector: SNR above a tracked noise
        floor, speech-band dominance over the full signal, energy present in
        BOTH sub-bands (voiced + consonant energy), and a syllabic modulation
        gate (speech level fluctuates 3–8 Hz; steady music/game beds do not)
      - onset/hangover state machine to reject clicks and bridge short gaps

    Voiced blocks feed streaming *median* level estimators. Because the median
    moves a fixed small step per block regardless of how far a block deviates,
    shouting and whispering are outliers that barely move the baseline — the
    applied gain tracks the user's *normal* talking level, so intentional
    dynamics survive intact. The gain slews at a configurable dB/s rate,
    making it an equalizing trim, not a compressor.

    References: in auto mode the filter attaches to every source that looks
    like voice-chat audio — desktop audio captures, application audio captures
    (e.g. Discord), and any audio source whose name suggests voice chat
    (discord/party/chat/voice/xbox/…). All references share one "friends"
    level median; whichever source carries speech at a given moment
    contributes. A manual reference can be added on top.
******************************************************************************/

#include <obs-module.h>
#include <media-io/audio-math.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

#define S_REF_SOURCE "reference_source"
#define S_AUTO_REFS "auto_references"
#define S_TARGET_OFFSET "target_offset"
#define S_MAX_GAIN "max_gain"
#define S_ADAPT_SPEED "adapt_speed"

#define MT_ obs_module_text
#define TEXT_REF_SOURCE MT_("VoiceMatch.ReferenceSource")
#define TEXT_AUTO_REFS MT_("VoiceMatch.AutoReferences")
#define TEXT_TARGET_OFFSET MT_("VoiceMatch.TargetOffset")
#define TEXT_MAX_GAIN MT_("VoiceMatch.MaxGain")
#define TEXT_ADAPT_SPEED MT_("VoiceMatch.AdaptSpeed")

/* VAD tuning (dB scale is 20*log10 of speech-band RMS) */
#define VAD_SNR_DB 10.0f          /* band level must exceed noise floor by this */
#define VAD_ABS_MIN_DB -50.0f     /* absolute minimum band level */
#define VAD_BAND_RATIO 0.25f      /* speech-band RMS vs full RMS */
#define VAD_LOW_MIN 0.04f         /* low sub-band share of band energy */
#define VAD_HIGH_MIN 0.02f        /* high sub-band share of band energy */
#define VAD_MOD_MIN_DB 2.5f       /* min stddev of band level over ~0.7 s */
#define VAD_ONSET_BLOCKS 2        /* consecutive candidates to enter speech */
#define VAD_HANGOVER_BLOCKS 6     /* blocks speech persists after candidates stop */
#define NOISE_RISE_DB 0.04f       /* noise floor recovery per block */
#define MOD_RING_LEN 32           /* blocks in the modulation window (~0.7 s) */

#define MEDIAN_STEP_WARM 0.05f    /* dB per voiced block once warmed up */
#define MEDIAN_STEP_COLD 0.40f    /* dB per voiced block during warm-up */
#define WARM_BLOCKS 140           /* voiced blocks (~3 s) before trusted */

#define MAX_REFS 8
#define REF_RECONCILE_NS 3000000000ULL
#define REF_VAD_HOLD_NS 200000000ULL

struct biquad {
	float b0, b1, b2, a1, a2;
	float z1, z2;
};

/* One per-source analysis chain */
struct vm_side {
	struct biquad hp;      /* 150 Hz high-pass */
	struct biquad lp;      /* 4 kHz low-pass */
	struct biquad split;   /* 1 kHz low-pass on the band signal */

	float noise_floor_db;

	/* syllabic modulation ring of recent band levels */
	float mod_ring[MOD_RING_LEN];
	uint32_t mod_pos;
	uint32_t mod_filled;

	/* onset / hangover state */
	uint32_t onset_run;
	uint32_t hangover;
	bool speaking;

	float last_db;
};

/* Streaming median of voiced speech level */
struct vm_level {
	float speech_db;
	uint32_t voiced_blocks;
};

struct vm_ref {
	struct voice_match_data *vm;
	struct vm_side side;
	obs_weak_source_t *weak; /* NULL = free slot */
	char *name;
};

struct voice_match_data {
	obs_source_t *context;

	size_t num_channels;
	uint32_t sample_rate;

	/* settings */
	float target_offset_db;
	float max_gain_db;
	float adapt_speed_db_s;
	bool auto_refs;
	char *manual_ref; /* bstrdup'd or NULL */

	/* mic side (filter audio thread only) */
	struct vm_side mic;
	struct vm_level mic_level;
	bool mic_vad;

	/* reference DSP state, guarded by ref_mutex */
	pthread_mutex_t ref_mutex;
	struct vm_level ref_level;
	uint64_t ref_vad_until;

	/* reference topology, guarded by ref_update_mutex */
	pthread_mutex_t ref_update_mutex;
	struct vm_ref refs[MAX_REFS];
	uint64_t ref_check_time;

	/* current applied gain */
	float cur_gain_db;
	float prev_mul;

	/* mono scratch */
	float *mono_buf;
	size_t mono_len;
};

/* ------------------------------------------------------------------------- */
/* DSP primitives                                                             */

static void biquad_set(struct biquad *f, bool highpass, float fc, float rate)
{
	const float Q = 0.7071f;
	float w = 2.0f * (float)M_PI * fc / rate;
	float cw = cosf(w);
	float sw = sinf(w);
	float alpha = sw / (2.0f * Q);
	float a0 = 1.0f + alpha;

	if (highpass) {
		f->b0 = (1.0f + cw) / 2.0f / a0;
		f->b1 = -(1.0f + cw) / a0;
		f->b2 = (1.0f + cw) / 2.0f / a0;
	} else {
		f->b0 = (1.0f - cw) / 2.0f / a0;
		f->b1 = (1.0f - cw) / a0;
		f->b2 = (1.0f - cw) / 2.0f / a0;
	}
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
	memset(s, 0, sizeof(*s));
	biquad_set(&s->hp, true, 150.0f, (float)rate);
	biquad_set(&s->lp, false, 4000.0f, (float)rate);
	biquad_set(&s->split, false, 1000.0f, (float)rate);
	s->noise_floor_db = -60.0f;
	s->last_db = -90.0f;
}

static void vm_level_init(struct vm_level *l)
{
	l->speech_db = -30.0f;
	l->voiced_blocks = 0;
}

static void vm_level_update(struct vm_level *l, float band_db)
{
	float step = (l->voiced_blocks >= WARM_BLOCKS) ? MEDIAN_STEP_WARM : MEDIAN_STEP_COLD;
	l->speech_db += (band_db > l->speech_db) ? step : -step;
	if (l->voiced_blocks < UINT32_MAX)
		l->voiced_blocks++;
}

/* Analyze one block of mono audio. Returns true if the block is speech; the
 * voiced band level is written to *out_db. */
static bool vm_side_analyze(struct vm_side *s, const float *mono, uint32_t frames, float *out_db)
{
	if (!frames)
		return false;

	float full_sq = 0.0f;
	float band_sq = 0.0f;
	float low_sq = 0.0f;

	for (uint32_t i = 0; i < frames; i++) {
		float x = mono[i];
		full_sq += x * x;
		float b = biquad_run(&s->lp, biquad_run(&s->hp, x));
		band_sq += b * b;
		float lo = biquad_run(&s->split, b);
		low_sq += lo * lo;
	}

	float high_sq = band_sq - low_sq;
	if (high_sq < 0.0f)
		high_sq = 0.0f;

	float full_rms = sqrtf(full_sq / (float)frames);
	float band_rms = sqrtf(band_sq / (float)frames);
	float band_db = mul_to_db(band_rms);
	if (!isfinite(band_db))
		band_db = -90.0f;

	s->last_db = band_db;
	*out_db = band_db;

	/* min-tracking noise floor */
	if (band_db < s->noise_floor_db)
		s->noise_floor_db = band_db;
	else
		s->noise_floor_db += NOISE_RISE_DB;
	if (s->noise_floor_db < -90.0f)
		s->noise_floor_db = -90.0f;

	/* syllabic modulation window */
	s->mod_ring[s->mod_pos] = band_db;
	s->mod_pos = (s->mod_pos + 1) % MOD_RING_LEN;
	if (s->mod_filled < MOD_RING_LEN)
		s->mod_filled++;

	float mod_std = 0.0f;
	if (s->mod_filled == MOD_RING_LEN) {
		float mean = 0.0f;
		for (uint32_t i = 0; i < MOD_RING_LEN; i++)
			mean += s->mod_ring[i];
		mean /= MOD_RING_LEN;
		float var = 0.0f;
		for (uint32_t i = 0; i < MOD_RING_LEN; i++) {
			float d = s->mod_ring[i] - mean;
			var += d * d;
		}
		mod_std = sqrtf(var / MOD_RING_LEN);
	}

	/* candidate features */
	bool candidate = band_db > s->noise_floor_db + VAD_SNR_DB && band_db > VAD_ABS_MIN_DB &&
			 band_rms > VAD_BAND_RATIO * full_rms;

	/* both voiced (low) and consonant (high) energy must be present */
	if (candidate && band_sq > 0.0f) {
		float low_share = low_sq / band_sq;
		float high_share = high_sq / band_sq;
		candidate = low_share > VAD_LOW_MIN && high_share > VAD_HIGH_MIN;
	}

	/* speech has strong syllabic level modulation; steady beds do not */
	if (candidate && s->mod_filled == MOD_RING_LEN && mod_std < VAD_MOD_MIN_DB)
		candidate = false;

	/* onset / hangover state machine */
	if (candidate) {
		if (s->onset_run < UINT32_MAX)
			s->onset_run++;
		if (s->onset_run >= VAD_ONSET_BLOCKS) {
			s->speaking = true;
			s->hangover = VAD_HANGOVER_BLOCKS;
		}
	} else {
		s->onset_run = 0;
		if (s->speaking) {
			if (s->hangover > 0)
				s->hangover--;
			else
				s->speaking = false;
		}
	}

	/* only genuine candidate blocks update levels; hangover merely keeps
	 * the state (and UI light) from flickering between syllables */
	return s->speaking && candidate;
}

/* ------------------------------------------------------------------------- */
/* reference management                                                       */

static const char *voice_keywords[] = {"discord", "party", "chat", "voice", "xbox",
				       "teamspeak", "mumble", "vent", "skype", "call"};

static bool source_is_voice_reference(obs_source_t *source, obs_source_t *parent)
{
	if (!source || source == parent)
		return false;

	uint32_t caps = obs_source_get_output_flags(source);
	if ((caps & OBS_SOURCE_AUDIO) == 0)
		return false;

	const char *id = obs_source_get_unversioned_id(source);

	/* desktop audio + application audio capture (Discord etc.) */
	if (strcmp(id, "wasapi_output_capture") == 0)
		return true;
	if (strcmp(id, "wasapi_process_output_capture") == 0)
		return true;

	/* any audio source named like a voice-chat feed (e.g. an Audio Input
	 * Capture carrying Xbox party chat) */
	const char *name = obs_source_get_name(source);
	if (name) {
		for (size_t i = 0; i < sizeof(voice_keywords) / sizeof(voice_keywords[0]); i++) {
			if (astrstri(name, voice_keywords[i]))
				return true;
		}
	}

	return false;
}

static void ref_capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	struct vm_ref *ref = param;
	struct voice_match_data *vm = ref->vm;

	UNUSED_PARAMETER(source);

	if (muted || !audio_data->frames)
		return;

	uint32_t frames = audio_data->frames;
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

		float db;
		if (vm_side_analyze(&ref->side, chunk, n, &db)) {
			vm_level_update(&vm->ref_level, db);
			vm->ref_vad_until = os_gettime_ns() + REF_VAD_HOLD_NS;
		}
		pos += n;
	}
	pthread_mutex_unlock(&vm->ref_mutex);
}

/* Detach one slot (topology mutex must be held). */
static void ref_slot_detach(struct voice_match_data *vm, struct vm_ref *ref)
{
	if (!ref->weak)
		return;

	obs_source_t *src = obs_weak_source_get_source(ref->weak);
	if (src) {
		obs_source_remove_audio_capture_callback(src, ref_capture, ref);
		obs_source_release(src);
	}
	obs_weak_source_release(ref->weak);
	ref->weak = NULL;
	bfree(ref->name);
	ref->name = NULL;
}

struct ref_enum_ctx {
	struct voice_match_data *vm;
	obs_source_t *parent;
	struct dstr names[MAX_REFS];
	size_t num;
};

static bool collect_voice_refs(void *param, obs_source_t *source)
{
	struct ref_enum_ctx *ctx = param;

	if (ctx->num >= MAX_REFS)
		return false;

	if (source_is_voice_reference(source, ctx->parent)) {
		dstr_init_copy(&ctx->names[ctx->num], obs_source_get_name(source));
		ctx->num++;
	}
	return true;
}

/* Reconcile attached references with the desired set (auto matches + manual
 * selection). Runs on the tick thread every few seconds. */
static void reconcile_refs(struct voice_match_data *vm)
{
	struct ref_enum_ctx ctx = {0};
	ctx.vm = vm;
	ctx.parent = obs_filter_get_parent(vm->context);

	pthread_mutex_lock(&vm->ref_update_mutex);
	bool auto_refs = vm->auto_refs;
	char *manual = vm->manual_ref ? bstrdup(vm->manual_ref) : NULL;
	pthread_mutex_unlock(&vm->ref_update_mutex);

	if (auto_refs)
		obs_enum_sources(collect_voice_refs, &ctx);

	/* include the manual selection if not already present */
	if (manual && *manual && ctx.num < MAX_REFS) {
		bool already = false;
		for (size_t i = 0; i < ctx.num; i++) {
			if (strcmp(ctx.names[i].array, manual) == 0) {
				already = true;
				break;
			}
		}
		if (!already) {
			obs_source_t *src = obs_get_source_by_name(manual);
			if (src) {
				if (src != ctx.parent) {
					dstr_init_copy(&ctx.names[ctx.num], manual);
					ctx.num++;
				}
				obs_source_release(src);
			}
		}
	}
	bfree(manual);

	pthread_mutex_lock(&vm->ref_update_mutex);

	/* detach refs that are no longer desired or whose source died */
	for (size_t i = 0; i < MAX_REFS; i++) {
		struct vm_ref *ref = &vm->refs[i];
		if (!ref->weak)
			continue;

		bool keep = false;
		obs_source_t *still = obs_weak_source_get_source(ref->weak);
		if (still) {
			for (size_t n = 0; n < ctx.num; n++) {
				if (ref->name && strcmp(ctx.names[n].array, ref->name) == 0) {
					keep = true;
					break;
				}
			}
			obs_source_release(still);
		}
		if (!keep)
			ref_slot_detach(vm, ref);
	}

	/* attach newly desired refs */
	for (size_t n = 0; n < ctx.num; n++) {
		bool attached = false;
		for (size_t i = 0; i < MAX_REFS; i++) {
			struct vm_ref *ref = &vm->refs[i];
			if (ref->weak && ref->name && strcmp(ref->name, ctx.names[n].array) == 0) {
				attached = true;
				break;
			}
		}
		if (attached)
			continue;

		/* find a free slot */
		struct vm_ref *slot = NULL;
		for (size_t i = 0; i < MAX_REFS; i++) {
			if (!vm->refs[i].weak) {
				slot = &vm->refs[i];
				break;
			}
		}
		if (!slot)
			break;

		obs_source_t *src = obs_get_source_by_name(ctx.names[n].array);
		if (!src)
			continue;
		if (src == ctx.parent) {
			obs_source_release(src);
			continue;
		}

		pthread_mutex_lock(&vm->ref_mutex);
		vm_side_init(&slot->side, vm->sample_rate);
		pthread_mutex_unlock(&vm->ref_mutex);

		slot->vm = vm;
		slot->name = bstrdup(ctx.names[n].array);
		slot->weak = obs_source_get_weak_source(src);
		obs_source_add_audio_capture_callback(src, ref_capture, slot);
		obs_source_release(src);

		blog(LOG_INFO, "[voice match: '%s'] attached reference '%s'",
		     obs_source_get_name(vm->context), slot->name);
	}

	pthread_mutex_unlock(&vm->ref_update_mutex);

	for (size_t n = 0; n < ctx.num; n++)
		dstr_free(&ctx.names[n]);
}

static size_t count_refs(struct voice_match_data *vm)
{
	size_t count = 0;
	pthread_mutex_lock(&vm->ref_update_mutex);
	for (size_t i = 0; i < MAX_REFS; i++) {
		if (vm->refs[i].weak)
			count++;
	}
	pthread_mutex_unlock(&vm->ref_update_mutex);
	return count;
}

/* ------------------------------------------------------------------------- */
/* filter callbacks                                                           */

static const char *voice_match_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return MT_("VoiceMatch");
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

	pthread_mutex_lock(&vm->ref_update_mutex);
	vm->auto_refs = obs_data_get_bool(s, S_AUTO_REFS);
	bfree(vm->manual_ref);
	vm->manual_ref = valid_ref ? bstrdup(ref_name) : NULL;
	vm->ref_check_time = 0; /* reconcile on next tick */
	pthread_mutex_unlock(&vm->ref_update_mutex);
}

static void voice_match_stats_proc(void *data, calldata_t *cd)
{
	struct voice_match_data *vm = data;

	calldata_set_float(cd, "mic_db", vm->mic_level.speech_db);
	calldata_set_bool(cd, "mic_vad", vm->mic_vad);
	calldata_set_bool(cd, "mic_warm", vm->mic_level.voiced_blocks >= WARM_BLOCKS);
	calldata_set_float(cd, "mic_now_db", vm->mic.last_db);

	pthread_mutex_lock(&vm->ref_mutex);
	calldata_set_float(cd, "ref_db", vm->ref_level.speech_db);
	calldata_set_bool(cd, "ref_vad", os_gettime_ns() < vm->ref_vad_until);
	calldata_set_bool(cd, "ref_warm", vm->ref_level.voiced_blocks >= WARM_BLOCKS);
	pthread_mutex_unlock(&vm->ref_mutex);

	size_t refs = count_refs(vm);
	calldata_set_bool(cd, "has_ref", refs > 0);
	calldata_set_int(cd, "ref_count", (long long)refs);

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
	vm_level_init(&vm->mic_level);
	vm_level_init(&vm->ref_level);
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

	pthread_mutex_lock(&vm->ref_update_mutex);
	for (size_t i = 0; i < MAX_REFS; i++)
		ref_slot_detach(vm, &vm->refs[i]);
	pthread_mutex_unlock(&vm->ref_update_mutex);

	pthread_mutex_destroy(&vm->ref_mutex);
	pthread_mutex_destroy(&vm->ref_update_mutex);
	bfree(vm->manual_ref);
	bfree(vm->mono_buf);
	bfree(vm);
}

static void voice_match_tick(void *data, float seconds)
{
	struct voice_match_data *vm = data;

	pthread_mutex_lock(&vm->ref_update_mutex);
	uint64_t t = os_gettime_ns();
	bool due = (t - vm->ref_check_time) > REF_RECONCILE_NS;
	if (due)
		vm->ref_check_time = t;
	pthread_mutex_unlock(&vm->ref_update_mutex);

	if (due)
		reconcile_refs(vm);

	UNUSED_PARAMETER(seconds);
}

static struct obs_audio_data *voice_match_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct voice_match_data *vm = data;
	const uint32_t frames = audio->frames;
	float **samples = (float **)audio->data;

	if (!frames || !samples[0])
		return audio;

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

	float mic_db;
	bool voiced = vm_side_analyze(&vm->mic, vm->mono_buf, frames, &mic_db);
	vm->mic_vad = vm->mic.speaking;
	if (voiced)
		vm_level_update(&vm->mic_level, mic_db);

	/* decide target gain */
	bool mic_warm = vm->mic_level.voiced_blocks >= WARM_BLOCKS;

	pthread_mutex_lock(&vm->ref_mutex);
	bool ref_warm = vm->ref_level.voiced_blocks >= WARM_BLOCKS;
	float ref_speech = vm->ref_level.speech_db;
	pthread_mutex_unlock(&vm->ref_mutex);

	bool has_ref = count_refs(vm) > 0;

	float target = vm->cur_gain_db;
	if (has_ref && mic_warm && ref_warm) {
		target = ref_speech + vm->target_offset_db - vm->mic_level.speech_db;
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

	obs_properties_add_bool(props, S_AUTO_REFS, TEXT_AUTO_REFS);

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
	obs_data_set_default_bool(s, S_AUTO_REFS, true);
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

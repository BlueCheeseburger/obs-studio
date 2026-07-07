#include "RecordingHealth.hpp"

#include <OBSApp.hpp>

#include <media-io/audio-math.h>

#include <qt-wrappers.hpp>

#include <QFileInfo>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "moc_RecordingHealth.cpp"

/* live watchdog tuning */
static constexpr int kPollMs = 2000;
static constexpr double kLiveSkipRatio = 0.10;  /* >10% skipped in a window */
static constexpr int kLiveBadPolls = 3;         /* ~6 s sustained */
static constexpr uint32_t kLiveMinFrames = 30;  /* ignore tiny windows */

/* live freeze watchdog tuning.
 *
 * Audio level variation alone is NOT a reliable signal that the picture
 * should be changing — a podcast, music, or voice call can play happily for
 * a long time while the screen is legitimately static (reading, AFK, a
 * paused/menu screen). The real discriminator is whether the encode
 * pipeline itself has shown any sign of trouble: a genuine capture/render
 * hitch almost always leaves some trace in the skip-ratio, whereas ordinary
 * inactivity shows a perfectly clean 0% the entire time (confirmed against
 * this fork's own false-positive recordings). So a freeze only alerts when
 * BOTH the audio is active AND the live encoder has shown at least a little
 * skipping at some point in the recording — not just one or the other. */
static constexpr uint32_t kFreezeSampleW = 160;
static constexpr uint32_t kFreezeSampleH = 90;
static constexpr float kFreezeDiffThreshold = 1.5f;    /* mean abs luma diff below this = "unchanged" */
static constexpr int kFreezeAlertSeconds = 45;         /* sustained frozen picture before considering an alert */
static constexpr int kAudioRingSeconds = 12;           /* rolling window for audio activity */
static constexpr float kAudioBlockSeconds = 0.25f;     /* sub-window audio level is bucketed into */
static constexpr float kAudioNoiseFloorDb = -55.0f;    /* below this, audio is considered silent */
static constexpr float kAudioActivityMinStdDb = 3.0f;  /* min level variation to count as "active" */

/* file probe tuning */
static constexpr double kFileFpsFactor = 0.70;    /* PTS gaps: second is "bad" below 70% of nominal */
static constexpr int kFileMinBadSeconds = 5;      /* PTS gaps: alert at this many bad seconds */
static constexpr int kContentPacketBytes = 1024;  /* packets above this carry real content */
static constexpr int kBaselineSeconds = 180;      /* self-baseline window at file start */
static constexpr double kCollapseFactor = 0.50;   /* "collapsed" = below half the baseline */
static constexpr int kCollapseMinRun = 30;        /* alert on this many consecutive bad s */
static constexpr double kCollapseMinShare = 0.20; /* ... or this share of the whole file */
static constexpr double kCollapseCorroborationRatio = 0.02; /* live skip ratio needed to trust a collapse */

RecordingHealthMonitor::RecordingHealthMonitor(QObject *parent) : QObject(parent) {}

RecordingHealthMonitor::~RecordingHealthMonitor()
{
	if (rawVideoTapActive)
		obs_remove_raw_video_callback(&RecordingHealthMonitor::RawVideoFrame, this);
	if (rawAudioTapActive)
		obs_remove_raw_audio_callback(0, &RecordingHealthMonitor::RawAudioFrame, this);
}

/* ------------------------------------------------------------------------- */
/* live watchdog                                                              */

void RecordingHealthMonitor::recordingStarted(obs_output_t *output)
{
	weakOutput = obs_output_get_weak_output(output);

	video_t *video = obs_output_video(output);
	lastSkipped = video ? video_output_get_skipped_frames(video) : 0;
	lastTotal = video ? video_output_get_total_frames(video) : 0;
	badPolls = 0;
	liveAlerted = false;
	maxLiveSkipRatio = 0.0;

	if (!pollTimer) {
		pollTimer = new QTimer(this);
		connect(pollTimer, &QTimer::timeout, this, &RecordingHealthMonitor::poll);
	}
	pollTimer->start(kPollMs);

	/* live freeze watchdog: reset state and attach the raw taps */
	freezeMutex.lock();
	prevVideoSample.clear();
	frozenSeconds = 0;
	freezeAlerted = false;
	freezeMutex.unlock();

	audioMutex.lock();
	audioBlockAccum = 0.0f;
	audioBlockAccumSamples = 0;
	audioLevelRingDb.clear();
	audioMutex.unlock();

	if (!rawVideoTapActive) {
		struct obs_video_info ovi;
		uint32_t divisor = 60;
		if (obs_get_video_info(&ovi) && ovi.fps_den > 0 && ovi.fps_num > 0)
			divisor = std::max<uint32_t>(1, ovi.fps_num / ovi.fps_den);

		struct video_scale_info conv = {};
		conv.format = VIDEO_FORMAT_Y800;
		conv.width = kFreezeSampleW;
		conv.height = kFreezeSampleH;
		conv.range = VIDEO_RANGE_DEFAULT;
		conv.colorspace = VIDEO_CS_DEFAULT;

		obs_add_raw_video_callback2(&conv, divisor, &RecordingHealthMonitor::RawVideoFrame, this);
		rawVideoTapActive = true;
	}

	if (!rawAudioTapActive) {
		obs_add_raw_audio_callback(0, nullptr, &RecordingHealthMonitor::RawAudioFrame, this);
		rawAudioTapActive = true;
	}
}

void RecordingHealthMonitor::recordingStopped()
{
	if (pollTimer)
		pollTimer->stop();
	weakOutput = nullptr;

	if (rawVideoTapActive) {
		obs_remove_raw_video_callback(&RecordingHealthMonitor::RawVideoFrame, this);
		rawVideoTapActive = false;
	}
	if (rawAudioTapActive) {
		obs_remove_raw_audio_callback(0, &RecordingHealthMonitor::RawAudioFrame, this);
		rawAudioTapActive = false;
	}
}

void RecordingHealthMonitor::poll()
{
	OBSOutputAutoRelease output = obs_weak_output_get_output(weakOutput);
	if (!output)
		return;

	video_t *video = obs_output_video(output);
	if (!video)
		return;

	uint32_t skipped = video_output_get_skipped_frames(video);
	uint32_t total = video_output_get_total_frames(video);

	uint32_t dSkipped = skipped - lastSkipped;
	uint32_t dTotal = total - lastTotal;
	lastSkipped = skipped;
	lastTotal = total;

	if (dTotal < kLiveMinFrames)
		return;

	double ratio = (double)dSkipped / (double)dTotal;
	if (ratio > maxLiveSkipRatio)
		maxLiveSkipRatio = ratio;

	if (ratio > kLiveSkipRatio) {
		badPolls++;
	} else {
		badPolls = 0;
	}

	if (badPolls >= kLiveBadPolls && !liveAlerted) {
		liveAlerted = true;
		int pct = (int)(ratio * 100.0);
		blog(LOG_WARNING,
		     "[recording health] live: encoder skipping %d%% of frames "
		     "(%u of %u in the last window)",
		     pct, dSkipped, dTotal);
		emit healthAlert(QTStr("Basic.RecordingHealth.LiveAlert").arg(pct));
	}

	/* live freeze watchdog. A frozen picture by itself proves nothing —
	 * that's just as consistent with reading/AFK/a paused menu (with a
	 * podcast or music playing) as with a real problem. Only alert when
	 * BOTH: audio is genuinely active (rules out plain inactivity) AND
	 * the encode pipeline has shown at least a little skipping somewhere
	 * in this recording (rules out "podcast playing over a static
	 * screen," which is clean the entire time). Fires at most once per
	 * recording. */
	freezeMutex.lock();
	int frozen = frozenSeconds;
	bool alreadyAlerted = freezeAlerted;
	freezeMutex.unlock();

	if (frozen < kFreezeAlertSeconds || alreadyAlerted)
		return;

	if (maxLiveSkipRatio <= kCollapseCorroborationRatio)
		return;

	audioMutex.lock();
	std::vector<float> levels(audioLevelRingDb.begin(), audioLevelRingDb.end());
	audioMutex.unlock();

	if (levels.size() < 4)
		return;

	double sum = 0.0;
	for (float v : levels)
		sum += v;
	double mean = sum / (double)levels.size();

	double varSum = 0.0;
	for (float v : levels) {
		double d = (double)v - mean;
		varSum += d * d;
	}
	double stddev = std::sqrt(varSum / (double)levels.size());

	bool audioActive = mean > kAudioNoiseFloorDb && stddev > kAudioActivityMinStdDb;
	if (!audioActive)
		return;

	freezeMutex.lock();
	freezeAlerted = true;
	freezeMutex.unlock();

	blog(LOG_WARNING,
	     "[recording health] live: picture has not visibly changed for %d s while audio "
	     "shows activity (mean %.1f dB, variation %.1f dB) - possible capture/encoder issue",
	     frozen, mean, stddev);
	emit healthAlert(QTStr("Basic.RecordingHealth.LiveFreezeAlert").arg(frozen));
}

void RecordingHealthMonitor::RawVideoFrame(void *param, struct video_data *frame)
{
	auto *self = static_cast<RecordingHealthMonitor *>(param);
	if (!frame || !frame->data[0])
		return;
	self->handleVideoSample(frame->data[0], frame->linesize[0], kFreezeSampleW, kFreezeSampleH);
}

void RecordingHealthMonitor::handleVideoSample(const uint8_t *data, uint32_t linesize, uint32_t width,
					       uint32_t height)
{
	std::vector<uint8_t> sample(width * height);
	for (uint32_t y = 0; y < height; y++)
		memcpy(sample.data() + y * width, data + (size_t)y * linesize, width);

	freezeMutex.lock();

	if (prevVideoSample.size() == sample.size()) {
		uint64_t sum = 0;
		for (size_t i = 0; i < sample.size(); i++)
			sum += (uint64_t)std::abs((int)sample[i] - (int)prevVideoSample[i]);
		float meanDiff = (float)sum / (float)sample.size();

		if (meanDiff < kFreezeDiffThreshold) {
			frozenSeconds++;
		} else {
			/* Content resumed: reset the streak, but freezeAlerted is
			 * intentionally NOT cleared here — once this recording has
			 * alerted, it stays quiet for the rest of the session
			 * rather than re-firing on every subsequent freeze/resume
			 * cycle (e.g. repeatedly pausing to read something). */
			frozenSeconds = 0;
		}
	}

	prevVideoSample = std::move(sample);
	freezeMutex.unlock();
}

void RecordingHealthMonitor::RawAudioFrame(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	auto *self = static_cast<RecordingHealthMonitor *>(param);
	if (!data || !data->frames || !data->data[0])
		return;

	size_t channels = audio_output_get_channels(obs_get_audio());
	if (channels == 0 || channels > MAX_AV_PLANES)
		return;

	/* mono-mix the available planes (matches the downmix approach used
	 * elsewhere in this fork, e.g. the voice-match filter's sidechain) */
	static thread_local std::vector<float> mono;
	mono.resize(data->frames);

	size_t activeCh = 0;
	const float *chans[MAX_AV_PLANES] = {};
	for (size_t c = 0; c < channels; c++) {
		if (!data->data[c])
			break;
		chans[activeCh++] = reinterpret_cast<const float *>(data->data[c]);
	}
	if (!activeCh)
		return;

	for (uint32_t i = 0; i < data->frames; i++) {
		float acc = 0.0f;
		for (size_t c = 0; c < activeCh; c++)
			acc += chans[c][i];
		mono[i] = acc / (float)activeCh;
	}

	self->handleAudioBlock(mono.data(), data->frames);
}

void RecordingHealthMonitor::handleAudioBlock(const float *samples, uint32_t frames)
{
	uint32_t sampleRate = audio_output_get_sample_rate(obs_get_audio());
	if (sampleRate == 0)
		return;

	audioMutex.lock();

	uint32_t blockFrames = (uint32_t)(kAudioBlockSeconds * (float)sampleRate);
	if (blockFrames == 0)
		blockFrames = 1;

	for (uint32_t i = 0; i < frames; i++) {
		audioBlockAccum += samples[i] * samples[i];
		audioBlockAccumSamples++;

		if (audioBlockAccumSamples >= blockFrames) {
			float rms = std::sqrt(audioBlockAccum / (float)audioBlockAccumSamples);
			float db = mul_to_db(rms);
			if (!std::isfinite(db))
				db = -100.0f;

			audioLevelRingDb.push_back(db);
			size_t maxEntries =
				(size_t)(kAudioRingSeconds / kAudioBlockSeconds) + 1;
			while (audioLevelRingDb.size() > maxEntries)
				audioLevelRingDb.pop_front();

			audioBlockAccum = 0.0f;
			audioBlockAccumSamples = 0;
		}
	}

	audioMutex.unlock();
}

/* ------------------------------------------------------------------------- */
/* file probe                                                                 */

struct ProbeResult {
	bool ok = false;
	int totalSeconds = 0;
	double nominalFps = 0.0;

	/* PTS gaps: seconds with too few packets (muxer/disk stalls) */
	int gapSeconds = 0;
	int worstGapRun = 0;

	/* content collapse: seconds where the rate of real (non-duplicate)
	 * frames fell far below the file's own early baseline */
	double baselineContentFps = 0.0;
	int collapseSeconds = 0;
	int worstCollapseRun = 0;
};

static ProbeResult probe_file(const QString &path, double nominalFps)
{
	ProbeResult res;

	AVFormatContext *fmt = nullptr;
	std::string utf8 = path.toStdString();
	if (avformat_open_input(&fmt, utf8.c_str(), nullptr, nullptr) < 0)
		return res;

	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return res;
	}

	int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0) {
		avformat_close_input(&fmt);
		return res;
	}

	AVStream *stream = fmt->streams[vIdx];

	/* nominal rate: caller > container-declared > bail */
	if (nominalFps <= 0.0 && stream->r_frame_rate.den > 0)
		nominalFps = av_q2d(stream->r_frame_rate);
	if (nominalFps < 10.0) {
		avformat_close_input(&fmt);
		return res;
	}
	res.nominalFps = nominalFps;

	/* per second of presentation time: all packets + content packets
	 * (frozen duplicate frames encode to near-empty delta packets, so
	 * packets above a small size floor approximate real picture updates) */
	std::vector<uint32_t> total;
	std::vector<uint32_t> content;
	double tb = av_q2d(stream->time_base);

	AVPacket *pkt = av_packet_alloc();
	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == vIdx) {
			int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			if (ts != AV_NOPTS_VALUE) {
				double sec = (double)ts * tb;
				if (sec >= 0.0 && sec < 24.0 * 3600.0) {
					size_t idx = (size_t)sec;
					if (total.size() <= idx) {
						total.resize(idx + 1, 0);
						content.resize(idx + 1, 0);
					}
					total[idx]++;
					if (pkt->size > kContentPacketBytes)
						content[idx]++;
				}
			}
		}
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
	avformat_close_input(&fmt);

	/* skip the first and last second (partial buckets) */
	if (total.size() < 4)
		return res;

	const size_t first = 1;
	const size_t last = total.size() - 1;

	/* 1) PTS gaps (muxer/disk stalls) */
	uint32_t gapThreshold = (uint32_t)(nominalFps * kFileFpsFactor);
	int run = 0;

	for (size_t i = first; i < last; i++) {
		res.totalSeconds++;
		if (total[i] < gapThreshold) {
			res.gapSeconds++;
			run++;
			if (run > res.worstGapRun)
				res.worstGapRun = run;
		} else {
			run = 0;
		}
	}

	/* 2) content collapse vs the file's own early baseline (75th
	 * percentile of content fps over the first minutes). Using a
	 * self-baseline means intentionally static footage (idle desktop)
	 * does not alert; a collapse from healthy to slideshow does. */
	size_t baseEnd = first + (size_t)kBaselineSeconds;
	if (baseEnd > last)
		baseEnd = last;

	std::vector<uint32_t> base(content.begin() + first, content.begin() + baseEnd);
	if (base.size() >= 30) {
		std::sort(base.begin(), base.end());
		double baseline = (double)base[base.size() * 3 / 4];
		res.baselineContentFps = baseline;

		/* a tiny baseline means the content was static from the start
		 * (e.g. idle desktop); nothing meaningful to compare against */
		if (baseline >= nominalFps * 0.20) {
			double collapse = baseline * kCollapseFactor;
			run = 0;
			for (size_t i = first; i < last; i++) {
				if ((double)content[i] < collapse) {
					res.collapseSeconds++;
					run++;
					if (run > res.worstCollapseRun)
						res.worstCollapseRun = run;
				} else {
					run = 0;
				}
			}
		}
	}

	res.ok = true;
	return res;
}

void RecordingHealthMonitor::checkFileAsync(const QString &path, double nominalFps)
{
	if (path.isEmpty())
		return;

	if (nominalFps <= 0.0) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ovi.fps_den > 0)
			nominalFps = (double)ovi.fps_num / (double)ovi.fps_den;
	}

	QPointer<RecordingHealthMonitor> guard(this);
	double liveSkipRatio = maxLiveSkipRatio;

	std::thread([guard, path, nominalFps, liveSkipRatio]() {
		ProbeResult res = probe_file(path, nominalFps);

		QString fileName = QFileInfo(path).fileName();

		if (!res.ok) {
			blog(LOG_INFO, "[recording health] probe: could not analyze '%s'", QT_TO_UTF8(fileName));
			return;
		}

		int gapPct = res.totalSeconds > 0 ? res.gapSeconds * 100 / res.totalSeconds : 0;
		int colPct = res.totalSeconds > 0 ? res.collapseSeconds * 100 / res.totalSeconds : 0;

		blog(LOG_INFO,
		     "[recording health] probe '%s': %d s analyzed; PTS gaps %d s "
		     "(%d%%, worst run %d s); content baseline %.1f fps, collapse "
		     "%d s (%d%%, worst run %d s); live skip ratio during recording: %.1f%%",
		     QT_TO_UTF8(fileName), res.totalSeconds, res.gapSeconds, gapPct, res.worstGapRun,
		     res.baselineContentFps, res.collapseSeconds, colPct, res.worstCollapseRun,
		     liveSkipRatio * 100.0);

		bool gapAlert = res.gapSeconds >= kFileMinBadSeconds;
		bool collapseCandidate = res.worstCollapseRun >= kCollapseMinRun ||
					 (res.totalSeconds > 0 &&
					  (double)res.collapseSeconds / (double)res.totalSeconds >= kCollapseMinShare);

		/* A content collapse looks identical whether it's an encoder fault
		 * or just static footage (paused game, menu, idle desktop) — NVENC
		 * compresses both down to near-nothing. Only trust it as a real
		 * fault if the live encoder was also actually struggling at some
		 * point during this recording; otherwise it's almost certainly
		 * static content, so log it but don't alert. */
		bool collapseAlert = collapseCandidate && liveSkipRatio > kCollapseCorroborationRatio;

		if (collapseCandidate && !collapseAlert) {
			blog(LOG_INFO,
			     "[recording health] content collapse detected in '%s' but the live "
			     "encoder never skipped frames (max %.1f%% in any window) - likely "
			     "static/idle content, not alerting",
			     QT_TO_UTF8(fileName), liveSkipRatio * 100.0);
		}

		if (!gapAlert && !collapseAlert)
			return;

		int pct = gapAlert ? gapPct : colPct;
		int worst = gapAlert ? res.worstGapRun : res.worstCollapseRun;

		QString msg = QTStr("Basic.RecordingHealth.FileAlert").arg(fileName).arg(pct).arg(worst);

		/* hop to the UI thread; guard against monitor teardown */
		QMetaObject::invokeMethod(
			qApp,
			[guard, msg]() {
				if (guard)
					emit guard->healthAlert(msg);
			},
			Qt::QueuedConnection);
	}).detach();
}

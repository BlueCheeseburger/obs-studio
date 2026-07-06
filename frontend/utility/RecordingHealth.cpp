#include "RecordingHealth.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QFileInfo>
#include <QTimer>

#include <algorithm>
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

/* file probe tuning */
static constexpr double kFileFpsFactor = 0.70;    /* PTS gaps: second is "bad" below 70% of nominal */
static constexpr int kFileMinBadSeconds = 5;      /* PTS gaps: alert at this many bad seconds */
static constexpr int kContentPacketBytes = 1024;  /* packets above this carry real content */
static constexpr int kBaselineSeconds = 180;      /* self-baseline window at file start */
static constexpr double kCollapseFactor = 0.50;   /* "collapsed" = below half the baseline */
static constexpr int kCollapseMinRun = 30;        /* alert on this many consecutive bad s */
static constexpr double kCollapseMinShare = 0.20; /* ... or this share of the whole file */

RecordingHealthMonitor::RecordingHealthMonitor(QObject *parent) : QObject(parent) {}

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

	if (!pollTimer) {
		pollTimer = new QTimer(this);
		connect(pollTimer, &QTimer::timeout, this, &RecordingHealthMonitor::poll);
	}
	pollTimer->start(kPollMs);
}

void RecordingHealthMonitor::recordingStopped()
{
	if (pollTimer)
		pollTimer->stop();
	weakOutput = nullptr;
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

	std::thread([guard, path, nominalFps]() {
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
		     "%d s (%d%%, worst run %d s)",
		     QT_TO_UTF8(fileName), res.totalSeconds, res.gapSeconds, gapPct, res.worstGapRun,
		     res.baselineContentFps, res.collapseSeconds, colPct, res.worstCollapseRun);

		bool gapAlert = res.gapSeconds >= kFileMinBadSeconds;
		bool collapseAlert = res.worstCollapseRun >= kCollapseMinRun ||
				     (res.totalSeconds > 0 &&
				      (double)res.collapseSeconds / (double)res.totalSeconds >= kCollapseMinShare);

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

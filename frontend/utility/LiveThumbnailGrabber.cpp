/******************************************************************************
    Auto live-stream thumbnail grabber. See header for an overview.
******************************************************************************/

#include "LiveThumbnailGrabber.hpp"

#include <utility/ScreenshotObj.hpp>
#include <widgets/OBSBasic.hpp>

#include <QTimer>

#include <cmath>

#include "moc_LiveThumbnailGrabber.cpp"

/* Analysis resolution. The grabbed frame is downscaled to this size before the
 * quality filters run, which both smooths out sensor/encoder noise and keeps
 * the per-frame analysis trivially cheap. 16:9. */
static constexpr int kAnalyzeW = 160;
static constexpr int kAnalyzeH = 90;

/* Thumbnail upload resolution handed to YouTube (16:9, >= 640px wide). */
static constexpr int kThumbW = 1280;
static constexpr int kThumbH = 720;

/* Filter thresholds, all on a 0-255 luminance scale. Deliberately
 * conservative: the cost of rejecting a usable frame is just a 15s retry,
 * while a false-accept publishes a bad thumbnail, so we only reject frames we
 * are confident are bad. */
static constexpr double kNearBlackMean = 16.0;
static constexpr double kNearWhiteMean = 240.0;
static constexpr int kSolidColorRange = 6;   // max-min luma
static constexpr double kLowVarianceStd = 6.0;
static constexpr double kFrozenMeanAbsDiff = 2.0;

/* Minimum visual change (mean abs luma diff vs. the last uploaded frame)
 * required before we spend an upload. Skips redundant uploads of a thumbnail
 * that looks the same as the current one. */
static constexpr double kMinUploadChange = 8.0;

/* Mean absolute per-pixel difference between two same-size grayscale
 * fingerprints. Returns a large value if they are missing or mismatched. */
static double signatureMeanAbsDiff(const QImage &a, const QImage &b)
{
	if (a.isNull() || b.isNull() || a.width() != b.width() || a.height() != b.height())
		return 255.0;

	const int w = a.width();
	const int h = a.height();
	double sum = 0.0;
	for (int y = 0; y < h; ++y) {
		const uchar *pa = a.constScanLine(y);
		const uchar *pb = b.constScanLine(y);
		for (int x = 0; x < w; ++x)
			sum += std::abs(int(pa[x]) - int(pb[x]));
	}
	return sum / (w * h);
}

LiveThumbnailGrabber::LiveThumbnailGrabber(QObject *parent) : QObject(parent) {}

LiveThumbnailGrabber::~LiveThumbnailGrabber()
{
	stop();
}

void LiveThumbnailGrabber::start()
{
	if (running)
		return;

	running = true;
	attemptsThisCycle = 0;
	lastSignature = QImage();
	lastUploadedSignature = QImage();

	if (!cycleTimer) {
		cycleTimer = new QTimer(this);
		connect(cycleTimer, &QTimer::timeout, this, [this]() {
			/* A scheduled cycle begins: reset the per-cycle retry
			 * counter and take the first attempt. */
			attemptsThisCycle = 0;
			beginAttempt();
		});
	}
	if (!retryTimer) {
		retryTimer = new QTimer(this);
		retryTimer->setSingleShot(true);
		connect(retryTimer, &QTimer::timeout, this, &LiveThumbnailGrabber::beginAttempt);
	}

	cycleTimer->start(kCycleIntervalMs);
	blog(LOG_INFO, "[LiveThumbnail] Auto-thumbnail grabber started (every %d min)", kCycleIntervalMs / 60000);
}

void LiveThumbnailGrabber::stop()
{
	if (!running && !cycleTimer)
		return;

	running = false;
	captureInFlight = false;

	if (cycleTimer)
		cycleTimer->stop();
	if (retryTimer)
		retryTimer->stop();

	/* ScreenshotObj cleans itself up via deleteLater once its render
	 * finishes; dropping our QPointer is enough. Qt also auto-disconnects
	 * the imageReady connection if it is destroyed first. */
	activeShot = nullptr;

	blog(LOG_INFO, "[LiveThumbnail] Auto-thumbnail grabber stopped");
}

void LiveThumbnailGrabber::beginAttempt()
{
	if (!running)
		return;

	if (captureInFlight) {
		/* A previous grab's readback hasn't finished yet. Skip this
		 * scheduled tick rather than stacking captures. */
		blog(LOG_DEBUG, "[LiveThumbnail] Skipping attempt, capture already in flight");
		return;
	}

	attemptsThisCycle++;

	QString reason;
	if (!gateAllowsCapture(reason)) {
		handleRejected(QT_TO_UTF8(reason));
		return;
	}

	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	if (!scene) {
		handleRejected("no current scene");
		return;
	}

	ScreenshotObj *shot = new ScreenshotObj(scene);
	shot->setSaveToFile(false);
	shot->setForceSDR(true);
	shot->setSize(kThumbW, kThumbH);
	connect(shot, &ScreenshotObj::imageReady, this, &LiveThumbnailGrabber::onFrameReady);

	activeShot = shot;
	captureInFlight = true;
}

void LiveThumbnailGrabber::onFrameReady(QImage image)
{
	captureInFlight = false;
	activeShot = nullptr;

	if (!running)
		return;

	QString reason;
	bool acceptable = frameIsAcceptable(image, reason);

	if (acceptable) {
		if (retryTimer)
			retryTimer->stop();

		/* Skip the upload if this frame is visually unchanged from the
		 * one we last uploaded — the current thumbnail is already good,
		 * and this is the main quota saver. lastSignature was just set to
		 * this frame's fingerprint by frameIsAcceptable(). */
		if (!lastUploadedSignature.isNull() &&
		    signatureMeanAbsDiff(lastSignature, lastUploadedSignature) < kMinUploadChange) {
			blog(LOG_INFO, "[LiveThumbnail] Frame accepted but unchanged from last "
				       "upload; keeping current thumbnail");
			return;
		}

		blog(LOG_INFO, "[LiveThumbnail] Frame accepted, updating broadcast thumbnail");
		lastUploadedSignature = lastSignature;
		OBSBasic::Get()->OnAutoThumbnailAccepted(image);
	} else {
		handleRejected(QT_TO_UTF8(reason));
	}
}

void LiveThumbnailGrabber::handleRejected(const char *reason)
{
	if (!running)
		return;

	if (attemptsThisCycle < kMaxAttemptsPerCycle) {
		blog(LOG_INFO, "[LiveThumbnail] Frame rejected (%s); retry %d/%d in %ds", reason, attemptsThisCycle,
		     kMaxAttemptsPerCycle - 1, kRetryIntervalMs / 1000);
		if (retryTimer)
			retryTimer->start(kRetryIntervalMs);
	} else {
		blog(LOG_INFO,
		     "[LiveThumbnail] Frame rejected (%s); giving up this cycle, "
		     "keeping previous thumbnail",
		     reason);
		/* cycleTimer keeps running; the next scheduled cycle will try
		 * again with a fresh attempt counter. */
	}
}

bool LiveThumbnailGrabber::gateAllowsCapture(QString &reason)
{
	/* Block while a visual scene transition is mid-flight. Channel 0 holds
	 * the active transition; a fractional time means it is animating. */
	OBSSourceAutoRelease transition = obs_get_output_source(0);
	if (transition) {
		float t = obs_transition_get_time(transition);
		if (t > 0.0f && t < 1.0f) {
			reason = "scene transition in progress";
			return false;
		}
	}

	/* Honor the user's per-scene exclusion flag. */
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	if (scene) {
		OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
		if (obs_data_get_bool(priv, "exclude_from_auto_thumbnail")) {
			reason = "current scene excluded by user";
			return false;
		}
	}

	return true;
}

bool LiveThumbnailGrabber::frameIsAcceptable(const QImage &image, QString &reason)
{
	if (image.isNull() || image.width() == 0 || image.height() == 0) {
		reason = "empty frame";
		return false;
	}

	QImage gray = image.scaled(kAnalyzeW, kAnalyzeH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
			      .convertToFormat(QImage::Format_Grayscale8);

	const int w = gray.width();
	const int h = gray.height();
	if (w == 0 || h == 0) {
		reason = "empty frame";
		return false;
	}

	double sum = 0.0;
	double sumSq = 0.0;
	int minLuma = 255;
	int maxLuma = 0;
	const int count = w * h;

	for (int y = 0; y < h; ++y) {
		const uchar *line = gray.constScanLine(y);
		for (int x = 0; x < w; ++x) {
			int v = line[x];
			sum += v;
			sumSq += double(v) * v;
			if (v < minLuma)
				minLuma = v;
			if (v > maxLuma)
				maxLuma = v;
		}
	}

	const double mean = sum / count;
	const double variance = (sumSq / count) - (mean * mean);
	const double stddev = variance > 0.0 ? std::sqrt(variance) : 0.0;

	/* 1. Near-black (loading fade, monitor off, transition hold). */
	if (mean < kNearBlackMean) {
		reason = "near-black";
		lastSignature = gray;
		return false;
	}

	/* 2. Near-white / blown out (flash, overexposure). */
	if (mean > kNearWhiteMean) {
		reason = "near-white / overexposed";
		lastSignature = gray;
		return false;
	}

	/* 3. Solid color (flat fill, color bars, transition hold). */
	if ((maxLuma - minLuma) < kSolidColorRange) {
		reason = "solid / flat color";
		lastSignature = gray;
		return false;
	}

	/* 4. Uniform low variance (near-blank, minimal detail). */
	if (stddev < kLowVarianceStd) {
		reason = "low detail / near-uniform";
		lastSignature = gray;
		return false;
	}

	/* 5. Frozen / duplicate (stream stutter or static screen): compare
	 * against the previously analyzed frame. */
	if (!lastSignature.isNull()) {
		if (signatureMeanAbsDiff(gray, lastSignature) < kFrozenMeanAbsDiff) {
			reason = "frozen / duplicate frame";
			lastSignature = gray;
			return false;
		}
	}

	lastSignature = gray;
	return true;
}

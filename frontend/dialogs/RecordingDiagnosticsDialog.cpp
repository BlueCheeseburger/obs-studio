#include "RecordingDiagnosticsDialog.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <utility/BasicOutputHandler.hpp>

#include <qt-wrappers.hpp>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "moc_RecordingDiagnosticsDialog.cpp"

/* ------------------------------------------------------------------------- */
/* tuning                                                                     */

/* Total live-sampling window. Everything except the file tail scan finishes
 * within this; kept short so the whole run stays a "quick check". */
static constexpr int kSampleTickMs = 100;
static constexpr int kSampleTicks = 30; /* 3.0 s */

/* freeze sampling: small greyscale frames, a few per second */
static constexpr uint32_t kFreezeW = 160;
static constexpr uint32_t kFreezeH = 90;
static constexpr int kFreezeFps = 3;
/* mean absolute luma difference below this counts as "no visible change" */
static constexpr float kFreezeDiffThreshold = 1.5f;

/* audio is "present" above this peak level */
static constexpr float kAudioSilenceFloor = 0.0025f; /* ~ -52 dBFS */

/* file tail scan */
static constexpr int64_t kTailBytes = 192LL * 1024 * 1024; /* how far back to seek */
static constexpr int kTailMaxPackets = 60000;              /* hard work cap */
static constexpr double kTailLowFpsFactor = 0.70;          /* second is "bad" below 70% nominal */

/* The point at which OBS force-stops recording. Mirrors
 * MBYTES_LEFT_STOP_REC in OBSBasic_Recording.cpp - the single disk-space
 * threshold in the codebase (verified: one definition, one call site in
 * LowDiskSpace()). Note this is NOT the same as the Stats dialog's 1 GB /
 * 5 GB colour thresholds, which only tint the text and stop nothing. */
static constexpr uint64_t kStopRecordingFreeBytes = 50ULL * 1024ULL * 1024ULL;

/* severity levels for SetRowResult */
enum { SevPass = 0, SevWarn = 1, SevFail = 2, SevInfo = 3 };

static const char *kSpinnerFrames[] = {"|", "/", "-", "\\"};

/* ------------------------------------------------------------------------- */

RecordingDiagnosticsDialog::RecordingDiagnosticsDialog(OBSBasic *main_, QWidget *parent)
	: QDialog(parent),
	  main(main_)
{
	setWindowTitle(QTStr("RecordingDiagnostics.Title"));
	setMinimumWidth(620);

	BuildUi();

	spinnerTimer = new QTimer(this);
	connect(spinnerTimer, &QTimer::timeout, this, [this]() {
		for (auto &c : checks) {
			if (!c.running || c.done)
				continue;
			c.spinnerFrame = (c.spinnerFrame + 1) % 4;
			c.statusLabel->setText(QString::fromLatin1(kSpinnerFrames[c.spinnerFrame]));
		}
	});

	/* run immediately on open - the point of the button is "tell me now" */
	QTimer::singleShot(0, this, &RecordingDiagnosticsDialog::StartRun);
}

RecordingDiagnosticsDialog::~RecordingDiagnosticsDialog()
{
	DetachTaps();
}

void RecordingDiagnosticsDialog::BuildUi()
{
	auto *root = new QVBoxLayout(this);

	auto *intro = new QLabel(QTStr("RecordingDiagnostics.Intro"), this);
	intro->setWordWrap(true);
	setClasses(intro, "text-muted");
	root->addWidget(intro);

	grid = new QGridLayout();
	grid->setColumnStretch(0, 0); /* status glyph */
	grid->setColumnStretch(1, 0); /* title */
	grid->setColumnStretch(2, 1); /* progress */
	grid->setColumnStretch(3, 2); /* result */
	root->addLayout(grid);

	static const char *titleKeys[CheckCount] = {
		"RecordingDiagnostics.Check.Output", "RecordingDiagnostics.Check.Encoder",
		"RecordingDiagnostics.Check.Freeze", "RecordingDiagnostics.Check.Audio",
		"RecordingDiagnostics.Check.File",   "RecordingDiagnostics.Check.Disk",
	};

	checks.resize(CheckCount);
	for (int i = 0; i < CheckCount; i++) {
		DiagCheck &c = checks[i];
		c.title = QTStr(titleKeys[i]);

		c.statusLabel = new QLabel(QStringLiteral(" "), this);
		c.statusLabel->setFixedWidth(20);
		c.statusLabel->setAlignment(Qt::AlignCenter);

		c.titleLabel = new QLabel(c.title, this);
		c.titleLabel->setMinimumWidth(150);

		c.progress = new QProgressBar(this);
		c.progress->setRange(0, 100);
		c.progress->setValue(0);
		c.progress->setTextVisible(true);
		c.progress->setFixedHeight(14);

		c.resultLabel = new QLabel(QStringLiteral("—"), this);
		c.resultLabel->setWordWrap(true);
		setClasses(c.resultLabel, "text-muted");

		grid->addWidget(c.statusLabel, i, 0);
		grid->addWidget(c.titleLabel, i, 1);
		grid->addWidget(c.progress, i, 2);
		grid->addWidget(c.resultLabel, i, 3);
	}

	summaryLabel = new QLabel(QStringLiteral(" "), this);
	summaryLabel->setWordWrap(true);
	root->addSpacing(6);
	root->addWidget(summaryLabel);

	auto *buttons = new QHBoxLayout();
	buttons->addStretch(1);
	runButton = new QPushButton(QTStr("RecordingDiagnostics.RunAgain"), this);
	closeButton = new QPushButton(QTStr("Close"), this);
	buttons->addWidget(runButton);
	buttons->addWidget(closeButton);
	root->addLayout(buttons);

	connect(runButton, &QPushButton::clicked, this, &RecordingDiagnosticsDialog::StartRun);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

/* ------------------------------------------------------------------------- */
/* row helpers                                                                */

void RecordingDiagnosticsDialog::SetRowRunning(int id)
{
	DiagCheck &c = checks[id];
	c.running = true;
	c.done = false;
	c.progress->setValue(0);
	c.statusLabel->setText(QString::fromLatin1(kSpinnerFrames[0]));
	c.resultLabel->setText(QTStr("RecordingDiagnostics.Working"));
	setClasses(c.resultLabel, "text-muted");
}

void RecordingDiagnosticsDialog::SetRowProgress(int id, int pct)
{
	checks[id].progress->setValue(std::clamp(pct, 0, 100));
}

void RecordingDiagnosticsDialog::SetRowResult(int id, int severity, const QString &text)
{
	DiagCheck &c = checks[id];
	c.running = false;
	c.done = true;
	c.progress->setValue(100);
	c.resultLabel->setText(text);

	switch (severity) {
	case SevPass:
		c.statusLabel->setText(QStringLiteral("✓")); /* check mark */
		setClasses(c.statusLabel, "text-success");
		setClasses(c.resultLabel, "");
		break;
	case SevWarn:
		c.statusLabel->setText(QStringLiteral("!"));
		setClasses(c.statusLabel, "text-warning");
		setClasses(c.resultLabel, "text-warning");
		warnCount++;
		break;
	case SevFail:
		c.statusLabel->setText(QStringLiteral("✕")); /* ballot X */
		setClasses(c.statusLabel, "text-danger");
		setClasses(c.resultLabel, "text-danger");
		failCount++;
		break;
	default:
		c.statusLabel->setText(QStringLiteral("–"));
		setClasses(c.statusLabel, "text-muted");
		setClasses(c.resultLabel, "text-muted");
		break;
	}

	completedCount++;
	if (completedCount >= CheckCount)
		FinishRun();
}

/* ------------------------------------------------------------------------- */
/* run control                                                                */

void RecordingDiagnosticsDialog::StartRun()
{
	DetachTaps();

	failCount = 0;
	warnCount = 0;
	completedCount = 0;
	sampleTicks = 0;
	frameSamples = 0;
	changedSamples = 0;
	maxDiff = 0;
	audioBlocks = 0;
	audioActiveBlocks = 0;
	audioPeakMilli = 0;
	prevFrame.clear();

	runButton->setEnabled(false);
	summaryLabel->setText(QTStr("RecordingDiagnostics.Running"));
	setClasses(summaryLabel, "text-muted");

	for (int i = 0; i < CheckCount; i++) {
		checks[i].done = false;
		checks[i].running = false;
		checks[i].progress->setValue(0);
		checks[i].statusLabel->setText(QStringLiteral(" "));
		checks[i].resultLabel->setText(QStringLiteral("—"));
		setClasses(checks[i].resultLabel, "text-muted");
		setClasses(checks[i].statusLabel, "");
	}

	spinnerTimer->start(120);

	RunInstantChecks();
}

void RecordingDiagnosticsDialog::FinishRun()
{
	spinnerTimer->stop();
	runButton->setEnabled(true);

	if (failCount > 0) {
		summaryLabel->setText(QTStr("RecordingDiagnostics.Summary.Bad").arg(failCount));
		setClasses(summaryLabel, "text-danger");
	} else if (warnCount > 0) {
		summaryLabel->setText(QTStr("RecordingDiagnostics.Summary.Warn").arg(warnCount));
		setClasses(summaryLabel, "text-warning");
	} else {
		summaryLabel->setText(QTStr("RecordingDiagnostics.Summary.Good"));
		setClasses(summaryLabel, "text-success");
	}
}

/* ------------------------------------------------------------------------- */
/* checks                                                                     */

void RecordingDiagnosticsDialog::RunInstantChecks()
{
	BasicOutputHandler *handler = main->GetOutputHandler();
	obs_output_t *out = handler ? (obs_output_t *)handler->fileOutput : nullptr;
	bool recording = handler && handler->RecordingActive();

	/* --- 1. output active --- */
	SetRowRunning(CheckOutput);
	if (recording && out) {
		weakOutput = obs_output_get_weak_output(out);
		SetRowResult(CheckOutput, SevPass, QTStr("RecordingDiagnostics.Output.Active"));
	} else {
		/* Nothing is recording: the live checks have nothing to measure.
		 * Say so plainly rather than reporting misleading passes. */
		SetRowResult(CheckOutput, SevWarn, QTStr("RecordingDiagnostics.Output.Inactive"));

		for (int id : {CheckEncoder, CheckFreeze, CheckAudio, CheckFile}) {
			SetRowRunning(id);
			SetRowResult(id, SevInfo, QTStr("RecordingDiagnostics.Skipped.NotRecording"));
		}
		/* disk space is still meaningful when idle */
		RunDiskCheck();
		return;
	}

	/* --- prime the sampled counters --- */
	sampledVideo = obs_output_video(out);
	if (!sampledVideo)
		sampledVideo = obs_get_video();

	startSkipped = sampledVideo ? video_output_get_skipped_frames(sampledVideo) : 0;
	startTotal = sampledVideo ? video_output_get_total_frames(sampledVideo) : 0;
	startBytes = obs_output_get_total_bytes(out);

	SetRowRunning(CheckEncoder);
	SetRowRunning(CheckFreeze);
	SetRowRunning(CheckAudio);

	AttachTaps();
	BeginSampling();

	/* the file tail scan runs concurrently on its own thread */
	SetRowRunning(CheckFile);
	RunFileTailCheckAsync();
}

void RecordingDiagnosticsDialog::BeginSampling()
{
	if (!sampleTimer) {
		sampleTimer = new QTimer(this);
		connect(sampleTimer, &QTimer::timeout, this, &RecordingDiagnosticsDialog::OnSampleTick);
	}
	sampleTicks = 0;
	sampleTimer->start(kSampleTickMs);
}

void RecordingDiagnosticsDialog::OnSampleTick()
{
	sampleTicks++;
	int pct = (sampleTicks * 100) / kSampleTicks;

	SetRowProgress(CheckEncoder, pct);
	SetRowProgress(CheckFreeze, pct);
	SetRowProgress(CheckAudio, pct);

	if (sampleTicks >= kSampleTicks) {
		sampleTimer->stop();
		FinishSamplingChecks();
	}
}

void RecordingDiagnosticsDialog::FinishSamplingChecks()
{
	DetachTaps();

	/* --- 2. encoder skipped frames over the window --- */
	uint32_t skipped = sampledVideo ? video_output_get_skipped_frames(sampledVideo) : 0;
	uint32_t total = sampledVideo ? video_output_get_total_frames(sampledVideo) : 0;
	uint32_t dSkipped = skipped - startSkipped;
	uint32_t dTotal = total - startTotal;

	if (dTotal == 0) {
		SetRowResult(CheckEncoder, SevWarn, QTStr("RecordingDiagnostics.Encoder.NoFrames"));
	} else {
		double ratio = (double)dSkipped / (double)dTotal;
		int pct = (int)std::lround(ratio * 100.0);
		QString detail = QTStr("RecordingDiagnostics.Encoder.Result").arg(pct).arg(dSkipped).arg(dTotal);
		if (ratio > 0.10)
			SetRowResult(CheckEncoder, SevFail, detail);
		else if (ratio > 0.02)
			SetRowResult(CheckEncoder, SevWarn, detail);
		else
			SetRowResult(CheckEncoder, SevPass, detail);
	}

	/* --- 3. picture freeze --- */
	int samples = frameSamples.load();
	int changed = changedSamples.load();
	if (samples < 2) {
		SetRowResult(CheckFreeze, SevWarn, QTStr("RecordingDiagnostics.Freeze.NoSamples"));
	} else if (changed == 0) {
		/* Genuinely static footage (a paused game, an idle desktop, a
		 * menu) looks identical to a broken capture over a window this
		 * short, so this is reported as a warning to look at rather
		 * than a hard failure. */
		SetRowResult(CheckFreeze, SevWarn, QTStr("RecordingDiagnostics.Freeze.Static").arg(samples));
	} else {
		SetRowResult(CheckFreeze, SevPass, QTStr("RecordingDiagnostics.Freeze.Moving").arg(changed).arg(samples));
	}

	/* --- 4. audio --- */
	int blocks = audioBlocks.load();
	int activeBlocks = audioActiveBlocks.load();
	double peak = (double)audioPeakMilli.load() / 1000.0;
	double peakDb = peak > 0.0 ? 20.0 * std::log10(peak) : -100.0;

	if (blocks == 0) {
		SetRowResult(CheckAudio, SevFail, QTStr("RecordingDiagnostics.Audio.None"));
	} else if (activeBlocks == 0) {
		SetRowResult(CheckAudio, SevWarn, QTStr("RecordingDiagnostics.Audio.Silent"));
	} else {
		SetRowResult(CheckAudio, SevPass, QTStr("RecordingDiagnostics.Audio.Ok").arg(peakDb, 0, 'f', 1));
	}

	/* --- 6. disk (needs startBytes, so runs after the window) --- */
	RunDiskCheck();
}

void RecordingDiagnosticsDialog::RunDiskCheck()
{
	SetRowRunning(CheckDisk);

	const char *path = main->GetCurrentOutputPath();
	if (!path || !*path) {
		SetRowResult(CheckDisk, SevWarn, QTStr("RecordingDiagnostics.Disk.NoPath"));
		return;
	}

	uint64_t freeBytes = os_get_free_disk_space(path);
	double freeGb = (double)freeBytes / (1024.0 * 1024.0 * 1024.0);

	/* Space actually usable before OBS force-stops the recording, not raw
	 * free space - recording halts once free space drops below the stop
	 * threshold, so that slice is unusable. */
	uint64_t usable = freeBytes > kStopRecordingFreeBytes ? freeBytes - kStopRecordingFreeBytes : 0;

	/* Write rate: prefer this session's average (stable across the varying
	 * bitrate a CQP encode produces) and fall back to the short sampling
	 * window when the recording is too young for an average to mean much. */
	BasicOutputHandler *handler = main->GetOutputHandler();
	obs_output_t *out = handler ? (obs_output_t *)handler->fileOutput : nullptr;
	bool recording = handler && handler->RecordingActive() && out;

	double bytesPerSec = 0.0;
	bool rateFromSession = false;

	if (recording) {
		uint64_t totalBytes = obs_output_get_total_bytes(out);
		int elapsed = main->GetRecordingElapsedSeconds();

		if (elapsed >= 30 && totalBytes > 0) {
			bytesPerSec = (double)totalBytes / (double)elapsed;
			rateFromSession = true;
		} else if (totalBytes > startBytes) {
			double windowSec = (double)(kSampleTicks * kSampleTickMs) / 1000.0;
			bytesPerSec = (double)(totalBytes - startBytes) / windowSec;
		}
	}

	if (bytesPerSec <= 0.0) {
		/* Not recording, or no measurable rate yet: report space only,
		 * with no time projection rather than a fabricated one. */
		SetRowResult(CheckDisk, SevInfo, QTStr("RecordingDiagnostics.Disk.SpaceOnly").arg(freeGb, 0, 'f', 1));
		return;
	}

	double secondsLeft = (double)usable / bytesPerSec;
	double hoursLeft = secondsLeft / 3600.0;
	double mbPerMin = (bytesPerSec * 60.0) / (1024.0 * 1024.0);

	QString detail = QTStr(rateFromSession ? "RecordingDiagnostics.Disk.Result"
					       : "RecordingDiagnostics.Disk.ResultShortSample")
				 .arg(hoursLeft, 0, 'f', 1)
				 .arg(freeGb, 0, 'f', 1)
				 .arg(mbPerMin, 0, 'f', 0);

	if (hoursLeft < 1.0)
		SetRowResult(CheckDisk, SevFail, detail);
	else if (hoursLeft < 4.0)
		SetRowResult(CheckDisk, SevWarn, detail);
	else
		SetRowResult(CheckDisk, SevPass, detail);
}

/* ------------------------------------------------------------------------- */
/* file tail scan                                                             */

void RecordingDiagnosticsDialog::RunFileTailCheckAsync()
{
	BasicOutputHandler *handler = main->GetOutputHandler();
	obs_output_t *out = handler ? (obs_output_t *)handler->fileOutput : nullptr;
	if (!out) {
		SetRowResult(CheckFile, SevInfo, QTStr("RecordingDiagnostics.Skipped.NotRecording"));
		return;
	}

	/* path of the file currently being written */
	OBSDataAutoRelease settings = obs_output_get_settings(out);
	QString path = QT_UTF8(obs_data_get_string(settings, "path"));
	if (path.isEmpty())
		path = QT_UTF8(obs_data_get_string(settings, "url"));

	if (path.isEmpty()) {
		SetRowResult(CheckFile, SevInfo, QTStr("RecordingDiagnostics.File.NoPath"));
		return;
	}

	struct obs_video_info ovi;
	double nominalFps = 60.0;
	if (obs_get_video_info(&ovi) && ovi.fps_den > 0)
		nominalFps = (double)ovi.fps_num / (double)ovi.fps_den;

	QPointer<RecordingDiagnosticsDialog> guard(this);

	std::thread([guard, path, nominalFps]() {
		int severity = SevInfo;
		QString text;

		auto report = [&](int sev, const QString &t) {
			severity = sev;
			text = t;
		};

		std::string utf8 = path.toStdString();
		AVFormatContext *fmt = nullptr;

		if (avformat_open_input(&fmt, utf8.c_str(), nullptr, nullptr) < 0) {
			report(SevInfo, QTStr("RecordingDiagnostics.File.Unreadable"));
		} else {
			if (avformat_find_stream_info(fmt, nullptr) < 0) {
				report(SevInfo, QTStr("RecordingDiagnostics.File.Unreadable"));
			} else {
				int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
				if (vIdx < 0) {
					report(SevInfo, QTStr("RecordingDiagnostics.File.Unreadable"));
				} else {
					AVStream *stream = fmt->streams[vIdx];
					double tb = av_q2d(stream->time_base);

					/* Seek near EOF by byte offset. Duration
					 * metadata on a file still being written is
					 * unreliable, and scanning from byte 0 would
					 * blow the time budget on a multi-GB file. */
					int64_t size = avio_size(fmt->pb);
					bool seeked = false;
					if (size > kTailBytes) {
						if (avio_seek(fmt->pb, size - kTailBytes, SEEK_SET) >= 0)
							seeked = true;
					}

					std::vector<uint32_t> perSecond;
					int64_t firstTs = AV_NOPTS_VALUE;
					int packets = 0;

					AVPacket *pkt = av_packet_alloc();
					while (packets < kTailMaxPackets && av_read_frame(fmt, pkt) >= 0) {
						if (pkt->stream_index == vIdx) {
							int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts
												: pkt->dts;
							if (ts != AV_NOPTS_VALUE) {
								if (firstTs == AV_NOPTS_VALUE)
									firstTs = ts;
								double sec = (double)(ts - firstTs) * tb;
								if (sec >= 0.0 && sec < 100000.0) {
									size_t idx = (size_t)sec;
									if (perSecond.size() <= idx)
										perSecond.resize(idx + 1, 0);
									perSecond[idx]++;
								}
							}
							packets++;
						}
						av_packet_unref(pkt);
					}
					av_packet_free(&pkt);

					/* drop the partial first/last buckets */
					if (perSecond.size() >= 4) {
						size_t first = 1;
						size_t last = perSecond.size() - 1;
						uint32_t threshold = (uint32_t)(nominalFps * kTailLowFpsFactor);

						int badSeconds = 0;
						int worstRun = 0;
						int run = 0;
						uint64_t sum = 0;
						int counted = 0;

						for (size_t i = first; i < last; i++) {
							sum += perSecond[i];
							counted++;
							if (perSecond[i] < threshold) {
								badSeconds++;
								run++;
								worstRun = std::max(worstRun, run);
							} else {
								run = 0;
							}
						}

						double avgFps = counted > 0 ? (double)sum / (double)counted : 0.0;

						QString scope = seeked ? QTStr("RecordingDiagnostics.File.ScopeTail")
								       : QTStr("RecordingDiagnostics.File.ScopeAll");
						QString detail = QTStr("RecordingDiagnostics.File.Result")
									 .arg(counted)
									 .arg(avgFps, 0, 'f', 1)
									 .arg(badSeconds)
									 .arg(scope);

						if (badSeconds > 0 && worstRun >= 3)
							report(SevFail, detail);
						else if (badSeconds > 0)
							report(SevWarn, detail);
						else
							report(SevPass, detail);
					} else {
						report(SevInfo, QTStr("RecordingDiagnostics.File.TooShort"));
					}
				}
			}
			avformat_close_input(&fmt);
		}

		QMetaObject::invokeMethod(
			qApp,
			[guard, severity, text]() {
				if (guard)
					guard->SetRowResult(CheckFile, severity, text);
			},
			Qt::QueuedConnection);
	}).detach();
}

/* ------------------------------------------------------------------------- */
/* raw taps                                                                   */

void RecordingDiagnosticsDialog::AttachTaps()
{
	if (!videoTapActive && sampledVideo) {
		const struct video_output_info *info = video_output_get_info(sampledVideo);
		uint32_t fps = 60;
		if (info && info->fps_den > 0)
			fps = info->fps_num / info->fps_den;
		uint32_t divisor = std::max<uint32_t>(1, fps / (uint32_t)kFreezeFps);

		struct video_scale_info conv = {};
		conv.format = VIDEO_FORMAT_Y800;
		conv.width = kFreezeW;
		conv.height = kFreezeH;
		conv.range = VIDEO_RANGE_DEFAULT;
		conv.colorspace = VIDEO_CS_DEFAULT;

		obs_add_raw_video_callback_mix(sampledVideo, &conv, divisor,
					       &RecordingDiagnosticsDialog::RawVideoFrame, this);
		videoTapActive = true;
	}

	if (!audioTapActive) {
		obs_add_raw_audio_callback(0, nullptr, &RecordingDiagnosticsDialog::RawAudioFrame, this);
		audioTapActive = true;
	}
}

void RecordingDiagnosticsDialog::DetachTaps()
{
	if (videoTapActive && sampledVideo) {
		obs_remove_raw_video_callback_mix(sampledVideo, &RecordingDiagnosticsDialog::RawVideoFrame, this);
		videoTapActive = false;
	}
	if (audioTapActive) {
		obs_remove_raw_audio_callback(0, &RecordingDiagnosticsDialog::RawAudioFrame, this);
		audioTapActive = false;
	}
}

void RecordingDiagnosticsDialog::RawVideoFrame(void *param, struct video_data *frame)
{
	auto *self = static_cast<RecordingDiagnosticsDialog *>(param);
	if (!frame || !frame->data[0])
		return;
	self->HandleVideoSample(frame->data[0], frame->linesize[0], kFreezeW, kFreezeH);
}

void RecordingDiagnosticsDialog::HandleVideoSample(const uint8_t *data, uint32_t linesize, uint32_t w, uint32_t h)
{
	/* Runs on the video thread. Only touches prevFrame (not shared with the
	 * UI thread while sampling is live) and atomics. */
	std::vector<uint8_t> sample(w * h);
	for (uint32_t y = 0; y < h; y++)
		memcpy(sample.data() + y * w, data + (size_t)y * linesize, w);

	if (prevFrame.size() == sample.size()) {
		uint64_t sum = 0;
		for (size_t i = 0; i < sample.size(); i++)
			sum += (uint64_t)std::abs((int)sample[i] - (int)prevFrame[i]);
		float meanDiff = (float)sum / (float)sample.size();

		frameSamples.fetch_add(1);
		if (meanDiff >= kFreezeDiffThreshold)
			changedSamples.fetch_add(1);

		int milli = (int)(meanDiff * 1000.0f);
		int prev = maxDiff.load();
		while (milli > prev && !maxDiff.compare_exchange_weak(prev, milli)) {
		}
	}

	prevFrame = std::move(sample);
}

void RecordingDiagnosticsDialog::RawAudioFrame(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	auto *self = static_cast<RecordingDiagnosticsDialog *>(param);
	if (!data || !data->data[0] || !data->frames)
		return;

	const float *samples = (const float *)data->data[0];
	float peak = 0.0f;
	for (uint32_t i = 0; i < data->frames; i++) {
		float v = std::fabs(samples[i]);
		if (v > peak)
			peak = v;
	}

	self->audioBlocks.fetch_add(1);
	if (peak > kAudioSilenceFloor)
		self->audioActiveBlocks.fetch_add(1);

	int milli = (int)(peak * 1000.0f);
	int prev = self->audioPeakMilli.load();
	while (milli > prev && !self->audioPeakMilli.compare_exchange_weak(prev, milli)) {
	}
}

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
#include <libavutil/error.h>
}

#include "moc_RecordingDiagnosticsDialog.cpp"

/* ------------------------------------------------------------------------- */
/* tuning                                                                     */

/* Live sampling window. Long enough that a picture which is merely slow to
 * change (a menu, a mostly-static scene) still has a chance to show motion -
 * a 3 s window turned out to be short enough that ordinary gameplay pauses
 * read as "frozen" - while still finishing quickly. */
static constexpr int kSampleTickMs = 100;
static constexpr int kSampleTicks = 60; /* 6.0 s */

static constexpr uint32_t kFreezeW = 160;
static constexpr uint32_t kFreezeH = 90;
static constexpr int kFreezeFps = 4;
static constexpr float kFreezeDiffThreshold = 1.5f;

static constexpr float kAudioSilenceFloor = 0.0025f; /* ~ -52 dBFS */

/* A static picture only counts against the recording when the encode
 * pipeline is also struggling; on its own it is far more likely to be
 * genuinely static content. Same 2% corroboration threshold the live freeze
 * watchdog uses. */
static constexpr double kFreezeCorroborationSkipRatio = 0.02;

static constexpr int64_t kTailBytes = 192LL * 1024 * 1024;
static constexpr int kTailMaxPackets = 60000;
static constexpr double kTailLowFpsFactor = 0.70;

/* Mirrors MBYTES_LEFT_STOP_REC in OBSBasic_Recording.cpp - the only
 * disk-space threshold that actually stops a recording (one definition, one
 * call site in LowDiskSpace()). Deliberately NOT the Stats dialog's 1 GB /
 * 5 GB values, which only colour text and stop nothing. */
static constexpr uint64_t kStopRecordingFreeBytes = 50ULL * 1024ULL * 1024ULL;

enum { SevPass = 0, SevWarn = 1, SevFail = 2, SevInfo = 3 };

static const char *kSpinnerFrames[] = {"|", "/", "-", "\\"};

/* ------------------------------------------------------------------------- */

RecordingDiagnosticsDialog::RecordingDiagnosticsDialog(OBSBasic *main_, QWidget *parent)
	: QDialog(parent),
	  main(main_)
{
	setWindowTitle(QTStr("RecordingDiagnostics.Title"));
	setMinimumWidth(900);

	cpuInfo = os_cpu_usage_info_start();

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

	QTimer::singleShot(0, this, &RecordingDiagnosticsDialog::StartRun);
}

RecordingDiagnosticsDialog::~RecordingDiagnosticsDialog()
{
	DetachTaps();
	if (cpuInfo) {
		os_cpu_usage_info_destroy(cpuInfo);
		cpuInfo = nullptr;
	}
}

void RecordingDiagnosticsDialog::BuildUi()
{
	auto *root = new QVBoxLayout(this);

	auto *intro = new QLabel(QTStr("RecordingDiagnostics.Intro"), this);
	intro->setWordWrap(true);
	setClasses(intro, "text-muted");
	root->addWidget(intro);

	grid = new QGridLayout();
	grid->setColumnStretch(0, 0);
	grid->setColumnStretch(1, 0);
	grid->setColumnStretch(2, 0);
	grid->setColumnStretch(3, 1); /* result text gets the slack */
	grid->setHorizontalSpacing(10);
	root->addLayout(grid);

	static const char *titleKeys[CheckCount] = {
		"RecordingDiagnostics.Check.Output",       "RecordingDiagnostics.Check.Encoder",
		"RecordingDiagnostics.Check.RenderLag",    "RecordingDiagnostics.Check.Fps",
		"RecordingDiagnostics.Check.Freeze",       "RecordingDiagnostics.Check.Audio",
		"RecordingDiagnostics.Check.AudioRouting", "RecordingDiagnostics.Check.FileGrowing",
		"RecordingDiagnostics.Check.File",         "RecordingDiagnostics.Check.Cpu",
		"RecordingDiagnostics.Check.Disk",
	};

	checks.resize(CheckCount);
	for (int i = 0; i < CheckCount; i++) {
		DiagCheck &c = checks[i];
		c.title = QTStr(titleKeys[i]);

		c.statusLabel = new QLabel(QStringLiteral(" "), this);
		c.statusLabel->setFixedWidth(18);
		c.statusLabel->setAlignment(Qt::AlignCenter | Qt::AlignTop);

		c.titleLabel = new QLabel(c.title, this);
		c.titleLabel->setMinimumWidth(160);
		c.titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);

		c.progress = new QProgressBar(this);
		c.progress->setRange(0, 100);
		c.progress->setValue(0);
		c.progress->setTextVisible(true);
		c.progress->setFixedHeight(14);
		c.progress->setFixedWidth(150);

		/* Result text is the part that actually varies in length, so it
		 * wraps and is allowed to grow the row rather than being
		 * clipped. */
		c.resultLabel = new QLabel(QStringLiteral("—"), this);
		c.resultLabel->setWordWrap(true);
		c.resultLabel->setMinimumWidth(380);
		c.resultLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		c.resultLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
		c.resultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
		setClasses(c.resultLabel, "text-muted");

		grid->addWidget(c.statusLabel, i, 0);
		grid->addWidget(c.titleLabel, i, 1);
		grid->addWidget(c.progress, i, 2, Qt::AlignTop);
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
	if (c.done)
		return;

	c.running = false;
	c.done = true;
	c.progress->setValue(100);
	c.resultLabel->setText(text);

	switch (severity) {
	case SevPass:
		c.statusLabel->setText(QStringLiteral("✓"));
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
		c.statusLabel->setText(QStringLiteral("✕"));
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

void RecordingDiagnosticsDialog::StartRun()
{
	DetachTaps();

	failCount = 0;
	warnCount = 0;
	completedCount = 0;
	sampleTicks = 0;
	frameSamples = 0;
	changedSamples = 0;
	audioBlocks = 0;
	audioActiveBlocks = 0;
	audioPeakMilli = 0;
	measuredSkipRatio = 0.0;
	haveSkipRatio = false;
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

void RecordingDiagnosticsDialog::SkipRemainingLiveChecks()
{
	for (int id : {CheckEncoder, CheckRenderLag, CheckFps, CheckFreeze, CheckAudio, CheckAudioRouting,
		       CheckFileGrowing, CheckFile}) {
		if (!checks[id].done) {
			SetRowRunning(id);
			SetRowResult(id, SevInfo, QTStr("RecordingDiagnostics.Skipped.NotRecording"));
		}
	}
}

/* ------------------------------------------------------------------------- */
/* checks                                                                     */

void RecordingDiagnosticsDialog::RunInstantChecks()
{
	BasicOutputHandler *handler = main->GetOutputHandler();
	obs_output_t *out = handler ? (obs_output_t *)handler->fileOutput : nullptr;
	bool recording = handler && handler->RecordingActive();

	SetRowRunning(CheckOutput);
	if (recording && out) {
		if (obs_output_paused(out))
			SetRowResult(CheckOutput, SevWarn, QTStr("RecordingDiagnostics.Output.Paused"));
		else
			SetRowResult(CheckOutput, SevPass, QTStr("RecordingDiagnostics.Output.Active"));
	} else {
		SetRowResult(CheckOutput, SevWarn, QTStr("RecordingDiagnostics.Output.Inactive"));
		SkipRemainingLiveChecks();
		RunCpuCheck();
		RunDiskCheck();
		return;
	}

	sampledVideo = obs_output_video(out);
	if (!sampledVideo)
		sampledVideo = obs_get_video();

	startSkipped = sampledVideo ? video_output_get_skipped_frames(sampledVideo) : 0;
	startTotal = sampledVideo ? video_output_get_total_frames(sampledVideo) : 0;
	startLagged = obs_get_lagged_frames();
	startRendered = obs_get_total_frames();
	startBytes = obs_output_get_total_bytes(out);

	SetRowRunning(CheckEncoder);
	SetRowRunning(CheckRenderLag);
	SetRowRunning(CheckFps);
	SetRowRunning(CheckFreeze);
	SetRowRunning(CheckAudio);
	SetRowRunning(CheckFileGrowing);

	AttachTaps();
	BeginSampling();

	/* independent of the sampling window */
	RunAudioRoutingCheck();
	RunCpuCheck();

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

	for (int id : {CheckEncoder, CheckRenderLag, CheckFps, CheckFreeze, CheckAudio, CheckFileGrowing})
		SetRowProgress(id, pct);

	if (sampleTicks >= kSampleTicks) {
		sampleTimer->stop();
		FinishSamplingChecks();
	}
}

void RecordingDiagnosticsDialog::FinishSamplingChecks()
{
	DetachTaps();

	BasicOutputHandler *handler = main->GetOutputHandler();
	obs_output_t *out = handler ? (obs_output_t *)handler->fileOutput : nullptr;

	/* --- encoder skipped frames --- */
	uint32_t skipped = sampledVideo ? video_output_get_skipped_frames(sampledVideo) : 0;
	uint32_t total = sampledVideo ? video_output_get_total_frames(sampledVideo) : 0;
	uint32_t dSkipped = skipped - startSkipped;
	uint32_t dTotal = total - startTotal;

	if (dTotal == 0) {
		SetRowResult(CheckEncoder, SevWarn, QTStr("RecordingDiagnostics.Encoder.NoFrames"));
	} else {
		measuredSkipRatio = (double)dSkipped / (double)dTotal;
		haveSkipRatio = true;
		int pct = (int)std::lround(measuredSkipRatio * 100.0);
		QString detail = QTStr("RecordingDiagnostics.Encoder.Result").arg(pct).arg(dSkipped).arg(dTotal);
		if (measuredSkipRatio > 0.10)
			SetRowResult(CheckEncoder, SevFail, detail);
		else if (measuredSkipRatio > 0.02)
			SetRowResult(CheckEncoder, SevWarn, detail);
		else
			SetRowResult(CheckEncoder, SevPass, detail);
	}

	/* --- render lag (compositor, distinct from encoder) --- */
	uint32_t dLagged = obs_get_lagged_frames() - startLagged;
	uint32_t dRendered = obs_get_total_frames() - startRendered;
	if (dRendered == 0) {
		SetRowResult(CheckRenderLag, SevWarn, QTStr("RecordingDiagnostics.RenderLag.NoFrames"));
	} else {
		double lagRatio = (double)dLagged / (double)dRendered;
		int pct = (int)std::lround(lagRatio * 100.0);
		QString detail = QTStr("RecordingDiagnostics.RenderLag.Result").arg(pct).arg(dLagged).arg(dRendered);
		if (lagRatio > 0.05)
			SetRowResult(CheckRenderLag, SevFail, detail);
		else if (lagRatio > 0.01)
			SetRowResult(CheckRenderLag, SevWarn, detail);
		else
			SetRowResult(CheckRenderLag, SevPass, detail);
	}

	/* --- actual vs configured frame rate --- */
	{
		struct obs_video_info ovi;
		double target = 0.0;
		if (obs_get_video_info(&ovi) && ovi.fps_den > 0)
			target = (double)ovi.fps_num / (double)ovi.fps_den;
		double actual = obs_get_active_fps();

		if (target <= 0.0) {
			SetRowResult(CheckFps, SevInfo, QTStr("RecordingDiagnostics.Fps.Unknown"));
		} else {
			double ratio = actual / target;
			QString detail = QTStr("RecordingDiagnostics.Fps.Result")
						 .arg(actual, 0, 'f', 1)
						 .arg(target, 0, 'f', 0);
			if (ratio < 0.90)
				SetRowResult(CheckFps, SevFail, detail);
			else if (ratio < 0.98)
				SetRowResult(CheckFps, SevWarn, detail);
			else
				SetRowResult(CheckFps, SevPass, detail);
		}
	}

	/* --- picture freeze, corroborated against the encoder --- */
	int samples = frameSamples.load();
	int changed = changedSamples.load();
	if (samples < 2) {
		SetRowResult(CheckFreeze, SevWarn, QTStr("RecordingDiagnostics.Freeze.NoSamples"));
	} else if (changed > 0) {
		SetRowResult(CheckFreeze, SevPass, QTStr("RecordingDiagnostics.Freeze.Moving").arg(changed).arg(samples));
	} else if (haveSkipRatio && measuredSkipRatio > kFreezeCorroborationSkipRatio) {
		/* static AND the encoder is struggling: this is the real
		 * failure signature, not merely quiet content */
		SetRowResult(CheckFreeze, SevFail, QTStr("RecordingDiagnostics.Freeze.Stuck").arg(samples));
	} else {
		/* Static with a healthy encoder is overwhelmingly just static
		 * content (paused game, menu, idle desktop). Reported as
		 * information, not a problem, so it cannot cry wolf. */
		SetRowResult(CheckFreeze, SevInfo, QTStr("RecordingDiagnostics.Freeze.Static").arg(samples));
	}

	/* --- audio --- */
	int blocks = audioBlocks.load();
	int activeBlocks = audioActiveBlocks.load();
	double peak = (double)audioPeakMilli.load() / 1000.0;
	double peakDb = peak > 0.0 ? 20.0 * std::log10(peak) : -100.0;

	if (blocks == 0)
		SetRowResult(CheckAudio, SevFail, QTStr("RecordingDiagnostics.Audio.None"));
	else if (activeBlocks == 0)
		SetRowResult(CheckAudio, SevWarn, QTStr("RecordingDiagnostics.Audio.Silent"));
	else
		SetRowResult(CheckAudio, SevPass, QTStr("RecordingDiagnostics.Audio.Ok").arg(peakDb, 0, 'f', 1));

	/* --- is the file actually growing --- */
	if (out) {
		uint64_t nowBytes = obs_output_get_total_bytes(out);
		uint64_t written = nowBytes > startBytes ? nowBytes - startBytes : 0;
		double windowSec = (double)(kSampleTicks * kSampleTickMs) / 1000.0;
		double mbPerMin = ((double)written / windowSec) * 60.0 / (1024.0 * 1024.0);

		if (written == 0)
			SetRowResult(CheckFileGrowing, SevFail, QTStr("RecordingDiagnostics.FileGrowing.Stalled"));
		else
			SetRowResult(CheckFileGrowing, SevPass,
				     QTStr("RecordingDiagnostics.FileGrowing.Ok")
					     .arg((double)written / (1024.0 * 1024.0), 0, 'f', 1)
					     .arg(mbPerMin, 0, 'f', 0));
	}

	RunDiskCheck();
}

void RecordingDiagnosticsDialog::RunAudioRoutingCheck()
{
	SetRowRunning(CheckAudioRouting);

	BasicOutputHandler *handler = main->GetOutputHandler();
	obs_output_t *out = handler ? (obs_output_t *)handler->fileOutput : nullptr;
	if (!out) {
		SetRowResult(CheckAudioRouting, SevInfo, QTStr("RecordingDiagnostics.Skipped.NotRecording"));
		return;
	}

	/* Which mixer tracks the recording is actually encoding. Read from the
	 * live output rather than reconstructed from config so it stays correct
	 * across Simple/Advanced modes and every container. */
	uint32_t recMask = 0;
	bool haveEncoders = false;
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		obs_encoder_t *enc = obs_output_get_audio_encoder(out, i);
		if (enc) {
			haveEncoders = true;
			recMask |= (1u << obs_encoder_get_mixer_index(enc));
		}
	}

	if (!haveEncoders) {
		/* FFmpeg/lossless output captures everything via
		 * obs_output_set_media and has no per-track encoders */
		SetRowResult(CheckAudioRouting, SevInfo, QTStr("RecordingDiagnostics.AudioRouting.NA"));
		return;
	}

	struct Ctx {
		uint32_t recMask;
		QStringList excluded;
	} ctx{recMask, {}};

	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			auto *c = static_cast<Ctx *>(param);
			if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;
			if (!obs_source_audio_active(source) || obs_source_muted(source))
				return true;
			/* deliberately stream-only or subtracted sources are not
			 * mistakes - same carve-outs the start-of-recording
			 * warning uses */
			if (obs_source_get_output_filter(source) == OBS_SOURCE_OUTPUT_FILTER_STREAM)
				return true;
			OBSDataAutoRelease priv = obs_source_get_private_settings(source);
			if (obs_data_get_bool(priv, "audio_subtract"))
				return true;

			if ((obs_source_get_audio_mixers(source) & c->recMask) == 0)
				c->excluded.append(QT_UTF8(obs_source_get_name(source)));
			return true;
		},
		&ctx);

	if (ctx.excluded.isEmpty())
		SetRowResult(CheckAudioRouting, SevPass, QTStr("RecordingDiagnostics.AudioRouting.Ok"));
	else
		SetRowResult(CheckAudioRouting, SevFail,
			     QTStr("RecordingDiagnostics.AudioRouting.Excluded")
				     .arg(ctx.excluded.join(QStringLiteral(", "))));
}

void RecordingDiagnosticsDialog::RunCpuCheck()
{
	SetRowRunning(CheckCpu);

	if (!cpuInfo) {
		SetRowResult(CheckCpu, SevInfo, QTStr("RecordingDiagnostics.Cpu.Unknown"));
		return;
	}

	double usage = os_cpu_usage_info_query(cpuInfo);
	QString detail = QTStr("RecordingDiagnostics.Cpu.Result").arg(usage, 0, 'f', 1);

	if (usage > 85.0)
		SetRowResult(CheckCpu, SevFail, detail);
	else if (usage > 60.0)
		SetRowResult(CheckCpu, SevWarn, detail);
	else
		SetRowResult(CheckCpu, SevPass, detail);
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

	/* Space usable before OBS force-stops, not raw free space. */
	uint64_t usable = freeBytes > kStopRecordingFreeBytes ? freeBytes - kStopRecordingFreeBytes : 0;

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
		SetRowResult(CheckDisk, SevInfo, QTStr("RecordingDiagnostics.Disk.SpaceOnly").arg(freeGb, 0, 'f', 1));
		return;
	}

	double hoursLeft = ((double)usable / bytesPerSec) / 3600.0;
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
		std::string utf8 = path.toStdString();

		blog(LOG_INFO, "[diagnostics] tail scan opening '%s'", utf8.c_str());

		AVFormatContext *fmt = nullptr;
		/* Tell the demuxer this is a growing file so it doesn't trust a
		 * stale/absent duration, and keep the probe cheap. */
		AVDictionary *opts = nullptr;
		av_dict_set(&opts, "fflags", "+nobuffer", 0);

		int rc = avformat_open_input(&fmt, utf8.c_str(), nullptr, &opts);
		av_dict_free(&opts);

		if (rc < 0) {
			char err[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_strerror(rc, err, sizeof(err));
			blog(LOG_WARNING, "[diagnostics] tail scan: avformat_open_input failed: %s", err);
			severity = SevInfo;
			text = QTStr("RecordingDiagnostics.File.OpenFailed").arg(QT_UTF8(err));
		} else {
			rc = avformat_find_stream_info(fmt, nullptr);
			int vIdx = rc >= 0 ? av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;

			if (vIdx < 0) {
				char err[AV_ERROR_MAX_STRING_SIZE] = {0};
				av_strerror(rc < 0 ? rc : vIdx, err, sizeof(err));
				blog(LOG_WARNING, "[diagnostics] tail scan: no video stream (%s)", err);
				severity = SevInfo;
				text = QTStr("RecordingDiagnostics.File.NoStream").arg(QT_UTF8(err));
			} else {
				AVStream *stream = fmt->streams[vIdx];
				double tb = av_q2d(stream->time_base);

				/* Seek near EOF by byte offset: duration metadata
				 * on a file still being written is unreliable, and
				 * scanning a multi-GB file from byte 0 would blow
				 * the time budget. */
				int64_t size = avio_size(fmt->pb);
				bool seeked = false;
				if (size > kTailBytes) {
					int64_t target = size - kTailBytes;
					if (avio_seek(fmt->pb, target, SEEK_SET) >= 0)
						seeked = true;
					else
						blog(LOG_INFO, "[diagnostics] tail seek failed, scanning from start");
				}

				std::vector<uint32_t> perSecond;
				int64_t firstTs = AV_NOPTS_VALUE;
				int packets = 0;

				AVPacket *pkt = av_packet_alloc();
				while (packets < kTailMaxPackets && av_read_frame(fmt, pkt) >= 0) {
					if (pkt->stream_index == vIdx) {
						int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
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

				blog(LOG_INFO, "[diagnostics] tail scan read %d video packets across %zu buckets",
				     packets, perSecond.size());

				if (perSecond.size() >= 4) {
					size_t first = 1;
					size_t last = perSecond.size() - 1;
					uint32_t threshold = (uint32_t)(nominalFps * kTailLowFpsFactor);

					int badSeconds = 0, worstRun = 0, run = 0, counted = 0;
					uint64_t sum = 0;

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
						severity = SevFail;
					else if (badSeconds > 0)
						severity = SevWarn;
					else
						severity = SevPass;
					text = detail;
				} else {
					severity = SevInfo;
					text = QTStr("RecordingDiagnostics.File.TooShort");
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

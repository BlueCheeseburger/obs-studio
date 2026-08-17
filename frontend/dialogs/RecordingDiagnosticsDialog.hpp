/******************************************************************************
    Recording Diagnostics.

    An on-demand health check that can be run WHILE a recording is in
    progress, rather than waiting for the passive watchdog to trip a
    threshold or for the post-recording file probe to run once the file is
    already finished.

    Motivation: a 19-hour unattended recording degraded into ~93% skipped
    frames and a frozen picture. The live watchdog did detect it and fire
    notifications, but nobody was at the machine to see them, and there was
    no way to ask "is this recording OK right now?" on demand.

    Each check runs independently with its own progress, so the ones backed
    by instantly-available state resolve immediately while the ones that
    need real sampling report genuine progress as they work.

    A deliberate design rule throughout: a signal that is ambiguous on its
    own is corroborated against another before it is allowed to fail. Most
    importantly, a static picture is NOT a fault by itself - a paused game,
    a menu, or an idle desktop looks exactly like a dead capture - so it is
    only escalated when the encoder is also in trouble. This mirrors the
    corroboration logic the live freeze watchdog already uses, and exists
    to avoid the false positives that made an earlier version of that
    watchdog untrustworthy.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <util/platform.h>

#include <QDialog>
#include <QPointer>

#include <atomic>
#include <vector>

class OBSBasic;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class QGridLayout;

/* One row of the checklist. */
struct DiagCheck {
	QString title;
	QLabel *titleLabel = nullptr;
	QLabel *statusLabel = nullptr; /* spinner glyph, then a verdict mark */
	QProgressBar *progress = nullptr;
	QLabel *resultLabel = nullptr;

	bool running = false;
	bool done = false;
	int spinnerFrame = 0;
};

class RecordingDiagnosticsDialog : public QDialog {
	Q_OBJECT

public:
	/* notifyOnly: run the whole checklist without ever showing the window,
	 * then deliver the verdict as a desktop notification and self-destruct.
	 * Used by the "run diagnostics" hotkey so the check can be triggered
	 * from inside a fullscreen game without switching away from it. */
	explicit RecordingDiagnosticsDialog(OBSBasic *main, QWidget *parent = nullptr, bool notifyOnly = false);
	~RecordingDiagnosticsDialog() override;

private:
	enum CheckId {
		CheckOutput = 0,
		CheckEncoder,
		CheckRenderLag,
		CheckFps,
		CheckFreeze,
		CheckAudio,
		CheckAudioRouting,
		CheckFileGrowing,
		CheckFile,
		CheckCpu,
		CheckDisk,
		CheckCount,
	};

	OBSBasic *main;

	std::vector<DiagCheck> checks;
	QGridLayout *grid = nullptr;
	QLabel *summaryLabel = nullptr;
	QPushButton *runButton = nullptr;
	QPushButton *closeButton = nullptr;

	QPointer<QTimer> spinnerTimer;
	QPointer<QTimer> sampleTimer;

	int failCount = 0;
	int warnCount = 0;
	int completedCount = 0;

	/* notification mode */
	bool notifyOnly = false;
	QStringList problemLines; /* "<check> — <result>" for each warn/fail */
	QString diskLine;         /* always kept, healthy or not */

	void SendNotification();

	/* ---- sampling state ---- */
	video_t *sampledVideo = nullptr;

	uint32_t startSkipped = 0;
	uint32_t startTotal = 0;
	uint32_t startLagged = 0;
	uint32_t startRendered = 0;
	uint64_t startBytes = 0;

	/* Encoder skip ratio measured this run; the freeze verdict is
	 * corroborated against it rather than standing on its own. */
	double measuredSkipRatio = 0.0;
	bool haveSkipRatio = false;

	os_cpu_usage_info_t *cpuInfo = nullptr;

	/* freeze detection */
	bool videoTapActive = false;
	std::vector<uint8_t> prevFrame;
	std::atomic<int> frameSamples{0};
	std::atomic<int> changedSamples{0};

	/* audio activity */
	bool audioTapActive = false;
	std::atomic<int> audioBlocks{0};
	std::atomic<int> audioActiveBlocks{0};
	std::atomic<int> audioPeakMilli{0};

	int sampleTicks = 0;

	void BuildUi();
	void StartRun();
	void FinishRun();

	void SetRowRunning(int id);
	void SetRowResult(int id, int severity, const QString &text);
	void SetRowProgress(int id, int pct);
	void SkipRemainingLiveChecks();

	void RunInstantChecks();
	void BeginSampling();
	void OnSampleTick();
	void FinishSamplingChecks();
	void RunAudioRoutingCheck();
	void RunDiskCheck();
	void RunCpuCheck();
	void RunFileTailCheckAsync();

	void AttachTaps();
	void DetachTaps();

	static void RawVideoFrame(void *param, struct video_data *frame);
	static void RawAudioFrame(void *param, size_t mix_idx, struct audio_data *data);

	void HandleVideoSample(const uint8_t *data, uint32_t linesize, uint32_t w, uint32_t h);
};

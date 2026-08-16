/******************************************************************************
    Recording Diagnostics.

    An on-demand health check that can be run WHILE a recording is in
    progress, rather than waiting for the passive watchdog to trip a
    threshold or for the post-recording file probe to run once the file is
    already finished.

    Motivation: a 19-hour unattended recording degraded into ~93% skipped
    frames and a frozen picture. The live watchdog did detect it and fire
    notifications, but nobody was at the machine to see them, and there was
    no way to ask "is this recording OK right now?" on demand. This dialog
    is that question.

    Each check runs independently with its own progress, so the ones backed
    by instantly-available state resolve immediately while the two that need
    real sampling (picture freeze, recent file integrity) report genuine
    progress as they work. The whole run is bounded to a few seconds.

    Checks:
      1. Output active      - is a recording actually running right now
      2. Encoder skip       - skipped/total frames sampled over a short
                              window, i.e. the encoder failing to keep up
      3. Picture freeze     - downscaled frames sampled from the record mix
                              and diffed, i.e. the picture silently stuck
      4. Audio flowing      - raw mixed audio sampled for real signal
      5. Recent file        - demuxes only the TAIL of the in-progress file
                              (seeking near EOF rather than scanning from
                              byte 0, which would defeat the "few seconds"
                              budget on a multi-GB file) and looks for PTS
                              gaps / collapsed-content stretches
      6. Disk space         - free space minus the point at which OBS force
                              stops recording, divided by this session's
                              measured write rate, reported as remaining
                              recording time
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QDialog>
#include <QPointer>

#include <atomic>
#include <memory>
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
	explicit RecordingDiagnosticsDialog(OBSBasic *main, QWidget *parent = nullptr);
	~RecordingDiagnosticsDialog() override;

private:
	enum CheckId {
		CheckOutput = 0,
		CheckEncoder,
		CheckFreeze,
		CheckAudio,
		CheckFile,
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

	/* verdicts, filled in as checks complete */
	int failCount = 0;
	int warnCount = 0;
	int completedCount = 0;

	/* ---- sampling state for the live checks ---- */
	OBSWeakOutputAutoRelease weakOutput;
	video_t *sampledVideo = nullptr;

	/* encoder skip sampling */
	uint32_t startSkipped = 0;
	uint32_t startTotal = 0;

	/* byte-rate sampling for the disk projection */
	uint64_t startBytes = 0;

	/* freeze detection: raw video tap on the record mix */
	bool videoTapActive = false;
	std::vector<uint8_t> prevFrame;
	std::atomic<int> frameSamples{0};
	std::atomic<int> changedSamples{0};
	std::atomic<int> maxDiff{0};

	/* audio activity: raw audio tap */
	bool audioTapActive = false;
	std::atomic<int> audioBlocks{0};
	std::atomic<int> audioActiveBlocks{0};
	std::atomic<int> audioPeakMilli{0}; /* peak level * 1000, ints for atomics */

	/* elapsed ticks of the sampling window */
	int sampleTicks = 0;

	void BuildUi();
	void StartRun();
	void FinishRun();

	void SetRowRunning(int id);
	void SetRowResult(int id, int severity, const QString &text);
	void SetRowProgress(int id, int pct);

	/* individual checks */
	void RunInstantChecks();
	void BeginSampling();
	void OnSampleTick();
	void FinishSamplingChecks();
	void RunDiskCheck();
	void RunFileTailCheckAsync();

	void AttachTaps();
	void DetachTaps();

	static void RawVideoFrame(void *param, struct video_data *frame);
	static void RawAudioFrame(void *param, size_t mix_idx, struct audio_data *data);

	void HandleVideoSample(const uint8_t *data, uint32_t linesize, uint32_t w, uint32_t h);
};

/******************************************************************************
    Recording health monitor.

    Two independent detectors, both alerting through healthAlert():

    1. Live watchdog — while recording, polls the video output's
       skipped-frame counters every 2 s. Sustained skipping (encoder
       overloaded) raises an alert within seconds, while the recording is
       still running.

    2. File probe — after a recording (or split-file segment) finishes, a
       background thread demuxes the file and reads video packet timestamps
       (no decoding), bucketing frames per second. Stretches where the
       effective framerate fell well below nominal raise an alert. This is
       ground truth for what actually landed on disk.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QObject>
#include <QPointer>

class QTimer;

class RecordingHealthMonitor : public QObject {
	Q_OBJECT

public:
	explicit RecordingHealthMonitor(QObject *parent = nullptr);

	void recordingStarted(obs_output_t *output);
	void recordingStopped();

	/* Probe a finished file (final recording or completed split segment)
	 * on a background thread. nominalFps <= 0 uses the current video
	 * settings. */
	void checkFileAsync(const QString &path, double nominalFps = 0.0);

signals:
	/* message is ready for direct display in a desktop notification */
	void healthAlert(const QString &message);

private:
	QPointer<QTimer> pollTimer;
	OBSWeakOutputAutoRelease weakOutput;

	uint32_t lastSkipped = 0;
	uint32_t lastTotal = 0;
	int badPolls = 0;
	bool liveAlerted = false;

	void poll();
};

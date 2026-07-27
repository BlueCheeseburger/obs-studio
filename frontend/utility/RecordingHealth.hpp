/******************************************************************************
    Recording health monitor.

    Three independent detectors, all alerting through healthAlert():

    1. Live encoder watchdog — while recording, polls the video output's
       skipped-frame counters every 2 s. Sustained skipping (encoder
       overloaded) raises an alert within seconds, while the recording is
       still running.

    2. Live freeze watchdog — while recording, samples a small downscaled
       copy of the actual pre-encode picture (~1/s) and the raw mixed audio
       in real time. If the picture stops visibly changing for a sustained
       stretch (content freeze — the failure mode that produced the
       original "82% skipped frames" recording, which the encoder watchdog
       alone did not catch, since OBS's own render pipeline never fell
       behind), it only alerts when BOTH:
         - audio is showing real level variation (rules out plain silence),
           AND
         - the encoder has shown at least a little skipping somewhere in
           the recording (rules out ordinary inactivity — audio activity
           alone is not a reliable signal that the *picture* should be
           moving: a podcast or music can play for a long time over a
           legitimately static screen, e.g. reading, AFK, a paused menu,
           and that is clean 0% skip the entire time).
       Fires at most once per recording, so it doesn't repeat-alert on
       every subsequent freeze/resume cycle in one long session.

    3. File probe — after a recording (or split-file segment) finishes, a
       background thread demuxes the file and reads video packet timestamps
       (no decoding), bucketing frames per second. Stretches where the
       effective framerate fell well below nominal raise an alert. This is
       ground truth for what actually landed on disk.

       Caveat this guards against: a stretch of near-empty delta packets
       (the "content collapse" signal used to catch frozen/duplicate
       frames) looks identical whether it is caused by an encoder problem
       or by genuinely static footage (a paused game, a menu, an idle
       desktop) — NVENC correctly compresses unchanging content down to
       almost nothing either way. To tell those apart, the file probe is
       corroborated against the live watchdog's own skip ratio for that
       recording: if the encoder never meaningfully skipped frames while
       it was running, a content collapse is logged but not surfaced as
       an alert, since it is very likely just static content rather than
       an actual problem.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QMutex>
#include <QObject>
#include <QPointer>

#include <deque>
#include <vector>

class QTimer;

class RecordingHealthMonitor : public QObject {
	Q_OBJECT

public:
	explicit RecordingHealthMonitor(QObject *parent = nullptr);
	~RecordingHealthMonitor();

	void recordingStarted(obs_output_t *output);
	void recordingStopped();

	/* Probe a finished file (final recording or completed split segment)
	 * on a background thread. nominalFps <= 0 uses the current video
	 * settings. */
	void checkFileAsync(const QString &path, double nominalFps = 0.0);

signals:
	/* Real-time encoder-overload/dropped-frames alert only (the live skip-
	 * ratio watchdog) — the one case worth interrupting the user for, since
	 * it fires while the recording is still salvageable. Triggers a desktop
	 * notification + flashing tray icon. */
	void healthAlert(const QString &message);

	/* Detection/logging only, no notification or tray flash: the freeze
	 * watchdog (picture stuck while audio active) and the post-recording
	 * file probe. Both still run and still log every finding — just without
	 * interrupting the user, since by the time either fires there is
	 * nothing left to act on live (freeze: encoder skip already covers the
	 * actionable "something is wrong now" case; file probe: the recording
	 * this refers to has already finished). */
	void healthAlertSilent(const QString &message);

private:
	QPointer<QTimer> pollTimer;
	OBSWeakOutputAutoRelease weakOutput;

	uint32_t lastSkipped = 0;
	uint32_t lastTotal = 0;
	int badPolls = 0;
	bool liveAlerted = false;

	/* highest 2 s-window skip ratio observed anywhere in the current (or
	 * just-finished) recording; corroborates the file probe's content
	 * collapse check so static footage doesn't get flagged as a fault */
	double maxLiveSkipRatio = 0.0;

	/* --- live freeze watchdog: raw video side (video thread) --- */
	bool rawVideoTapActive = false;
	/* video_t the raw tap is currently attached to (the record mix, not the
	 * main canvas — see recordingStarted()); needed at detach time since
	 * obs_remove_raw_video_callback_mix() must be called with the exact
	 * video_t it was added with. */
	video_t *tappedVideo = nullptr;
	std::vector<uint8_t> prevVideoSample;
	QMutex freezeMutex;
	int frozenSeconds = 0;
	bool freezeAlerted = false;

	/* --- live freeze watchdog: raw audio side (audio thread) --- */
	bool rawAudioTapActive = false;
	QMutex audioMutex;
	float audioBlockAccum = 0.0f;
	uint32_t audioBlockAccumSamples = 0;
	std::deque<float> audioLevelRingDb;

	static void RawVideoFrame(void *param, struct video_data *frame);
	static void RawAudioFrame(void *param, size_t mix_idx, struct audio_data *data);

	void handleVideoSample(const uint8_t *data, uint32_t linesize, uint32_t width, uint32_t height);
	void handleAudioBlock(const float *samples, uint32_t frames);

	void poll();
};

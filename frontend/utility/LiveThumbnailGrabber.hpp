/******************************************************************************
    Auto live-stream thumbnail grabber.

    Periodically grabs a frame from the program output and, if the frame passes
    a set of basic quality filters, hands it off to be uploaded as the live
    broadcast's thumbnail. Bad frames are retried a couple of times before the
    grabber gives up for the current cycle and keeps the previous thumbnail.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QImage>
#include <QObject>
#include <QPointer>

class QTimer;
class ScreenshotObj;

class LiveThumbnailGrabber : public QObject {
	Q_OBJECT

public:
	explicit LiveThumbnailGrabber(QObject *parent = nullptr);
	~LiveThumbnailGrabber() override;

	void start();
	void stop();
	bool isRunning() const { return running; }

	/* Interval between scheduled grab cycles. */
	static constexpr int kCycleIntervalMs = 5 * 60 * 1000;
	/* Delay between retries within a single cycle. */
	static constexpr int kRetryIntervalMs = 15 * 1000;
	/* Total grab attempts allowed per cycle (1 initial + 2 retries). */
	static constexpr int kMaxAttemptsPerCycle = 3;

private slots:
	void beginAttempt();
	void onFrameReady(QImage image);

private:
	void scheduleNextCycle();
	void handleRejected(const char *reason);

	/* Returns false (and fills reason) if it is not currently safe/allowed to
	 * grab a frame: a visual transition is in progress, or the current scene
	 * has been excluded by the user. */
	bool gateAllowsCapture(QString &reason);

	/* Runs the quality filters on a grabbed frame. Returns false (and fills
	 * reason) when the frame is unacceptable. */
	bool frameIsAcceptable(const QImage &image, QString &reason);

	QPointer<QTimer> cycleTimer;
	QPointer<QTimer> retryTimer;
	QPointer<ScreenshotObj> activeShot;

	int attemptsThisCycle = 0;
	bool running = false;
	bool captureInFlight = false;

	/* Small grayscale fingerprint of the most recently analyzed frame, used
	 * to detect a frozen/duplicate stream. */
	QImage lastSignature;

	/* Fingerprint of the frame we last actually uploaded. Used to skip
	 * re-uploading a thumbnail that is visually unchanged, which is the main
	 * lever for staying well under the YouTube API quota. */
	QImage lastUploadedSignature;
};

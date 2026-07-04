/******************************************************************************
    Live visualization for the Voice Level Match filter.

    Polls the filter's stats proc handler (~30 Hz) and draws:
      - current speech-level estimates for the mic and the reference
        (voice call) audio, with voice-activity indicators
      - the gain currently applied to the mic
      - a scrolling history graph of both estimates and the gain
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QDialog>
#include <QPointer>

#include <deque>

class QCheckBox;
class QLabel;
class QTimer;

class VoiceMatchGraph;

class VoiceMatchDialog : public QDialog {
	Q_OBJECT

public:
	/* source: the microphone source carrying the filter */
	VoiceMatchDialog(OBSSource source, QWidget *parent = nullptr);

	/* Returns the Voice Level Match filter on the source, creating it (with
	 * the first desktop-audio source as reference) when missing. */
	static OBSSourceAutoRelease FindOrCreateFilter(obs_source_t *source);

private:
	OBSWeakSourceAutoRelease weakSource;

	QCheckBox *enabledCheck;
	QLabel *statusLabel;
	VoiceMatchGraph *graph;
	QPointer<QTimer> pollTimer;

	void poll();
};

/* Custom-painted bars + history graph */
class VoiceMatchGraph : public QWidget {
	Q_OBJECT

public:
	struct Sample {
		float micDb;
		float refDb;
		float gainDb;
		bool micVad;
		bool refVad;
	};

	VoiceMatchGraph(QWidget *parent = nullptr);

	void addSample(const Sample &s, bool warm);
	void setInactive();

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	std::deque<Sample> history; /* newest at back */
	Sample current{-90.0f, -90.0f, 0.0f, false, false};
	bool warm = false;
	bool active = false;

	static constexpr int kMaxSamples = 30 * 60; /* 60 s at 30 Hz */
};

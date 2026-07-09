/******************************************************************************
    Output Routing Visualizer.

    A diagnostic window that shows what is actually going where: the
    Recording on the left, the Stream (and every enabled multi-stream
    destination) on the right, each with its own live low-frame-rate video
    preview, live resolution/bitrate readout, and the audio meters of the
    sources actually feeding that side.

    If a given output isn't currently running, its column still shows a
    preview and stats as if it were — the preview is driven by the same
    dedicated video mix the real output would use (created on demand for
    multi-stream destinations with a custom resolution), and the stats
    fall back to the configured target resolution/bitrate, clearly marked
    as not live.

    Purpose: let the user confirm sources are actually routed to the output
    they expect (Output Visibility / audio track separation) and compare
    per-destination settings (e.g. a high-quality horizontal YouTube stream
    next to a lower-bitrate vertical TikTok one) side by side.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QDialog>
#include <QImage>
#include <QPointer>

#include <memory>
#include <vector>

class OBSBasic;
class QLabel;
class QTimer;
class QBoxLayout;
class VolumeMeter;

struct RoutingColumn {
	QString title;

	/* Video preview */
	video_t *video = nullptr; /* borrowed unless ownsMix */
	bool ownsMix = false;     /* created via obs_add_cropped_scaled_mix; release on teardown */
	bool hooked = false;      /* video_output_connect2 succeeded */
	QLabel *previewLabel = nullptr;
	uint32_t previewW = 0;
	uint32_t previewH = 0;
	bool loggedFirstFrame = false;

	/* Live stats, when this column has an active output driving it */
	OBSWeakOutputAutoRelease weakOutput;
	uint64_t lastBytes = 0;
	uint64_t lastBytesTime = 0;
	bool haveLastBytes = false;

	/* Hypothetical/configured fallback, used whenever there's no active
	 * output (or for the recording column, which has no bitrate target) */
	int cfgWidth = 0;
	int cfgHeight = 0;
	int cfgBitrate = 0;

	QLabel *statsLabel = nullptr;

	~RoutingColumn();
};

class OutputRoutingVisualizer : public QDialog {
	Q_OBJECT

public:
	explicit OutputRoutingVisualizer(OBSBasic *main, QWidget *parent = nullptr);
	~OutputRoutingVisualizer() override;

private:
	OBSBasic *main;

	std::vector<std::unique_ptr<RoutingColumn>> streamColumns;
	std::unique_ptr<RoutingColumn> recordColumn;

	QBoxLayout *streamColumnsLayout = nullptr;
	QBoxLayout *recordAudioLayout = nullptr;
	QBoxLayout *streamAudioLayout = nullptr;

	std::vector<VolumeMeter *> recordMeters;
	std::vector<VolumeMeter *> streamMeters;

	QPointer<QTimer> statsTimer;

	void BuildRecordColumn();
	void BuildStreamColumns();
	QWidget *MakeColumnWidget(RoutingColumn *col);
	void HookPreview(RoutingColumn *col, uint32_t boundW, uint32_t boundH);
	void RebuildAudioMeters();
	void UpdateStats();

	static void RawVideoFrame(void *param, struct video_data *frame);
};

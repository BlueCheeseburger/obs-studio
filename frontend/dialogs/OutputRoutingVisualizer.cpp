#include "OutputRoutingVisualizer.hpp"

#include <components/VolumeMeter.hpp>
#include <utility/BasicOutputHandler.hpp>
#include <utility/MultiStreamOutput.hpp>
#include <widgets/OBSBasic.hpp>

#include <media-io/media-io-defs.h>
#include <media-io/video-io.h>
#include <util/platform.h>

#include <qt-wrappers.hpp>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include "moc_OutputRoutingVisualizer.cpp"

/* Preview tuning: cheap enough to run continuously alongside a live stream/
 * recording without adding meaningful GPU/CPU load — this is a routing
 * sanity check, not a program monitor. */
static constexpr uint32_t kPreviewBoundW = 220;
static constexpr uint32_t kPreviewBoundH = 220;
static constexpr int kPreviewTargetFps = 6;
static constexpr int kStatsIntervalMs = 1000;

RoutingColumn::~RoutingColumn()
{
	if (hooked && video) {
		obs_remove_raw_video_callback_mix(video, &OutputRoutingVisualizer::RawVideoFrame, this);
		hooked = false;
	}
	if (ownsMix && video) {
		obs_remove_video_mix(video);
		video = nullptr;
	}
}

/* ------------------------------------------------------------------------- */

OutputRoutingVisualizer::OutputRoutingVisualizer(OBSBasic *main_, QWidget *parent) : QDialog(parent), main(main_)
{
	setWindowTitle(QTStr("OutputRouting.Title"));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(900, 640);

	auto *root = new QHBoxLayout(this);

	/* ---- left: recording ---- */
	auto *recordSide = new QVBoxLayout();
	auto *recordHeader = new QLabel(QTStr("OutputRouting.Recording"), this);
	QFont hf = recordHeader->font();
	hf.setBold(true);
	hf.setPointSize(hf.pointSize() + 2);
	recordHeader->setFont(hf);
	recordSide->addWidget(recordHeader);

	BuildRecordColumn();
	recordSide->addWidget(MakeColumnWidget(recordColumn.get()));

	auto *recordAudioBox = new QGroupBox(QTStr("OutputRouting.RecordingAudio"), this);
	recordAudioLayout = new QHBoxLayout();
	recordAudioLayout->addStretch(1);
	recordAudioBox->setLayout(recordAudioLayout);
	recordSide->addWidget(recordAudioBox);
	recordSide->addStretch(1);

	root->addLayout(recordSide, 1);

	/* ---- right: stream + multi-stream destinations ---- */
	auto *streamSide = new QVBoxLayout();
	auto *streamHeader = new QLabel(QTStr("OutputRouting.Streaming"), this);
	streamHeader->setFont(hf);
	streamSide->addWidget(streamHeader);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto *scrollContent = new QWidget();
	streamColumnsLayout = new QHBoxLayout(scrollContent);

	BuildStreamColumns();
	for (auto &col : streamColumns)
		streamColumnsLayout->addWidget(MakeColumnWidget(col.get()));
	streamColumnsLayout->addStretch(1);

	scroll->setWidget(scrollContent);
	streamSide->addWidget(scroll, 1);

	auto *streamAudioBox = new QGroupBox(QTStr("OutputRouting.StreamingAudio"), this);
	streamAudioLayout = new QHBoxLayout();
	streamAudioLayout->addStretch(1);
	streamAudioBox->setLayout(streamAudioLayout);
	streamSide->addWidget(streamAudioBox);

	root->addLayout(streamSide, 2);

	RebuildAudioMeters();

	statsTimer = new QTimer(this);
	connect(statsTimer, &QTimer::timeout, this, &OutputRoutingVisualizer::UpdateStats);
	statsTimer->start(kStatsIntervalMs);
	UpdateStats();
}

OutputRoutingVisualizer::~OutputRoutingVisualizer()
{
	/* Columns (and their video hooks/owned mixes) must be torn down while
	 * this object — and libobs itself — is still fully alive; explicit
	 * destruction order here doesn't rely on member/vector defaults. */
	streamColumns.clear();
	recordColumn.reset();
}

/* ------------------------------------------------------------------------- */

void OutputRoutingVisualizer::BuildRecordColumn()
{
	recordColumn = std::make_unique<RoutingColumn>();
	recordColumn->title = QTStr("OutputRouting.Recording");

	BasicOutputHandler *handler = main->GetOutputHandler();
	recordColumn->video = (handler && handler->recordVideo) ? handler->recordVideo : obs_get_video();

	if (handler && handler->fileOutput)
		recordColumn->weakOutput = obs_output_get_weak_output(handler->fileOutput);

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		recordColumn->cfgWidth = (int)ovi.output_width;
		recordColumn->cfgHeight = (int)ovi.output_height;
	}
}

void OutputRoutingVisualizer::BuildStreamColumns()
{
	BasicOutputHandler *handler = main->GetOutputHandler();

	struct obs_video_info ovi;
	int primaryW = 0, primaryH = 0;
	if (obs_get_video_info(&ovi)) {
		primaryW = (int)ovi.output_width;
		primaryH = (int)ovi.output_height;
	}

	int primaryBitrate = 0;
	obs_encoder_t *primaryEncoder = (handler && handler->streamOutput)
						 ? obs_output_get_video_encoder(handler->streamOutput)
						 : nullptr;
	if (primaryEncoder) {
		OBSDataAutoRelease s = obs_encoder_get_settings(primaryEncoder);
		primaryBitrate = (int)obs_data_get_int(s, "bitrate");
		if (obs_encoder_get_width(primaryEncoder) && obs_encoder_get_height(primaryEncoder)) {
			primaryW = (int)obs_encoder_get_width(primaryEncoder);
			primaryH = (int)obs_encoder_get_height(primaryEncoder);
		}
	}

	/* primary stream column */
	{
		auto col = std::make_unique<RoutingColumn>();
		col->title = QTStr("OutputRouting.PrimaryStream");
		col->video = (handler && handler->streamVideo) ? handler->streamVideo : obs_get_video();
		if (handler && handler->streamOutput)
			col->weakOutput = obs_output_get_weak_output(handler->streamOutput);
		col->cfgWidth = primaryW;
		col->cfgHeight = primaryH;
		col->cfgBitrate = primaryBitrate;
		streamColumns.push_back(std::move(col));
	}

	/* multi-stream destinations */
	MultiStreamOutput *ms = main->GetMultiStreamOutput();
	if (!ms)
		return;

	ms->LoadConfig();

	for (auto &dest : ms->destinations) {
		if (!dest.enabled)
			continue;

		auto col = std::make_unique<RoutingColumn>();
		col->title = QT_UTF8(dest.name.c_str());

		if (dest.output)
			col->weakOutput = obs_output_get_weak_output(dest.output);

		if (dest.customVideoSettings && dest.width > 0 && dest.height > 0) {
			col->cfgWidth = dest.width;
			col->cfgHeight = dest.height;
			col->cfgBitrate = dest.bitrate > 0 ? dest.bitrate : primaryBitrate;

			if (dest.croppedVideo) {
				/* already streaming with a live cropped mix */
				col->video = dest.croppedVideo;
			} else {
				/* hypothetical preview: create our own cropped mix so
				 * the crop shows correctly even before the user hits
				 * Start Streaming. can_reuse_mix_texture() will let
				 * this share render work with the real mix if/when
				 * one is created with the exact same crop. */
				video_t *preview = obs_add_cropped_scaled_mix((uint32_t)dest.width, (uint32_t)dest.height,
									       OBS_SOURCE_OUTPUT_FILTER_STREAM);
				if (preview) {
					col->video = preview;
					col->ownsMix = true;
				} else {
					col->video = (handler && handler->streamVideo) ? handler->streamVideo
											: obs_get_video();
				}
			}
		} else {
			/* no custom video: this destination mirrors the primary
			 * stream's encoder exactly */
			col->video = (handler && handler->streamVideo) ? handler->streamVideo : obs_get_video();
			col->cfgWidth = primaryW;
			col->cfgHeight = primaryH;
			col->cfgBitrate = primaryBitrate;
		}

		streamColumns.push_back(std::move(col));
	}
}

QWidget *OutputRoutingVisualizer::MakeColumnWidget(RoutingColumn *col)
{
	auto *box = new QGroupBox(col->title, this);
	auto *layout = new QVBoxLayout();

	col->previewLabel = new QLabel(this);
	col->previewLabel->setAlignment(Qt::AlignCenter);
	col->previewLabel->setFixedSize(kPreviewBoundW, kPreviewBoundH * 9 / 16);
	col->previewLabel->setStyleSheet(QStringLiteral("background-color: black;"));
	col->previewLabel->setScaledContents(false);
	layout->addWidget(col->previewLabel);

	col->statsLabel = new QLabel(QTStr("OutputRouting.Loading"), this);
	col->statsLabel->setAlignment(Qt::AlignCenter);
	col->statsLabel->setWordWrap(true);
	layout->addWidget(col->statsLabel);

	box->setLayout(layout);

	HookPreview(col, kPreviewBoundW, kPreviewBoundH);

	return box;
}

void OutputRoutingVisualizer::HookPreview(RoutingColumn *col, uint32_t boundW, uint32_t boundH)
{
	if (!col->video)
		return;

	uint32_t srcW = video_output_get_width(col->video);
	uint32_t srcH = video_output_get_height(col->video);
	if (!srcW || !srcH) {
		srcW = col->cfgWidth > 0 ? (uint32_t)col->cfgWidth : 1920;
		srcH = col->cfgHeight > 0 ? (uint32_t)col->cfgHeight : 1080;
	}

	double aspect = (double)srcW / (double)srcH;
	uint32_t w, h;
	if (aspect >= 1.0) {
		w = boundW;
		h = (uint32_t)((double)boundW / aspect);
	} else {
		h = boundH;
		w = (uint32_t)((double)boundH * aspect);
	}
	if (w < 2)
		w = 2;
	if (h < 2)
		h = 2;
	w &= ~1u;
	h &= ~1u;

	col->previewW = w;
	col->previewH = h;
	if (col->previewLabel)
		col->previewLabel->setFixedSize((int)w, (int)h);

	struct video_scale_info conv = {};
	conv.format = VIDEO_FORMAT_BGRA;
	conv.width = w;
	conv.height = h;
	conv.range = VIDEO_RANGE_DEFAULT;
	conv.colorspace = VIDEO_CS_DEFAULT;

	const struct video_output_info *info = video_output_get_info(col->video);
	uint32_t fps = 30;
	if (info && info->fps_den > 0)
		fps = info->fps_num / info->fps_den;
	uint32_t divisor = fps / (uint32_t)kPreviewTargetFps;
	if (divisor < 1)
		divisor = 1;

	obs_add_raw_video_callback_mix(col->video, &conv, divisor, &OutputRoutingVisualizer::RawVideoFrame, col);
	col->hooked = true;

	blog(LOG_INFO,
	     "OutputRoutingVisualizer: '%s' src=%ux%u preview=%ux%u divisor=%u hooked=%d",
	     QT_TO_UTF8(col->title), srcW, srcH, w, h, divisor, col->hooked);
}

void OutputRoutingVisualizer::RawVideoFrame(void *param, struct video_data *frame)
{
	auto *col = static_cast<RoutingColumn *>(param);
	if (!frame || !frame->data[0] || !col->previewLabel || !col->previewW || !col->previewH)
		return;

	if (!col->loggedFirstFrame) {
		col->loggedFirstFrame = true;
		/* sample a few bytes to distinguish "never renders" from
		 * "renders but comes out black" once this is checked in the log */
		uint8_t b0 = frame->data[0][0], b1 = frame->data[0][1], b2 = frame->data[0][2];
		blog(LOG_INFO,
		     "OutputRoutingVisualizer: '%s' first frame received, linesize=%u sample BGR=%u,%u,%u",
		     QT_TO_UTF8(col->title), frame->linesize[0], b0, b1, b2);
	}

	/* deep copy: the source buffer is only valid for the duration of this
	 * callback, which runs on the video thread */
	QImage image(col->previewW, col->previewH, QImage::Format_ARGB32);
	uint32_t copyBytes = std::min<uint32_t>(frame->linesize[0], col->previewW * 4);
	for (uint32_t y = 0; y < col->previewH; y++)
		memcpy(image.scanLine((int)y), frame->data[0] + (size_t)y * frame->linesize[0], copyBytes);

	QLabel *label = col->previewLabel;
	QMetaObject::invokeMethod(
		label,
		[label, image]() {
			label->setPixmap(QPixmap::fromImage(image).scaled(label->size(), Qt::KeepAspectRatio,
									   Qt::SmoothTransformation));
		},
		Qt::QueuedConnection);
}

/* ------------------------------------------------------------------------- */

void OutputRoutingVisualizer::RebuildAudioMeters()
{
	auto clearLayout = [](QBoxLayout *layout) {
		while (layout->count() > 0) {
			QLayoutItem *item = layout->takeAt(0);
			if (item->widget())
				item->widget()->deleteLater();
			delete item;
		}
	};
	clearLayout(recordAudioLayout);
	clearLayout(streamAudioLayout);
	recordMeters.clear();
	streamMeters.clear();

	const char *mode = config_get_string(main->Config(), "Output", "Mode");
	bool adv = astrcmpi(mode, "Advanced") == 0;

	uint32_t streamMask = 0x1;
	uint32_t recMask = 0x1;
	if (adv) {
		int streamTrack = (int)config_get_int(main->Config(), "AdvOut", "TrackIndex");
		if (streamTrack < 1 || streamTrack > MAX_AUDIO_MIXES)
			streamTrack = 1;
		streamMask = 1u << (streamTrack - 1);

		recMask = (uint32_t)config_get_int(main->Config(), "AdvOut", "RecTracks");
		if (recMask == 0)
			recMask = streamMask;
	}

	struct EnumCtx {
		uint32_t streamMask, recMask;
		QBoxLayout *streamLayout, *recLayout;
		std::vector<VolumeMeter *> *streamMeters, *recMeters;
		QWidget *parent;
	} ctx{streamMask, recMask, streamAudioLayout, recordAudioLayout, &streamMeters, &recordMeters, this};

	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			auto *c = static_cast<EnumCtx *>(param);

			if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;
			if (!obs_source_audio_active(source))
				return true;
			if (obs_source_muted(source))
				return true;

			uint32_t mixers = obs_source_get_audio_mixers(source);

			if (mixers & c->recMask) {
				auto *strip = new QWidget(c->parent);
				auto *layout = new QVBoxLayout(strip);
				layout->setContentsMargins(2, 2, 2, 2);
				auto *name = new QLabel(QT_UTF8(obs_source_get_name(source)), strip);
				name->setAlignment(Qt::AlignCenter);
				name->setWordWrap(true);
				auto *meter = new VolumeMeter(strip, source);
				meter->setMinimumSize(24, 80);
				layout->addWidget(name);
				layout->addWidget(meter);
				c->recLayout->insertWidget(c->recLayout->count() - 1, strip);
				c->recMeters->push_back(meter);
			}

			if (mixers & c->streamMask) {
				auto *strip = new QWidget(c->parent);
				auto *layout = new QVBoxLayout(strip);
				layout->setContentsMargins(2, 2, 2, 2);
				auto *name = new QLabel(QT_UTF8(obs_source_get_name(source)), strip);
				name->setAlignment(Qt::AlignCenter);
				name->setWordWrap(true);
				auto *meter = new VolumeMeter(strip, source);
				meter->setMinimumSize(24, 80);
				layout->addWidget(name);
				layout->addWidget(meter);
				c->streamLayout->insertWidget(c->streamLayout->count() - 1, strip);
				c->streamMeters->push_back(meter);
			}

			return true;
		},
		&ctx);
}

/* ------------------------------------------------------------------------- */

static QString FormatStats(bool live, int width, int height, int bitrateKbps, bool showBitrate)
{
	QString res = (width > 0 && height > 0) ? QString("%1x%2").arg(width).arg(height)
						 : QTStr("OutputRouting.UnknownRes");

	QString line = res;
	if (showBitrate && bitrateKbps > 0)
		line += QString(" @ %1 Kbps").arg(bitrateKbps);

	line += live ? QStringLiteral("\n") + QTStr("OutputRouting.Live")
		     : QStringLiteral("\n") + QTStr("OutputRouting.NotActive");
	return line;
}

void OutputRoutingVisualizer::UpdateStats()
{
	auto updateColumn = [](RoutingColumn *col, bool showBitrate) {
		if (!col || !col->statsLabel)
			return;

		OBSOutputAutoRelease output =
			col->weakOutput ? obs_weak_output_get_output(col->weakOutput) : nullptr;
		bool active = output && obs_output_active(output);

		int width = col->cfgWidth;
		int height = col->cfgHeight;
		int bitrateKbps = col->cfgBitrate;

		if (active) {
			obs_encoder_t *enc = obs_output_get_video_encoder(output);
			if (enc && obs_encoder_get_width(enc) && obs_encoder_get_height(enc)) {
				width = (int)obs_encoder_get_width(enc);
				height = (int)obs_encoder_get_height(enc);
			}

			uint64_t bytes = obs_output_get_total_bytes(output);
			uint64_t now = os_gettime_ns();
			if (col->haveLastBytes && bytes >= col->lastBytes) {
				double bits = double(bytes - col->lastBytes) * 8.0;
				double secs = double(now - col->lastBytesTime) / 1000000000.0;
				if (secs > 0.01)
					bitrateKbps = (int)(bits / secs / 1000.0);
			}
			col->lastBytes = bytes;
			col->lastBytesTime = now;
			col->haveLastBytes = true;
		} else {
			col->haveLastBytes = false;
		}

		col->statsLabel->setText(FormatStats(active, width, height, bitrateKbps, showBitrate));
	};

	updateColumn(recordColumn.get(), false);
	for (auto &col : streamColumns)
		updateColumn(col.get(), true);
}

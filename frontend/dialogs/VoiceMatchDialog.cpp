#include "VoiceMatchDialog.hpp"

#include <OBSApp.hpp>

#include <QCheckBox>
#include <QLabel>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include "moc_VoiceMatchDialog.cpp"

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */

static obs_source_t *find_voice_match_filter(obs_source_t *source)
{
	struct FindCtx {
		obs_source_t *found = nullptr;
	} ctx;

	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *c = static_cast<FindCtx *>(param);
			if (!c->found && strcmp(obs_source_get_unversioned_id(filter), "voice_match_filter") == 0)
				c->found = filter;
		},
		&ctx);

	return ctx.found ? obs_source_get_ref(ctx.found) : nullptr;
}

static bool find_desktop_audio(void *param, obs_source_t *source)
{
	auto *name = static_cast<std::string *>(param);
	const char *id = obs_source_get_unversioned_id(source);

	if (strcmp(id, "wasapi_output_capture") == 0) {
		*name = obs_source_get_name(source);
		return false; /* stop enumerating */
	}
	return true;
}

OBSSourceAutoRelease VoiceMatchDialog::FindOrCreateFilter(obs_source_t *source)
{
	OBSSourceAutoRelease filter = find_voice_match_filter(source);
	if (filter)
		return filter;

	/* default the reference to the first desktop-audio capture source */
	std::string refName;
	obs_enum_sources(find_desktop_audio, &refName);

	OBSDataAutoRelease settings = obs_data_create();
	if (!refName.empty())
		obs_data_set_string(settings, "reference_source", refName.c_str());

	filter = obs_source_create("voice_match_filter", "Voice Level Match", settings, nullptr);
	if (filter)
		obs_source_filter_add(source, filter);

	return filter;
}

/* ------------------------------------------------------------------------- */
/* graph widget                                                               */

VoiceMatchGraph::VoiceMatchGraph(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(460, 260);
}

void VoiceMatchGraph::addSample(const Sample &s, bool w)
{
	current = s;
	warm = w;
	active = true;
	history.push_back(s);
	while (history.size() > kMaxSamples)
		history.pop_front();
	update();
}

void VoiceMatchGraph::setInactive()
{
	active = false;
	update();
}

static inline float dbToX(float db, float x0, float w)
{
	/* -60 dB .. 0 dB across the bar */
	float t = (db + 60.0f) / 60.0f;
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	return x0 + t * w;
}

void VoiceMatchGraph::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QColor bg(24, 24, 24);
	const QColor grid(60, 60, 60);
	const QColor micCol(80, 160, 255);
	const QColor refCol(90, 210, 120);
	const QColor gainCol(255, 170, 60);
	const QColor text(220, 220, 220);
	const QColor dim(140, 140, 140);

	p.fillRect(rect(), bg);

	if (!active) {
		p.setPen(dim);
		p.drawText(rect(), Qt::AlignCenter, QTStr("VoiceMatch.Viz.NoFilter"));
		return;
	}

	const int margin = 12;
	const int barH = 22;
	const int labelW = 64;
	int y = margin;
	float barX = (float)(margin + labelW);
	float barW = (float)(width() - margin) - barX;

	QFont f = p.font();
	f.setPixelSize(12);
	p.setFont(f);

	auto drawBar = [&](const QString &label, float levelDb, bool vad, const QColor &col) {
		p.setPen(text);
		p.drawText(QRectF(margin, y, labelW - 8, barH), Qt::AlignVCenter | Qt::AlignLeft, label);

		QRectF barRect(barX, y + 3, barW, barH - 6);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(40, 40, 40));
		p.drawRoundedRect(barRect, 3, 3);

		float lx = dbToX(levelDb, barX, barW);
		p.setBrush(col);
		p.drawRoundedRect(QRectF(barX, y + 3, lx - barX, barH - 6), 3, 3);

		/* VAD light */
		p.setBrush(vad ? col : QColor(70, 70, 70));
		p.drawEllipse(QPointF(barX - 12, y + barH / 2.0), 5, 5);

		p.setPen(dim);
		p.drawText(QRectF(barX, y + 3, barW - 6, barH - 6), Qt::AlignVCenter | Qt::AlignRight,
			   QString::number(levelDb, 'f', 1) + " dB");

		y += barH + 8;
	};

	drawBar(QTStr("VoiceMatch.Viz.You"), current.micDb, current.micVad, micCol);
	drawBar(QTStr("VoiceMatch.Viz.Friends"), current.refDb, current.refVad, refCol);

	/* gain readout */
	p.setPen(gainCol);
	QString gainStr = QTStr("VoiceMatch.Viz.Gain")
				  .arg((current.gainDb >= 0 ? "+" : "") + QString::number(current.gainDb, 'f', 1));
	if (!warm)
		gainStr += "  (" + QTStr("VoiceMatch.Viz.Learning") + ")";
	p.drawText(QRectF(margin, y, width() - 2 * margin, 18), Qt::AlignVCenter | Qt::AlignLeft, gainStr);
	y += 24;

	/* history graph */
	QRectF g(margin, y, width() - 2 * margin, height() - y - margin);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(32, 32, 32));
	p.drawRoundedRect(g, 3, 3);

	/* grid lines every 12 dB (-60..0) */
	p.setPen(grid);
	for (int db = -48; db <= -12; db += 12) {
		float gy = (float)g.bottom() - ((db + 60.0f) / 60.0f) * (float)g.height();
		p.drawLine(QPointF(g.left(), gy), QPointF(g.right(), gy));
	}

	if (history.size() >= 2) {
		auto plot = [&](const QColor &col, auto getter, float lo, float hi) {
			QPolygonF poly;
			poly.reserve((int)history.size());
			const float dx = (float)g.width() / (float)(kMaxSamples - 1);
			float x = (float)g.right() - dx * (float)(history.size() - 1);
			for (const Sample &s : history) {
				float v = getter(s);
				float t = (v - lo) / (hi - lo);
				if (t < 0.0f)
					t = 0.0f;
				if (t > 1.0f)
					t = 1.0f;
				poly << QPointF(x, (float)g.bottom() - t * (float)g.height());
				x += dx;
			}
			p.setPen(QPen(col, 1.6));
			p.setBrush(Qt::NoBrush);
			p.drawPolyline(poly);
		};

		plot(micCol, [](const Sample &s) { return s.micDb; }, -60.0f, 0.0f);
		plot(refCol, [](const Sample &s) { return s.refDb; }, -60.0f, 0.0f);
		/* gain drawn on its own ±15 dB scale, centered */
		plot(gainCol, [](const Sample &s) { return s.gainDb; }, -15.0f, 15.0f);
	}

	/* legend */
	p.setFont(f);
	float lx = (float)g.left() + 8;
	auto legend = [&](const QColor &col, const QString &name) {
		p.setPen(Qt::NoPen);
		p.setBrush(col);
		p.drawRect(QRectF(lx, g.top() + 8, 10, 3));
		p.setPen(text);
		p.drawText(QPointF(lx + 14, g.top() + 14), name);
		lx += 24 + p.fontMetrics().horizontalAdvance(name);
	};
	legend(micCol, QTStr("VoiceMatch.Viz.You"));
	legend(refCol, QTStr("VoiceMatch.Viz.Friends"));
	legend(gainCol, QTStr("VoiceMatch.Viz.GainLegend"));
}

/* ------------------------------------------------------------------------- */
/* dialog                                                                     */

VoiceMatchDialog::VoiceMatchDialog(OBSSource source, QWidget *parent)
	: QDialog(parent),
	  weakSource(obs_source_get_weak_source(source))
{
	setWindowTitle(QTStr("VoiceMatch.Viz.Title").arg(obs_source_get_name(source)));
	setAttribute(Qt::WA_DeleteOnClose);
	setSizeGripEnabled(true);

	auto *layout = new QVBoxLayout(this);

	auto *topRow = new QHBoxLayout();
	enabledCheck = new QCheckBox(QTStr("VoiceMatch.Viz.Enabled"), this);
	statusLabel = new QLabel(this);
	topRow->addWidget(enabledCheck);
	topRow->addStretch(1);
	topRow->addWidget(statusLabel);
	layout->addLayout(topRow);

	graph = new VoiceMatchGraph(this);
	layout->addWidget(graph, 1);

	OBSSourceAutoRelease filter = FindOrCreateFilter(source);
	if (filter) {
		enabledCheck->setChecked(obs_source_enabled(filter));
	}

	connect(enabledCheck, &QCheckBox::toggled, this, [this](bool on) {
		OBSSourceAutoRelease src = obs_weak_source_get_source(weakSource);
		if (!src)
			return;
		OBSSourceAutoRelease f = find_voice_match_filter(src);
		if (f)
			obs_source_set_enabled(f, on);
	});

	pollTimer = new QTimer(this);
	connect(pollTimer, &QTimer::timeout, this, &VoiceMatchDialog::poll);
	pollTimer->start(33);

	resize(520, 380);
}

void VoiceMatchDialog::poll()
{
	OBSSourceAutoRelease src = obs_weak_source_get_source(weakSource);
	if (!src) {
		graph->setInactive();
		return;
	}

	OBSSourceAutoRelease filter = find_voice_match_filter(src);
	if (!filter) {
		graph->setInactive();
		statusLabel->setText(QTStr("VoiceMatch.Viz.NoFilter"));
		return;
	}

	proc_handler_t *ph = obs_source_get_proc_handler(filter);
	calldata_t cd = {};
	if (!proc_handler_call(ph, "get_voice_match_stats", &cd)) {
		calldata_free(&cd);
		graph->setInactive();
		return;
	}

	VoiceMatchGraph::Sample s;
	s.micDb = (float)calldata_float(&cd, "mic_db");
	s.refDb = (float)calldata_float(&cd, "ref_db");
	s.gainDb = (float)calldata_float(&cd, "gain_db");
	s.micVad = calldata_bool(&cd, "mic_vad");
	s.refVad = calldata_bool(&cd, "ref_vad");

	bool warm = calldata_bool(&cd, "mic_warm") && calldata_bool(&cd, "ref_warm");
	bool hasRef = calldata_bool(&cd, "has_ref");
	long long refCount = calldata_int(&cd, "ref_count");
	calldata_free(&cd);

	if (!obs_source_enabled(filter))
		statusLabel->setText(QTStr("VoiceMatch.Viz.Disabled"));
	else if (!hasRef)
		statusLabel->setText(QTStr("VoiceMatch.Viz.NoReference"));
	else if (!warm)
		statusLabel->setText(QTStr("VoiceMatch.Viz.Learning"));
	else
		statusLabel->setText(QTStr("VoiceMatch.Viz.Matching") +
				     QStringLiteral(" · %1 ref%2").arg(refCount).arg(refCount == 1 ? "" : "s"));

	graph->addSample(s, warm);
}

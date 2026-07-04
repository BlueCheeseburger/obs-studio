#include "VolumeControl.hpp"

#include <components/MuteCheckBox.hpp>
#include <components/VolumeMeter.hpp>
#include <components/VolumeName.hpp>
#include <components/VolumeSlider.hpp>
#include <dialogs/NameDialog.hpp>
#include <dialogs/VoiceMatchDialog.hpp>
#include <widgets/OBSBasic.hpp>

#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QObjectCleanupHandler>

#include "moc_VolumeControl.cpp"

namespace {
bool isSourceUnassigned(obs_source_t *source)
{
	uint32_t mixes = (obs_source_get_audio_mixers(source) & ((1 << MAX_AUDIO_MIXES) - 1));
	obs_monitoring_type mt = obs_source_get_monitoring_type(source);

	return mixes == 0 && mt != OBS_MONITORING_TYPE_MONITOR_ONLY;
}

void showUnassignedWarning(const char *name)
{
	auto msgBox = [=]() {
		QMessageBox msgbox(App()->GetMainWindow());
		msgbox.setWindowTitle(QTStr("VolControl.UnassignedWarning.Title"));
		msgbox.setText(QTStr("VolControl.UnassignedWarning.Text").arg(name));
		msgbox.setIcon(QMessageBox::Icon::Information);
		msgbox.addButton(QMessageBox::Ok);

		QCheckBox *cb = new QCheckBox(QTStr("DoNotShowAgain"));
		msgbox.setCheckBox(cb);

		msgbox.exec();

		if (cb->isChecked()) {
			config_set_bool(App()->GetUserConfig(), "General", "WarnedAboutUnassignedSources", true);
			config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
		}
	};

	QMetaObject::invokeMethod(App(), "Exec", Qt::QueuedConnection, Q_ARG(VoidFunc, msgBox));
}
} // namespace

VolumeControl::VolumeControl(obs_source_t *source, QWidget *parent, bool vertical)
	: weakSource_(OBSGetWeakRef(source)),
	  obs_fader(obs_fader_create(OBS_FADER_LOG)),
	  vertical(vertical),
	  contextMenu(nullptr),
	  QFrame(parent)
{
	utils = std::make_unique<idian::Utils>(this);

	uuid = obs_source_get_uuid(source);

	mainLayout = new QBoxLayout(QBoxLayout::LeftToRight, this);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);
	setLayout(mainLayout);

	categoryLabel = new QLabel("Active");
	categoryLabel->setAlignment(Qt::AlignCenter);
	utils->addClass(categoryLabel, "mixer-category");
	utils->addClass(categoryLabel, "text-tiny");

	nameButton = new VolumeName(source, this);
	nameButton->setMaximumWidth(140);
	utils->addClass(nameButton, "text-small");
	utils->addClass(nameButton, "mixer-name");

	muteButton = new QPushButton(this);
	muteButton->setCheckable(true);
	utils->addClass(muteButton, "btn-mute");

	monitorButton = new QPushButton(this);
	monitorButton->setCheckable(true);
	utils->addClass(monitorButton, "btn-monitor");

	volumeLabel = new QLabel(this);
	volumeLabel->setIndent(0);
	volumeLabel->setObjectName("volLabel");

	slider = new VolumeSlider(obs_fader, Qt::Horizontal, this);
	slider->setMinimum(0);
	slider->setMaximum(int(FADER_PRECISION));

	sourceName = obs_source_get_name(source);
	setObjectName(sourceName);

	utils->applyStateStylingEventFilter(muteButton);
	utils->applyStateStylingEventFilter(monitorButton);

	volumeMeter = new VolumeMeter(this, source);

	audioFilterLabel = new QLabel(this);
	audioFilterLabel->setObjectName("audioFilterLabel");
	audioFilterLabel->setAlignment(Qt::AlignCenter);
	audioFilterLabel->setFixedSize(14, 14);
	audioFilterLabel->hide();

	/* Noise suppression toggle — only on microphone sources (any audio
	 * source whose name contains "mic", e.g. "Mic/Aux"). The themed icon
	 * and green-when-active styling match the monitor button. */
	if (sourceName.contains("mic", Qt::CaseInsensitive)) {
		noiseSupprButton = new QPushButton(this);
		noiseSupprButton->setCheckable(true);
		noiseSupprButton->setToolTip(QTStr("Basic.AudioMixer.NoiseSuppress"));
		utils->addClass(noiseSupprButton, "btn-noise-suppress");
		utils->applyStateStylingEventFilter(noiseSupprButton);

		/* Voice Level Match — opens the live match visualization */
		voiceMatchButton = new QPushButton(this);
		voiceMatchButton->setToolTip(QTStr("Basic.AudioMixer.VoiceMatch"));
		utils->addClass(voiceMatchButton, "btn-voice-match");
		utils->applyStateStylingEventFilter(voiceMatchButton);
	}

	bool muted = obs_source_muted(source);
	bool unassigned = isSourceUnassigned(source);

	volumeMeter->setMuted(muted || unassigned);

	setLayoutVertical(vertical);
	setName(sourceName);

	obs_fader_add_callback(obs_fader, obsVolumeChanged, this);

	obsSignals.reserve(9);
	obsSignals.emplace_back(obs_source_get_signal_handler(source), "mute", obsVolumeMuted, this);
	obsSignals.emplace_back(obs_source_get_signal_handler(source), "audio_mixers", obsMixersOrMonitoringChanged,
				this);
	obsSignals.emplace_back(obs_source_get_signal_handler(source), "audio_monitoring", obsMixersOrMonitoringChanged,
				this);
	obsSignals.emplace_back(obs_source_get_signal_handler(source), "activate", VolumeControl::obsSourceActivated,
				this);
	obsSignals.emplace_back(obs_source_get_signal_handler(source), "deactivate",
				VolumeControl::obsSourceDeactivated, this);
	obsSignals.emplace_back(obs_source_get_signal_handler(source), "audio_activate",
				VolumeControl::obsSourceActivated, this);
	obsSignals.emplace_back(obs_source_get_signal_handler(source), "audio_deactivate",
				VolumeControl::obsSourceDeactivated, this);

	obsSignals.emplace_back(obs_source_get_signal_handler(source), "remove", VolumeControl::obsSourceDestroy, this);
	obsSignals.emplace_back(obs_source_get_signal_handler(source), "destroy", VolumeControl::obsSourceDestroy,
				this);

	if (noiseSupprButton) {
		obsSignals.emplace_back(obs_source_get_signal_handler(source), "filter_add",
					VolumeControl::obsFilterChanged, this);
		obsSignals.emplace_back(obs_source_get_signal_handler(source), "filter_remove",
					VolumeControl::obsFilterChanged, this);
	}

	setContextMenuPolicy(Qt::CustomContextMenu);
	connect(this, &QWidget::customContextMenuRequested, this, &VolumeControl::showVolumeControlMenu);

	connect(nameButton, &VolumeName::renamed, this, &VolumeControl::setName);
	connect(nameButton, &VolumeName::clicked, this, [&]() { showVolumeControlMenu(); });

	connect(slider, &VolumeSlider::valueChanged, this, &VolumeControl::sliderChanged);
	connect(muteButton, &QPushButton::clicked, this, &VolumeControl::handleMuteButton);
	connect(monitorButton, &QPushButton::clicked, this, &VolumeControl::handleMonitorButton);
	if (noiseSupprButton) {
		connect(noiseSupprButton, &QPushButton::clicked, this, &VolumeControl::toggleNoiseSuppr);
		updateNoiseSupprButton();
	}
	if (voiceMatchButton)
		connect(voiceMatchButton, &QPushButton::clicked, this, &VolumeControl::openVoiceMatch);

	OBSBasic *main = OBSBasic::Get();
	if (main) {
		connect(main, &OBSBasic::profileSettingChanged, this,
			[this](const std::string &category, const std::string &name) {
				if (category == "Audio" && name == "MeterDecayRate") {
					updateDecayRate();
				} else if (category == "Audio" && name == "PeakMeterType") {
					updatePeakMeterType();
				}
			});
	}

	obs_fader_attach_source(obs_fader, source);

	// Call volume changed once to init the slider position and label
	changeVolume();

	updateMixerState();
	updateAudioOutputFilter();
}

VolumeControl::~VolumeControl()
{
	obs_fader_remove_callback(obs_fader, obsVolumeChanged, this);

	obsSignals.clear();

	if (contextMenu) {
		contextMenu->close();
	}
}

void VolumeControl::updateAudioOutputFilter()
{
	if (!audioFilterLabel)
		return;
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source)
		return;

	/* The old per-source "Audio Output Destination" feature stripped a
	 * source from individual mixer tracks to fake stream/record audio
	 * separation. In Simple Output mode streaming and recording share
	 * track 1, so excluding a source from a track silently dropped it from
	 * the recording (it still showed in the mixer). Heal any source left in
	 * that broken state: restore it to all tracks and clear the flag. */
	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	int filter = (int)obs_data_get_int(priv, "audio_output_filter");
	if (filter != 0) {
		obs_source_set_audio_mixers(source, 0xFF);
		obs_data_set_int(priv, "audio_output_filter", 0);
	}

	audioFilterLabel->setVisible(false);
}

const QIcon &VolumeControl::getUnassignedIcon()
{
	static const QIcon &icon = *new QIcon(":/res/images/unassigned.svg");
	return icon;
}

const QIcon &VolumeControl::getMutedIcon()
{
	static const QIcon &icon = *new QIcon(":/settings/images/settings/audio.svg");
	return icon;
}

const QIcon &VolumeControl::getUnmutedIcon()
{
	static const QIcon &icon = *new QIcon(":/settings/images/settings/audio.svg");
	return icon;
}

const QIcon &VolumeControl::getMonitorOnIcon()
{
	static const QIcon &icon = *new QIcon(":/res/images/headphones.svg");
	return icon;
}

const QIcon &VolumeControl::getMonitorOffIcon()
{
	static const QIcon &icon = *new QIcon(":/res/images/headphones-off.svg");
	return icon;
}

void VolumeControl::obsVolumeChanged(void *data, float)
{
	VolumeControl *volControl = static_cast<VolumeControl *>(data);

	QMetaObject::invokeMethod(volControl, "changeVolume", Qt::QueuedConnection);
}

void VolumeControl::obsVolumeMuted(void *data, calldata_t *)
{
	VolumeControl *volControl = static_cast<VolumeControl *>(data);

	QMetaObject::invokeMethod(volControl, "updateMixerState", Qt::QueuedConnection);
}

void VolumeControl::obsMixersOrMonitoringChanged(void *data, calldata_t *)
{
	VolumeControl *volControl = static_cast<VolumeControl *>(data);
	QMetaObject::invokeMethod(volControl, "updateMixerState", Qt::QueuedConnection);
}

void VolumeControl::obsSourceActivated(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<VolumeControl *>(data), "sourceActiveChanged", Qt::QueuedConnection,
				  Q_ARG(bool, true));
}

void VolumeControl::obsSourceDeactivated(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<VolumeControl *>(data), "sourceActiveChanged", Qt::QueuedConnection,
				  Q_ARG(bool, false));
}

void VolumeControl::obsSourceDestroy(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<VolumeControl *>(data), "handleSourceDestroyed", Qt::QueuedConnection);
}

void VolumeControl::setLayoutVertical(bool vertical)
{
	QBoxLayout *newLayout = new QBoxLayout(QBoxLayout::TopToBottom);
	newLayout->setContentsMargins(0, 0, 0, 0);
	newLayout->setSpacing(0);

	if (vertical) {
		setMaximumWidth(110);
		setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

		QHBoxLayout *categoryLayout = new QHBoxLayout;
		QHBoxLayout *nameLayout = new QHBoxLayout;
		QHBoxLayout *controlLayout = new QHBoxLayout;
		QHBoxLayout *volLayout = new QHBoxLayout;
		QFrame *meterFrame = new QFrame;
		QHBoxLayout *meterLayout = new QHBoxLayout;

		volumeMeter->setVertical(true);
		volumeMeter->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);

		slider->setOrientation(Qt::Vertical);
		slider->setLayoutDirection(Qt::LeftToRight);
		slider->setDisplayTicks(true);

		nameButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
		categoryLabel->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
		volumeLabel->setAlignment(Qt::AlignLeft);

		categoryLayout->setAlignment(Qt::AlignCenter);
		nameLayout->setAlignment(Qt::AlignCenter);
		meterLayout->setAlignment(Qt::AlignCenter);
		controlLayout->setAlignment(Qt::AlignCenter);
		volLayout->setAlignment(Qt::AlignCenter);

		meterFrame->setObjectName("volMeterFrame");

		categoryLayout->setContentsMargins(0, 0, 0, 0);
		categoryLayout->setSpacing(0);
		categoryLayout->addWidget(categoryLabel);

		nameLayout->setContentsMargins(0, 0, 0, 0);
		nameLayout->setSpacing(0);
		nameLayout->addWidget(nameButton);

		controlLayout->setContentsMargins(0, 0, 0, 0);
		controlLayout->setSpacing(0);

		// Add Headphone (audio monitoring) widget here
		controlLayout->addWidget(muteButton);
		controlLayout->addWidget(monitorButton);
		if (noiseSupprButton)
			controlLayout->addWidget(noiseSupprButton);
		if (voiceMatchButton)
			controlLayout->addWidget(voiceMatchButton);
		controlLayout->addWidget(audioFilterLabel);

		meterLayout->setContentsMargins(0, 0, 0, 0);
		meterLayout->setSpacing(0);
		meterLayout->addWidget(slider);
		meterLayout->addWidget(volumeMeter);

		meterFrame->setLayout(meterLayout);

		volLayout->setContentsMargins(0, 0, 0, 0);
		volLayout->setSpacing(0);
		volLayout->addWidget(volumeLabel);
		volLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::MinimumExpanding, QSizePolicy::Maximum));

		newLayout->addItem(categoryLayout);
		newLayout->addItem(nameLayout);
		newLayout->addItem(volLayout);
		newLayout->addWidget(meterFrame);
		newLayout->addItem(controlLayout);

		newLayout->setStretch(0, 0);
		newLayout->setStretch(1, 0);
		newLayout->setStretch(2, 0);
		newLayout->setStretch(3, 1);
		newLayout->setStretch(4, 0);

		volumeMeter->setFocusProxy(slider);
	} else {
		setMaximumWidth(QWIDGETSIZE_MAX);
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

		QHBoxLayout *textLayout = new QHBoxLayout;
		QHBoxLayout *controlLayout = new QHBoxLayout;
		QFrame *meterFrame = new QFrame;
		QVBoxLayout *meterLayout = new QVBoxLayout;
		QVBoxLayout *buttonLayout = new QVBoxLayout;

		volumeMeter->setVertical(false);
		volumeMeter->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);

		slider->setOrientation(Qt::Horizontal);
		slider->setLayoutDirection(Qt::LeftToRight);
		slider->setDisplayTicks(true);

		nameButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
		categoryLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
		volumeLabel->setAlignment(Qt::AlignRight);

		QHBoxLayout *textSubLayout = new QHBoxLayout;
		textSubLayout->setContentsMargins(0, 0, 0, 0);
		textLayout->setContentsMargins(0, 0, 0, 0);

		textLayout->addWidget(nameButton);
		textLayout->addItem(textSubLayout);

		textSubLayout->addSpacerItem(
			new QSpacerItem(0, 0, QSizePolicy::MinimumExpanding, QSizePolicy::Preferred));
		textSubLayout->addWidget(categoryLabel);
		textSubLayout->addWidget(volumeLabel);

		meterFrame->setObjectName("volMeterFrame");
		meterFrame->setLayout(meterLayout);

		meterLayout->setContentsMargins(0, 0, 0, 0);
		meterLayout->setSpacing(0);

		meterLayout->addWidget(slider);
		meterLayout->addWidget(volumeMeter);
		meterLayout->setStretch(0, 2);
		meterLayout->setStretch(1, 2);

		buttonLayout->setContentsMargins(0, 0, 0, 0);
		buttonLayout->setSpacing(0);

		buttonLayout->addWidget(muteButton);
		buttonLayout->addWidget(monitorButton);
		if (noiseSupprButton)
			buttonLayout->addWidget(noiseSupprButton);
		if (voiceMatchButton)
			buttonLayout->addWidget(voiceMatchButton);
		buttonLayout->addWidget(audioFilterLabel);

		controlLayout->addItem(buttonLayout);
		controlLayout->addWidget(meterFrame);

		newLayout->addItem(textLayout);
		newLayout->addItem(controlLayout);
		newLayout->setStretch(0, 3);
		newLayout->setStretch(1, 6);

		volumeMeter->setFocusProxy(slider);
	}

	QWidget().setLayout(mainLayout);

	setLayout(newLayout);
	mainLayout = newLayout;
	updateTabOrder();

	adjustSize();
}

void VolumeControl::showVolumeControlMenu(QPoint pos)
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		return;
	}

	QMenu *popup = new QMenu(this);

	// Create menu QActions
	QAction *lockAction = new QAction(QTStr("LockVolume"), popup);
	lockAction->setCheckable(true);
	lockAction->setChecked(mixerStatus().has(VolumeControl::MixerStatus::Locked));

	bool isGlobal = mixerStatus().has(VolumeControl::MixerStatus::Global);

	QAction *pinAction = new QAction(QTStr("Basic.AudioMixer.Pin"), popup);
	bool isPinned = mixerStatus().has(VolumeControl::MixerStatus::Pinned);
	if (isPinned) {
		pinAction->setText(QTStr("Basic.AudioMixer.Unpin"));
	}

	QAction *hideAction = new QAction(QTStr("Basic.AudioMixer.Hide"), popup);
	bool isHidden = mixerStatus().has(VolumeControl::MixerStatus::Hidden);
	if (isHidden && !isGlobal) {
		hideAction->setText(QTStr("Basic.AudioMixer.Unhide"));
	}

	QAction *unhideAllAction = new QAction(QTStr("UnhideAll"), popup);
	QAction *mixerRenameAction = new QAction(QTStr("Rename"), popup);

	QAction *copyFiltersAction = new QAction(QTStr("Copy.Filters"), popup);
	QAction *pasteFiltersAction = new QAction(QTStr("Paste.Filters"), popup);

	QAction *filtersAction = new QAction(QTStr("Filters"), popup);
	QAction *propertiesAction = new QAction(QTStr("Properties"), popup);

	/* Audio-only sources are hidden from the Sources panel, so they need a
	 * Remove option here — otherwise there is no way to delete them. */
	uint32_t srcOutputFlags = obs_source_get_output_flags(source);
	bool isAudioOnly = (srcOutputFlags & OBS_SOURCE_AUDIO) && !(srcOutputFlags & OBS_SOURCE_VIDEO);
	/* Only non-global audio-only sources (scene-item mic/capture sources
	 * hidden from the Sources panel) need a Remove here. Global sources
	 * (Desktop Audio, Mic/Aux) are managed through Audio Settings. */
	QAction *removeAction = (isAudioOnly && !isGlobal) ? new QAction(QTStr("Remove"), popup) : nullptr;

	// Set properties on actions that require source reference
	hideAction->setProperty("source", QVariant::fromValue<OBSSource>(source));
	pinAction->setProperty("source", QVariant::fromValue<OBSSource>(source));

	mixerRenameAction->setProperty("source", QVariant::fromValue<OBSSource>(source));

	copyFiltersAction->setProperty("source", QVariant::fromValue<OBSSource>(source));
	pasteFiltersAction->setProperty("source", QVariant::fromValue<OBSSource>(source));

	filtersAction->setProperty("source", QVariant::fromValue<OBSSource>(source));
	propertiesAction->setProperty("source", QVariant::fromValue<OBSSource>(source));

	// Connect actions to signals
	OBSBasic *main = OBSBasic::Get();

	connect(unhideAllAction, &QAction::triggered, this, [this]() { emit unhideAll(); });

	connect(hideAction, &QAction::triggered, this, [this, isHidden]() { setHiddenInMixer(!isHidden); });
	connect(
		pinAction, &QAction::triggered, this, [this, isPinned]() { setPinnedInMixer(!isPinned); },
		Qt::DirectConnection);
	connect(lockAction, &QAction::toggled, this, &VolumeControl::setLocked);

	connect(copyFiltersAction, &QAction::triggered, main, &OBSBasic::actionCopyFilters);
	connect(pasteFiltersAction, &QAction::triggered, main, &OBSBasic::actionPasteFilters);

	connect(mixerRenameAction, &QAction::triggered, this, &VolumeControl::renameSource);

	connect(filtersAction, &QAction::triggered, main, &OBSBasic::actionOpenSourceFilters);
	connect(propertiesAction, &QAction::triggered, main, &OBSBasic::actionOpenSourceProperties);

	if (removeAction) {
		OBSSource sourceRef = source;
		connect(removeAction, &QAction::triggered, this, [this, sourceRef]() {
			const char *name = obs_source_get_name(sourceRef);
			QString text = QTStr("ConfirmRemove.Text").arg(QT_UTF8(name));
			QMessageBox confirm(this);
			confirm.setText(text);
			QPushButton *yes = confirm.addButton(QTStr("Yes"), QMessageBox::YesRole);
			confirm.setDefaultButton(yes);
			confirm.addButton(QTStr("No"), QMessageBox::NoRole);
			confirm.setIcon(QMessageBox::Question);
			confirm.setWindowTitle(QTStr("ConfirmRemove.Title"));
			confirm.exec();
			if (yes == confirm.clickedButton())
				obs_source_remove(sourceRef);
		});
	}

	// Enable/disable actions
	copyFiltersAction->setEnabled(obs_source_filter_count(source) > 0);
	pasteFiltersAction->setEnabled(!obs_weak_source_expired(main->copyFiltersSource()));

	if (isGlobal) {
		pinAction->setDisabled(true);
		hideAction->setDisabled(true);
	}

	if (isPinned) {
		hideAction->setDisabled(true);
	}

	// Build menu
	popup->addAction(unhideAllAction);
	popup->addSeparator();
	popup->addAction(pinAction);
	popup->addAction(hideAction);
	popup->addAction(lockAction);

	popup->addSeparator();
	popup->addAction(copyFiltersAction);
	popup->addAction(pasteFiltersAction);
	popup->addSeparator();
	popup->addAction(mixerRenameAction);
	popup->addSeparator();

	if (removeAction) {
		popup->addAction(removeAction);
		popup->addSeparator();
	}

	/* Output Visibility submenu — mirrors the video source panel behavior */
	{
		QMenu *outputFilterMenu = new QMenu(QTStr("OutputFilter"), popup);
		auto curFilter = obs_source_get_output_filter(source);

		auto makeOFAction = [&](const char *key, enum obs_source_output_filter val) {
			QAction *a = outputFilterMenu->addAction(QTStr(key));
			a->setCheckable(true);
			a->setChecked(curFilter == val);
			OBSSource src = source;
			connect(a, &QAction::triggered, this, [src, val]() {
				obs_source_set_output_filter(src, val);
				OBSBasic::Get()->SaveProject();
			});
		};

		makeOFAction("OutputFilter.All", OBS_SOURCE_OUTPUT_FILTER_ALL);
		makeOFAction("OutputFilter.StreamOnly", OBS_SOURCE_OUTPUT_FILTER_STREAM);
		makeOFAction("OutputFilter.RecordOnly", OBS_SOURCE_OUTPUT_FILTER_RECORD);
		popup->addMenu(outputFilterMenu);
	}

	/* Noise Suppression submenu — only on mic sources (ones that have the
	 * NS button). Enable toggle + Configure to open the filter's settings. */
	if (noiseSupprButton) {
		popup->addSeparator();
		QMenu *nsMenu = new QMenu(QTStr("Basic.AudioMixer.NoiseSuppress"), popup);

		struct NSCtx {
			bool found = false;
			bool enabled = false;
			const char *name = nullptr;
		} nsCtx;
		obs_source_enum_filters(
			source,
			[](obs_source_t *, obs_source_t *filter, void *data) {
				auto *c = static_cast<NSCtx *>(data);
				if (strcmp(obs_source_get_unversioned_id(filter), "noise_suppress_filter") == 0) {
					c->found = true;
					c->enabled = obs_source_enabled(filter);
					c->name = obs_source_get_name(filter);
				}
			},
			&nsCtx);

		QAction *nsEnableAction = new QAction(QTStr("Enable"), nsMenu);
		nsEnableAction->setCheckable(true);
		nsEnableAction->setChecked(nsCtx.found && nsCtx.enabled);
		connect(nsEnableAction, &QAction::triggered, this, &VolumeControl::toggleNoiseSuppr);
		nsMenu->addAction(nsEnableAction);

		if (nsCtx.found && nsCtx.name) {
			QAction *nsConfigAction = new QAction(QTStr("Configure"), nsMenu);
			std::string filterName(nsCtx.name);
			OBSSource srcRef = source;
			connect(nsConfigAction, &QAction::triggered, this, [srcRef, filterName]() {
				OBSSourceAutoRelease filter =
					obs_source_get_filter_by_name(srcRef, filterName.c_str());
				if (filter)
					OBSBasic::Get()->CreatePropertiesWindow(filter);
			});
			nsMenu->addAction(nsConfigAction);
		}

		popup->addMenu(nsMenu);
	}

	popup->addSeparator();
	popup->addAction(filtersAction);
	popup->addAction(propertiesAction);

	// Calculate menu position
	QPoint popupPos = mapToGlobal(pos);

	if (pos.isNull()) {
		QPoint menuPos = nameButton->mapToGlobal(nameButton->rect().bottomLeft());
		QSize menuSize = popup->sizeHint();

		QRect available = QGuiApplication::screenAt(menuPos)->availableGeometry();
		int spaceBelow = available.bottom() - menuPos.y();
		int spaceAbove = menuPos.y() - available.top();

		if (menuSize.height() > spaceBelow && spaceAbove > spaceBelow) {
			menuPos = nameButton->mapToGlobal(nameButton->rect().topLeft());
			menuPos.ry() -= menuSize.height();
		}

		if (menuPos.x() + menuSize.width() > available.right()) {
			menuPos.rx() = available.right() - menuSize.width();
		}

		popupPos = menuPos;
	}

	popup->popup(popupPos);

	connect(popup, &QMenu::aboutToHide, popup, &QMenu::deleteLater);
}

void VolumeControl::renameSource()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	OBSSource source = action->property("source").value<OBSSource>();

	std::string uuid = obs_source_get_uuid(source);

	OBSBasic *main = OBSBasic::Get();

	// Defer the rename dialog to avoid blocking the UI thread while the context menu is closing, which can cause issues on some platforms
	QTimer::singleShot(0, main, [main, uuid]() {
		if (!main) {
			return;
		}

		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (!source) {
			return;
		}

		const char *prevName = obs_source_get_name(source);

		for (;;) {
			std::string name;
			bool accepted = NameDialog::AskForName(main, QTStr("Basic.Main.MixerRename.Title"),
							       QTStr("Basic.Main.MixerRename.Text"), name,
							       QT_UTF8(prevName));
			if (!accepted) {
				return;
			}

			if (name.empty()) {
				OBSMessageBox::warning(main, QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
				continue;
			}

			if (name == prevName) {
				return;
			}

			OBSSourceAutoRelease sourceTest = obs_get_source_by_name(name.c_str());

			if (sourceTest) {
				OBSMessageBox::warning(main, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
				continue;
			}

			std::string prevName(obs_source_get_name(source));
			auto undo = [prevName](const std::string &data) {
				OBSSourceAutoRelease source = obs_get_source_by_uuid(data.c_str());
				obs_source_set_name(source, prevName.c_str());
			};

			std::string editedName = name;
			auto redo = [editedName](const std::string &data) {
				OBSSourceAutoRelease source = obs_get_source_by_uuid(data.c_str());
				obs_source_set_name(source, editedName.c_str());
			};

			main->undo_s.add_action(QTStr("Undo.Rename").arg(name.c_str()), undo, redo, uuid, uuid);

			obs_source_set_name(source, name.c_str());
			break;
		}
	});
}

void VolumeControl::changeVolume()
{
	QSignalBlocker blocker(slider);
	slider->setValue((int)(obs_fader_get_deflection(obs_fader) * FADER_PRECISION));

	updateText();
}

void VolumeControl::setLocked(bool locked)
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		return;
	}
	OBSDataAutoRelease priv_settings = obs_source_get_private_settings(source);
	obs_data_set_bool(priv_settings, "volume_locked", locked);

	enableSlider(!locked);
	mixerStatus().set(VolumeControl::MixerStatus::Locked, locked);

	OBSBasic *main = OBSBasic::Get();
	emit main->mixerStatusChanged(uuid);
}

void VolumeControl::updateCategoryLabel()
{
	QString labelText = QTStr("Basic.AudioMixer.Category.Active");

	if (mixerStatus().has(VolumeControl::MixerStatus::Unassigned)) {
		labelText = QTStr("Basic.AudioMixer.Category.Unassigned");
	} else if (mixerStatus().has(VolumeControl::MixerStatus::Global)) {
		labelText = QTStr("Basic.AudioMixer.Category.Global");
	} else if (mixerStatus().has(VolumeControl::MixerStatus::Pinned)) {
		labelText = QTStr("Basic.AudioMixer.Category.Pinned");
	} else if (mixerStatus().has(VolumeControl::MixerStatus::Hidden)) {
		labelText = QTStr("Basic.AudioMixer.Category.Hidden");
	} else if (!mixerStatus().has(VolumeControl::MixerStatus::Active)) {
		labelText = QTStr("Basic.AudioMixer.Category.Inactive");

		if (mixerStatus().has(VolumeControl::MixerStatus::Preview)) {
			labelText = QTStr("Basic.AudioMixer.Category.Preview");
		}
	}

	bool stylePinned = mixerStatus().has(VolumeControl::MixerStatus::Global) ||
			   mixerStatus().has(VolumeControl::MixerStatus::Pinned);
	bool styleInactive = mixerStatus().has(VolumeControl::MixerStatus::Active) != true;
	bool styleHidden = mixerStatus().has(VolumeControl::MixerStatus::Hidden);
	bool styleUnassigned = mixerStatus().has(VolumeControl::MixerStatus::Unassigned);
	bool stylePreviewed = mixerStatus().has(VolumeControl::MixerStatus::Preview);

	utils->toggleClass("volume-pinned", stylePinned);
	utils->toggleClass("volume-inactive", styleInactive);
	utils->toggleClass("volume-preview", styleInactive && stylePreviewed);
	utils->toggleClass("volume-hidden", styleHidden && !stylePinned);
	utils->toggleClass("volume-unassigned", styleUnassigned);

	categoryLabel->setText(labelText);
	categoryLabel->setAlignment(Qt::AlignCenter);

	style()->polish(categoryLabel);
	style()->polish(volumeMeter);

	bool forceUpdate = true;
	volumeMeter->updateBackgroundCache(forceUpdate);
}

void VolumeControl::updateDecayRate()
{
	OBSBasic *main = OBSBasic::Get();
	double meterDecayRate = config_get_double(main->Config(), "Audio", "MeterDecayRate");

	setMeterDecayRate(meterDecayRate);
}

void VolumeControl::updatePeakMeterType()
{
	OBSBasic *main = OBSBasic::Get();
	uint32_t peakMeterTypeIdx = config_get_uint(main->Config(), "Audio", "PeakMeterType");

	enum obs_peak_meter_type peakMeterType;
	switch (peakMeterTypeIdx) {
	case 0:
		peakMeterType = SAMPLE_PEAK_METER;
		break;
	case 1:
		peakMeterType = TRUE_PEAK_METER;
		break;
	default:
		peakMeterType = SAMPLE_PEAK_METER;
		break;
	}

	setPeakMeterType(peakMeterType);
}

void VolumeControl::setMuted(bool mute)
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		return;
	}

	bool prev = obs_source_muted(source);
	bool unassigned = isSourceUnassigned(source);

	obs_source_set_muted(source, mute);

	if (!mute && unassigned) {
		// Show notice about the source no being assigned to any tracks
		bool has_shown_warning =
			config_get_bool(App()->GetUserConfig(), "General", "WarnedAboutUnassignedSources");
		if (!has_shown_warning) {
			showUnassignedWarning(obs_source_get_name(source));
		}
	}

	auto undo_redo = [](const std::string &uuid, bool val) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		obs_source_set_muted(source, val);
	};

	QString text = QTStr(mute ? "Undo.Volume.Mute" : "Undo.Volume.Unmute");

	const char *name = obs_source_get_name(source);
	OBSBasic::Get()->undo_s.add_action(text.arg(name), std::bind(undo_redo, std::placeholders::_1, prev),
					   std::bind(undo_redo, std::placeholders::_1, mute), uuid, uuid);
}

void VolumeControl::setMonitoring(obs_monitoring_type type)
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		return;
	}

	obs_monitoring_type prevMonitoringType = obs_source_get_monitoring_type(source);
	obs_source_set_monitoring_type(source, type);

	auto undo_redo = [](const std::string &uuid, obs_monitoring_type val) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		obs_source_set_monitoring_type(source, val);
	};

	QString text = QTStr("Undo.MonitoringType.Change");

	const char *name = obs_source_get_name(source);
	OBSBasic::Get()->undo_s.add_action(text.arg(name),
					   std::bind(undo_redo, std::placeholders::_1, prevMonitoringType),
					   std::bind(undo_redo, std::placeholders::_1, type), uuid, uuid);
}

void VolumeControl::sourceActiveChanged(bool active)
{
	setUseDisabledColors(!active);
	mixerStatus().set(VolumeControl::MixerStatus::Active, active);

	OBSBasic *main = OBSBasic::Get();
	emit main->mixerStatusChanged(uuid);
}

void VolumeControl::updateMixerState()
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		deleteLater();
		return;
	}

	bool muted = obs_source_muted(source);
	bool unassigned = isSourceUnassigned(source);
	obs_monitoring_type monitoringType = obs_source_get_monitoring_type(source);

	bool isActive = obs_source_active(source) && obs_source_audio_active(source);

	mixerStatus().set(VolumeControl::MixerStatus::Active, isActive);
	mixerStatus().set(VolumeControl::MixerStatus::Unassigned, unassigned);

	QSignalBlocker blockMute(muteButton);
	QSignalBlocker blockMonitor(monitorButton);

	bool showAsMuted = muted || monitoringType == OBS_MONITORING_TYPE_MONITOR_ONLY;
	bool showAsMonitored = !muted && monitoringType != OBS_MONITORING_TYPE_NONE;
	bool showAsUnassigned = !muted && unassigned;

	volumeMeter->setMuted((showAsMuted || showAsUnassigned) && !showAsMonitored);
	setUseDisabledColors(showAsMuted || !isActive);

	muteButton->setChecked(showAsMuted);
	monitorButton->setChecked(showAsMonitored);

	QString muteTooltip = showAsMuted ? QTStr("Unmute") : QTStr("Mute");
	muteButton->setToolTip(muteTooltip);

	QString monitorTooltip = showAsMonitored ? QTStr("Basic.AudioMixer.Monitoring.Disable")
						 : QTStr("Basic.AudioMixer.Monitoring.Enable");
	monitorButton->setToolTip(monitorTooltip);

	if (showAsUnassigned) {
		muteButton->setIcon(getUnassignedIcon());
	} else if (showAsMuted) {
		muteButton->setIcon(getMutedIcon());
	} else {
		muteButton->setIcon(getUnmutedIcon());
	}

	if (showAsMonitored) {
		monitorButton->setIcon(getMonitorOnIcon());
	} else {
		monitorButton->setIcon(getMonitorOffIcon());
	}

	// Qt doesn't support overriding the QPushButton icon using pseudo state selectors like :checked
	// in QSS so we set a checked class selector on the button to be used instead.
	utils->toggleClass(muteButton, "checked", showAsMuted);
	utils->toggleClass(monitorButton, "checked", showAsMonitored);

	utils->toggleClass(muteButton, "mute-unassigned", showAsUnassigned);

	style()->polish(muteButton);
	style()->polish(monitorButton);

	updateCategoryLabel();
}

void VolumeControl::handleMuteButton(bool mute)
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		return;
	}

	// The Mute and Monitor buttons in the volume mixer work as a pseudo quad-state toggle.
	// Both buttons must be in their "off" state in order to actually process it as a mute.
	// Otherwise, clicking "Mute" with monitoring enabled will toggle the monitoring type.
	obs_monitoring_type monitoringType = obs_source_get_monitoring_type(source);

	if (mute && monitoringType == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT) {
		setMonitoring(OBS_MONITORING_TYPE_MONITOR_ONLY);
	} else if (!mute && monitoringType == OBS_MONITORING_TYPE_MONITOR_ONLY) {
		setMonitoring(OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
	} else {
		setMuted(mute);
	}
}

void VolumeControl::handleMonitorButton(bool enableMonitoring)
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		return;
	}

	// The Mute and Monitor buttons in the volume mixer work as a pseudo quad-state toggle.
	// The source is only ever actually "Muted" if Monitoring is set to None.
	obs_monitoring_type monitoringType = obs_source_get_monitoring_type(source);

	bool muted = obs_source_muted(source);

	if (!enableMonitoring) {
		setMonitoring(OBS_MONITORING_TYPE_NONE);
		if (monitoringType == OBS_MONITORING_TYPE_MONITOR_ONLY) {
			setMuted(true);
		}
	} else if (enableMonitoring && muted) {
		setMonitoring(OBS_MONITORING_TYPE_MONITOR_ONLY);
		setMuted(false);
	} else if (enableMonitoring && !muted) {
		setMonitoring(OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
	}
}

void VolumeControl::sliderChanged(int vol)
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		return;
	}

	float prev = obs_source_get_volume(source);

	obs_fader_set_deflection(obs_fader, float(vol) / FADER_PRECISION);
	updateText();

	auto undo_redo = [](const std::string &uuid, float val) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		obs_source_set_volume(source, val);
	};

	float val = obs_source_get_volume(source);
	const char *name = obs_source_get_name(source);
	OBSBasic::Get()->undo_s.add_action(QTStr("Undo.Volume.Change").arg(name),
					   std::bind(undo_redo, std::placeholders::_1, prev),
					   std::bind(undo_redo, std::placeholders::_1, val), uuid, uuid, true);
}

void VolumeControl::updateText()
{
	QString text;
	float db = obs_fader_get_db(obs_fader);

	if (db < -96.0f) {
		text = "-inf dB";
	} else {
		text = QString::number(db, 'f', 1).append(" dB");
	}

	volumeLabel->setText(text);

	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source) {
		return;
	}

	bool muted = obs_source_muted(source);
	const char *accTextLookup = muted ? "VolControl.SliderMuted" : "VolControl.SliderUnmuted";

	QString sourceName = obs_source_get_name(source);
	QString accText = QTStr(accTextLookup).arg(sourceName);

	slider->setAccessibleName(accText);
}

void VolumeControl::setVertical(bool vertical_)
{
	if (vertical == vertical_) {
		return;
	}

	vertical = vertical_;

	setLayoutVertical(vertical);
}

void VolumeControl::updateTabOrder()
{
	QWidget *prevFocus = firstWidget()->previousInFocusChain();
	QWidget *lastFocus = lastWidget()->nextInFocusChain();

	if (vertical) {
		setTabOrder(prevFocus, nameButton);
		setTabOrder(nameButton, slider);
		setTabOrder(slider, muteButton);
		setTabOrder(muteButton, monitorButton);
		setTabOrder(monitorButton, lastFocus);
	} else {
		setTabOrder(prevFocus, nameButton);
		setTabOrder(nameButton, muteButton);
		setTabOrder(muteButton, monitorButton);
		setTabOrder(monitorButton, slider);
		setTabOrder(slider, lastFocus);
	}
}

void VolumeControl::updateName()
{
	setName(sourceName);
}

void VolumeControl::setName(QString name)
{
	sourceName = name;

	muteButton->setAccessibleName(QTStr("VolControl.Mute").arg(name));
}

void VolumeControl::setMeterDecayRate(qreal q)
{
	volumeMeter->setPeakDecayRate(q);
}

void VolumeControl::setPeakMeterType(enum obs_peak_meter_type peakMeterType)
{
	volumeMeter->setPeakMeterType(peakMeterType);
}

void VolumeControl::enableSlider(bool enable)
{
	slider->setEnabled(enable);
}

void VolumeControl::setUseDisabledColors(bool greyscale)
{
	volumeMeter->setUseDisabledColors(greyscale);
}

void VolumeControl::setGlobalInMixer(bool global)
{
	if (mixerStatus().has(VolumeControl::MixerStatus::Global) != global) {
		mixerStatus().set(VolumeControl::MixerStatus::Global, global);

		OBSBasic *main = OBSBasic::Get();
		emit main->mixerStatusChanged(uuid);
	}
}

void VolumeControl::setPinnedInMixer(bool pinned)
{
	if (mixerStatus().has(VolumeControl::MixerStatus::Pinned) != pinned) {
		OBSSource source = OBSGetStrongRef(weakSource());
		if (!source) {
			return;
		}
		OBSDataAutoRelease priv_settings = obs_source_get_private_settings(source);
		obs_data_set_bool(priv_settings, "mixer_pinned", pinned);

		mixerStatus().set(VolumeControl::MixerStatus::Pinned, pinned);

		if (pinned) {
			// Unset hidden state when pinning controls
			setHiddenInMixer(false);
		}

		OBSBasic *main = OBSBasic::Get();
		emit main->mixerStatusChanged(uuid);
	}
}

void VolumeControl::setHiddenInMixer(bool hidden)
{
	if (mixerStatus().has(VolumeControl::MixerStatus::Hidden) != hidden) {
		OBSSource source = OBSGetStrongRef(weakSource());
		if (!source) {
			return;
		}
		OBSDataAutoRelease priv_settings = obs_source_get_private_settings(source);
		obs_data_set_bool(priv_settings, "mixer_hidden", hidden);

		mixerStatus().set(VolumeControl::MixerStatus::Hidden, hidden);

		OBSBasic *main = OBSBasic::Get();
		emit main->mixerStatusChanged(uuid);
	}
}

void VolumeControl::refreshColors()
{
	volumeMeter->refreshColors();
}

void VolumeControl::setLevels(const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
			      const float inputPeak[MAX_AUDIO_CHANNELS])
{
	if (volumeMeter) {
		volumeMeter->setLevels(magnitude, peak, inputPeak);
	}
}

void VolumeControl::toggleNoiseSuppr()
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source)
		return;

	/* Find any existing noise suppression filter by type, capture its name
	 * so we can retrieve it via obs_source_get_filter_by_name (which returns
	 * a properly ref-counted handle without needing obs_source_addref). */
	struct FindCtx {
		const char *name = nullptr;
	} ctx;

	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *data) {
			auto *c = static_cast<FindCtx *>(data);
			if (!c->name &&
			    strcmp(obs_source_get_unversioned_id(filter), "noise_suppress_filter") == 0)
				c->name = obs_source_get_name(filter);
		},
		&ctx);

	if (ctx.name) {
		OBSSourceAutoRelease filter = obs_source_get_filter_by_name(source, ctx.name);
		if (filter)
			obs_source_set_enabled(filter, !obs_source_enabled(filter));
	} else {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "method", "rnnoise");
		OBSSourceAutoRelease filter =
			obs_source_create("noise_suppress_filter", "Noise Suppression", settings, nullptr);
		if (filter)
			obs_source_filter_add(source, filter);
	}

	updateNoiseSupprButton();
}

void VolumeControl::openVoiceMatch()
{
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source)
		return;

	/* Ensure the filter exists (created with Desktop Audio as the default
	 * reference), then show the live visualization. */
	VoiceMatchDialog::FindOrCreateFilter(source);

	auto *dialog = new VoiceMatchDialog(source, OBSBasic::Get());
	dialog->show();
}

void VolumeControl::updateNoiseSupprButton()
{
	if (!noiseSupprButton)
		return;
	OBSSource source = OBSGetStrongRef(weakSource());
	if (!source)
		return;

	struct CheckCtx {
		bool found = false;
		bool enabled = false;
	} ctx;

	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *data) {
			auto *c = static_cast<CheckCtx *>(data);
			if (strcmp(obs_source_get_unversioned_id(filter), "noise_suppress_filter") == 0) {
				c->found = true;
				c->enabled = obs_source_enabled(filter);
			}
		},
		&ctx);

	bool active = ctx.found && ctx.enabled;
	noiseSupprButton->setChecked(active);

	/* Qt can't swap the QPushButton icon via the :checked pseudo-state in
	 * QSS, so mirror the state into a "checked" class (same approach as the
	 * mute/monitor buttons) and re-polish to apply the green styling. */
	utils->toggleClass(noiseSupprButton, "checked", active);
	style()->polish(noiseSupprButton);
}

void VolumeControl::obsFilterChanged(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<VolumeControl *>(data), "updateNoiseSupprButton",
				  Qt::QueuedConnection);
}

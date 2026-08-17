/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

extern bool opt_minimize_tray;

void OBSBasic::SystemTrayInit()
{
#ifdef __APPLE__
	QIcon trayIconFile = QIcon(":/res/images/obs_macos.svg");
	trayIconFile.setIsMask(true);
#else
	QIcon trayIconFile = QIcon(":/res/images/obs.png");
#endif
	trayIcon = new QSystemTrayIcon(QIcon::fromTheme("obs-tray", trayIconFile), this);
	trayIcon->setToolTip("OBS Studio");

	trayMenu = new QMenu(this);

	showHide = new QAction(QTStr("Basic.SystemTray.Show"), trayMenu);
	sysTrayStream = new QAction(
		StreamingActive() ? QTStr("Basic.Main.StopStreaming") : QTStr("Basic.Main.StartStreaming"), trayMenu);
	sysTrayRecord = new QAction(
		RecordingActive() ? QTStr("Basic.Main.StopRecording") : QTStr("Basic.Main.StartRecording"), trayMenu);
	sysTrayReplayBuffer = new QAction(ReplayBufferActive() ? QTStr("Basic.Main.StopReplayBuffer")
							       : QTStr("Basic.Main.StartReplayBuffer"),
					  trayMenu);
	sysTrayVirtualCam = new QAction(VirtualCamActive() ? QTStr("Basic.Main.StopVirtualCam")
							   : QTStr("Basic.Main.StartVirtualCam"),
					trayMenu);
	sysTrayStopAllAndClose = new QAction(QTStr("Basic.SystemTray.StopAllAndClose"), trayMenu);
	exit = new QAction(QTStr("Exit"), trayMenu);

	previewProjector = new QMenu(QTStr("Projector.Open.Preview"), trayMenu);
	studioProgramProjector = new QMenu(QTStr("Projector.Open.Program"), trayMenu);
	OBSBasic::updateSysTrayProjectorMenu();

	trayMenu->addAction(showHide);
	trayMenu->addSeparator();
	trayMenu->addMenu(previewProjector);
	trayMenu->addMenu(studioProgramProjector);
	trayMenu->addSeparator();
	trayMenu->addAction(sysTrayStream);
	trayMenu->addAction(sysTrayRecord);
	trayMenu->addAction(sysTrayReplayBuffer);
	trayMenu->addAction(sysTrayVirtualCam);
	trayMenu->addSeparator();
	trayMenu->addAction(sysTrayStopAllAndClose);
	trayMenu->addAction(exit);
	trayIcon->setContextMenu(trayMenu);
	trayIcon->show();

#ifdef _WIN32
	/* A duplicate launch attempt that auto-cancels itself unattended (see
	 * the "OBS is already running" prompt) signals this instance to stop
	 * any flashing health-alert tray icon — see SignalClearTrayAlert(). */
	auto *clearAlertPoll = new QTimer(this);
	connect(clearAlertPoll, &QTimer::timeout, this, [this]() {
		if (CheckAndClearTrayAlertSignal())
			StopTrayAlertFlash();
	});
	clearAlertPoll->start(1000);
#endif

	if (outputHandler && !outputHandler->replayBuffer)
		sysTrayReplayBuffer->setEnabled(false);

	sysTrayVirtualCam->setEnabled(vcamEnabled);

	if (Active())
		OnActivate(true);

	connect(trayIcon.data(), &QSystemTrayIcon::activated, this, &OBSBasic::IconActivated);
	connect(showHide, &QAction::triggered, this, &OBSBasic::ToggleShowHide);
	connect(sysTrayStream, &QAction::triggered, this, &OBSBasic::StreamActionTriggered);
	connect(sysTrayRecord, &QAction::triggered, this, &OBSBasic::RecordActionTriggered);
	connect(sysTrayReplayBuffer.data(), &QAction::triggered, this, &OBSBasic::ReplayBufferActionTriggered);
	connect(sysTrayVirtualCam.data(), &QAction::triggered, this, &OBSBasic::VirtualCamActionTriggered);
	connect(sysTrayStopAllAndClose.data(), &QAction::triggered, this, &OBSBasic::StopAllAndClose);
	connect(exit, &QAction::triggered, this, &OBSBasic::close);
}

void OBSBasic::IconActivated(QSystemTrayIcon::ActivationReason reason)
{
	/* interacting with the tray acknowledges a health alert */
	StopTrayAlertFlash();

	OBSBasic::updateSysTrayProjectorMenu();

#ifdef __APPLE__
	UNUSED_PARAMETER(reason);
#else
	if (reason == QSystemTrayIcon::Trigger) {
		EnablePreviewDisplay(previewEnabled && !isVisible());
		ToggleShowHide();
	}
#endif
}

void OBSBasic::SysTrayNotify(const QString &text, QSystemTrayIcon::MessageIcon n, const QString &title)
{
	/* SysTrayNotify() can silently do nothing if any of these are false, with
	 * no other indication anywhere that the notification never went out -
	 * log which case this was so a "why didn't I get a notification" report
	 * can actually be diagnosed from the log instead of guessed at. Note
	 * that even a successful showMessage() call only means Windows was
	 * *asked* to show a toast; OS-level suppression (Focus Assist, this
	 * app's notification permission being off in Windows Settings) happens
	 * below Qt and can't be detected or logged from here. */
	bool haveIcon = trayIcon;
	bool iconVisible = trayIcon && trayIcon->isVisible();
	bool platformSupports = QSystemTrayIcon::supportsMessages();

	if (haveIcon && iconVisible && platformSupports) {
		QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::MessageIcon(n);
		trayIcon->showMessage(title.isEmpty() ? QStringLiteral("OBS Studio") : title, text, icon, 10000);
		blog(LOG_INFO, "SysTrayNotify: notification sent to Windows: \"%s\"", QT_TO_UTF8(text));
	} else {
		blog(LOG_WARNING,
		     "SysTrayNotify: notification NOT sent (tray icon exists: %d, visible: %d, "
		     "platform supports messages: %d): \"%s\"",
		     haveIcon, iconVisible, platformSupports, QT_TO_UTF8(text));
	}
}

void OBSBasic::SystemTray(bool firstStarted)
{
	if (!QSystemTrayIcon::isSystemTrayAvailable())
		return;
	if (!trayIcon && !firstStarted)
		return;

	bool sysTrayWhenStarted = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayWhenStarted");
	bool sysTrayEnabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayEnabled");

	if (firstStarted)
		SystemTrayInit();

	if (!sysTrayEnabled) {
		trayIcon->hide();
	} else {
		trayIcon->show();
		if (firstStarted && (sysTrayWhenStarted || opt_minimize_tray)) {
			EnablePreviewDisplay(false);
#ifdef __APPLE__
			EnableOSXDockIcon(false);
#endif
			opt_minimize_tray = false;
		}
	}

	if (isVisible())
		showHide->setText(QTStr("Basic.SystemTray.Hide"));
	else
		showHide->setText(QTStr("Basic.SystemTray.Show"));
}

bool OBSBasic::sysTrayMinimizeToTray()
{
	return config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayMinimizeToTray");
}

void OBSBasic::StopAllAndClose()
{
	blog(LOG_INFO, "Tray: Stop All & Close triggered");

	if (outputHandler) {
		/* Streaming first: StopStreaming() already cascades into
		 * StopRecording()/StopReplayBuffer() itself when the "keep
		 * recording/replay buffer running after stream stops" settings
		 * are off, so calling it first avoids doing that work twice. */
		if (outputHandler->StreamingActive())
			StopStreaming();
		if (outputHandler->RecordingActive())
			StopRecording();
		if (outputHandler->replayBuffer && outputHandler->ReplayBufferActive())
			StopReplayBuffer();
	}

	closeWindow();
}

void OBSBasic::updateSysTrayProjectorMenu()
{
	previewProjector->clear();
	studioProgramProjector->clear();
	AddProjectorMenuMonitors(previewProjector, this, &OBSBasic::OpenPreviewProjector);
	previewProjector->addSeparator();
	previewProjector->addAction(QTStr("Projector.Window"), this, &OBSBasic::OpenPreviewWindow);
	AddProjectorMenuMonitors(studioProgramProjector, this, &OBSBasic::OpenStudioProgramProjector);
	studioProgramProjector->addSeparator();
	studioProgramProjector->addAction(QTStr("Projector.Window"), this, &OBSBasic::OpenStudioProgramWindow);
}

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

#include <qt-wrappers.hpp>

#include <QDir>

/* Audio counterpart of the per-source Output Visibility feature. The video
 * side renders separate stream/record mixes; audio separation works through
 * mixer tracks instead: the stream output consumes its configured track and
 * the recording is moved onto its own track, then each source's track mask
 * is set from its filter. Without this, "stream only" / "record only"
 * silently did nothing for audio — e.g. Desktop Audio (record-only) and a
 * game capture's audio (stream-only) both landed in the recording, doubling
 * the game sound with a capture-latency echo. */
void OBSBasic::UpdateAudioOutputFilterRouting()
{
	if (obs_output_filtered_source_count() == 0)
		return;

	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	bool adv = astrcmpi(mode, "Advanced") == 0;
	if (!adv) {
		blog(LOG_WARNING, "Audio output separation: sources have stream-only/record-only "
				  "filters, but audio separation requires Advanced output mode");
		return;
	}

	uint32_t streamTrack = (uint32_t)config_get_int(activeConfiguration, "AdvOut", "TrackIndex");
	if (streamTrack < 1 || streamTrack > MAX_AUDIO_MIXES)
		streamTrack = 1;
	uint32_t streamMask = 1u << (streamTrack - 1);

	/* the recording must consume a different track than the stream,
	 * otherwise per-source separation is impossible */
	uint32_t recTracks = (uint32_t)config_get_int(activeConfiguration, "AdvOut", "RecTracks");
	if (recTracks == 0 || (recTracks & streamMask) != 0) {
		uint32_t recTrack = (streamTrack == 1) ? 2 : 1;
		recTracks = 1u << (recTrack - 1);
		config_set_int(activeConfiguration, "AdvOut", "RecTracks", recTracks);
		config_save_safe(activeConfiguration, "tmp", nullptr);
		blog(LOG_INFO,
		     "Audio output separation: recording audio moved to track %u "
		     "(stream uses track %u)",
		     recTrack, streamTrack);
	}

	struct RouteCtx {
		uint32_t streamMask;
		uint32_t recMask;
	} ctx = {streamMask, recTracks};

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *c = static_cast<RouteCtx *>(param);

			if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;

			uint32_t mixers = obs_source_get_audio_mixers(source);
			uint32_t want;

			switch (obs_source_get_output_filter(source)) {
			case OBS_SOURCE_OUTPUT_FILTER_STREAM:
				want = c->streamMask;
				break;
			case OBS_SOURCE_OUTPUT_FILTER_RECORD:
				want = c->recMask;
				break;
			default:
				/* unfiltered: make sure it reaches both outputs,
				 * keeping any extra tracks the user assigned */
				want = mixers | c->streamMask | c->recMask;
				break;
			}

			if (want != mixers) {
				obs_source_set_audio_mixers(source, want);
				blog(LOG_INFO,
				     "Audio output separation: '%s' tracks 0x%X -> 0x%X",
				     obs_source_get_name(source), mixers, want);
			}
			return true;
		},
		&ctx);
}

void OBSBasic::ResetOutputs()
{
	ProfileScope("OBSBasic::ResetOutputs");

	UpdateAudioOutputFilterRouting();

	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	bool advOut = astrcmpi(mode, "Advanced") == 0;

	if ((!outputHandler || !outputHandler->Active()) &&
	    (!setupStreamingGuard.valid() ||
	     setupStreamingGuard.wait_for(std::chrono::seconds{0}) == std::future_status::ready)) {
		outputHandler.reset();
		outputHandler.reset(advOut ? CreateAdvancedOutputHandler(this) : CreateSimpleOutputHandler(this));

		if (!multiStreamOutput)
			multiStreamOutput = std::make_unique<MultiStreamOutput>(this);

		emit ReplayBufEnabled(outputHandler->replayBuffer);

		if (sysTrayReplayBuffer)
			sysTrayReplayBuffer->setEnabled(!!outputHandler->replayBuffer);

		UpdateIsRecordingPausable();
	} else {
		outputHandler->Update();
	}
}

bool OBSBasic::Active() const
{
	if (!outputHandler)
		return false;
	return outputHandler->Active();
}

void OBSBasic::ResizeOutputSizeOfSource()
{
	if (obs_video_active())
		return;

	QMessageBox resize_output(this);
	resize_output.setText(QTStr("ResizeOutputSizeOfSource.Text") + "\n\n" +
			      QTStr("ResizeOutputSizeOfSource.Continue"));
	QAbstractButton *Yes = resize_output.addButton(QTStr("Yes"), QMessageBox::YesRole);
	resize_output.addButton(QTStr("No"), QMessageBox::NoRole);
	resize_output.setIcon(QMessageBox::Warning);
	resize_output.setWindowTitle(QTStr("ResizeOutputSizeOfSource"));
	resize_output.exec();

	if (resize_output.clickedButton() != Yes)
		return;

	OBSSource source = obs_sceneitem_get_source(GetCurrentSceneItem());

	int width = obs_source_get_width(source);
	int height = obs_source_get_height(source);

	width = ((width + 3) / 4) * 4;   // Round width up to the nearest multiple of 4
	height = ((height + 1) / 2) * 2; // Round height up to the nearest multiple of 2

	config_set_uint(activeConfiguration, "Video", "BaseCX", width);
	config_set_uint(activeConfiguration, "Video", "BaseCY", height);
	config_set_uint(activeConfiguration, "Video", "OutputCX", width);
	config_set_uint(activeConfiguration, "Video", "OutputCY", height);

	ResetVideo();
	ResetOutputs();
	activeConfiguration.SaveSafe("tmp");
	on_actionFitToScreen_triggered();
}

const char *OBSBasic::GetCurrentOutputPath()
{
	const char *path = nullptr;
	const char *mode = config_get_string(Config(), "Output", "Mode");

	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");

		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			path = config_get_string(Config(), "AdvOut", "FFFilePath");
		} else {
			path = config_get_string(Config(), "AdvOut", "RecFilePath");
		}
	} else {
		path = config_get_string(Config(), "SimpleOutput", "FilePath");
	}

	return path;
}

void OBSBasic::OutputPathInvalidMessage()
{
	blog(LOG_ERROR, "Recording stopped because of bad output path");

	OBSMessageBox::critical(this, QTStr("Output.BadPath.Title"), QTStr("Output.BadPath.Text"));
}

bool OBSBasic::IsFFmpegOutputToURL() const
{
	const char *mode = config_get_string(Config(), "Output", "Mode");
	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");
		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			bool is_local = config_get_bool(Config(), "AdvOut", "FFOutputToFile");
			if (!is_local)
				return true;
		}
	}

	return false;
}

bool OBSBasic::OutputPathValid()
{
	if (IsFFmpegOutputToURL())
		return true;

	const char *path = GetCurrentOutputPath();
	return path && *path && QDir(path).exists();
}

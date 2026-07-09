#include "MultiStreamOutput.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs.h>
#include <util/config-file.h>

#include <algorithm>

const char *const MultiStreamOutput::PLATFORM_NAMES[MAX_DESTINATIONS] = {"YouTube", "Twitch", "TikTok"};

const char *const MultiStreamOutput::CONFIG_SECTIONS[MAX_DESTINATIONS] = {"MultiStreamYouTube", "MultiStreamTwitch",
									   "MultiStreamTikTok"};

const char *const MultiStreamOutput::DEFAULT_SERVERS[MAX_DESTINATIONS] = {
	"rtmp://a.rtmp.youtube.com/live2",
	"rtmp://live.twitch.tv/app",
	"rtmp://push.tiktok.com/live/",
};

MultiStreamOutput::MultiStreamOutput(OBSBasic *main_) : main(main_)
{
	for (size_t i = 0; i < MAX_DESTINATIONS; i++) {
		destinations[i].name = PLATFORM_NAMES[i];
		destinations[i].server = DEFAULT_SERVERS[i];
	}
}

MultiStreamOutput::~MultiStreamOutput()
{
	/* Member destructors (for the OBSAutoRelease fields in each
	 * MultiStreamDestination) run AFTER this body, so the encoder that
	 * reads from croppedVideo is still alive here — release it (and its
	 * output/service) explicitly first, matching the assumption already
	 * relied on elsewhere in this codebase (e.g. BasicOutputHandler's own
	 * destructor) that outputs are fully stopped by the time these owning
	 * objects are torn down. */
	for (auto &dest : destinations) {
		dest.output = nullptr;
		dest.service = nullptr;
		dest.encoder = nullptr;
		if (dest.croppedVideo) {
			obs_remove_video_mix(dest.croppedVideo);
			dest.croppedVideo = nullptr;
		}
	}
}

void MultiStreamOutput::LoadConfig()
{
	config_t *config = main->Config();

	for (size_t i = 0; i < MAX_DESTINATIONS; i++) {
		const char *section = CONFIG_SECTIONS[i];

		destinations[i].enabled = config_get_bool(config, section, "Enabled");

		const char *server = config_get_string(config, section, "Server");
		destinations[i].server = (server && *server) ? server : DEFAULT_SERVERS[i];

		const char *key = config_get_string(config, section, "Key");
		destinations[i].key = key ? key : "";

		destinations[i].customVideoSettings = config_get_bool(config, section, "VideoOverrideEnabled");
		destinations[i].width = (int)config_get_int(config, section, "Width");
		destinations[i].height = (int)config_get_int(config, section, "Height");
		destinations[i].bitrate = (int)config_get_int(config, section, "Bitrate");
	}
}

/* Builds a dedicated encoder for a destination that has custom video
 * settings, cloning the primary encoder's type and settings so anything
 * the user didn't override (e.g. codec, preset) still matches the primary
 * stream. Returns nullptr (falling back to the shared primary encoder) if
 * the clone fails for any reason.
 *
 * When a custom resolution is set, the encoder is fed from a dedicated
 * cropped video mix (dest.croppedVideo) rather than the main video — this
 * center-crops the canvas to the destination's aspect ratio before scaling,
 * so e.g. a vertical resolution pulled from a horizontal canvas crops to
 * fill instead of squashing/stretching the picture. */
static OBSEncoderAutoRelease CreateDestinationEncoder(MultiStreamDestination &dest, obs_encoder_t *primaryEncoder)
{
	const char *encoderId = obs_encoder_get_id(primaryEncoder);
	if (!encoderId) {
		blog(LOG_WARNING, "MultiStream: Could not determine primary encoder type for %s", dest.name.c_str());
		return nullptr;
	}

	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease primarySettings = obs_encoder_get_settings(primaryEncoder);
	if (primarySettings)
		obs_data_apply(settings, primarySettings);

	if (dest.bitrate > 0) {
		obs_data_set_int(settings, "bitrate", dest.bitrate);
		obs_data_set_int(settings, "buffer_size", dest.bitrate);
	}

	std::string encoderName = "multistream_video_" + dest.name;
	OBSEncoderAutoRelease encoder = obs_video_encoder_create(encoderId, encoderName.c_str(), settings, nullptr);
	if (!encoder) {
		blog(LOG_WARNING, "MultiStream: Failed to create custom video encoder for %s", dest.name.c_str());
		return nullptr;
	}

	if (dest.width > 0 && dest.height > 0) {
		dest.croppedVideo = obs_add_cropped_scaled_mix((uint32_t)dest.width, (uint32_t)dest.height,
								OBS_SOURCE_OUTPUT_FILTER_STREAM);
		if (dest.croppedVideo) {
			obs_encoder_set_video(encoder, dest.croppedVideo);
		} else {
			blog(LOG_WARNING, "MultiStream: Failed to create cropped mix for %s, falling back to stretch",
			     dest.name.c_str());
			obs_encoder_set_video(encoder, obs_get_video());
			obs_encoder_set_scaled_size(encoder, dest.width, dest.height);
		}
	} else {
		obs_encoder_set_video(encoder, obs_get_video());
	}

	return encoder;
}

bool MultiStreamOutput::StartDestination(MultiStreamDestination &dest, obs_encoder_t *videoEncoder,
					 obs_encoder_t *audioEncoder)
{
	if (!dest.enabled || dest.key.empty()) {
		blog(LOG_INFO, "MultiStream: Skipping %s (disabled or no key)", dest.name.c_str());
		return false;
	}

	OBSDataAutoRelease serviceSettings = obs_data_create();
	obs_data_set_string(serviceSettings, "server", dest.server.c_str());
	obs_data_set_string(serviceSettings, "key", dest.key.c_str());

	std::string serviceName = "multistream_service_" + dest.name;
	dest.service = obs_service_create("rtmp_custom", serviceName.c_str(), serviceSettings, nullptr);
	if (!dest.service) {
		blog(LOG_WARNING, "MultiStream: Failed to create service for %s", dest.name.c_str());
		return false;
	}

	std::string outputName = "multistream_output_" + dest.name;
	dest.output = obs_output_create("rtmp_output", outputName.c_str(), nullptr, nullptr);
	if (!dest.output) {
		blog(LOG_WARNING, "MultiStream: Failed to create output for %s", dest.name.c_str());
		dest.service = nullptr;
		return false;
	}

	dest.encoder = nullptr;
	if (dest.customVideoSettings)
		dest.encoder = CreateDestinationEncoder(dest, videoEncoder);

	obs_output_set_video_encoder(dest.output, dest.encoder ? dest.encoder.Get() : videoEncoder);
	obs_output_set_audio_encoder(dest.output, audioEncoder, 0);
	obs_output_set_service(dest.output, dest.service);
	obs_output_set_reconnect_settings(dest.output, 20, 2);

	if (!obs_output_start(dest.output)) {
		const char *error = obs_output_get_last_error(dest.output);
		blog(LOG_WARNING, "MultiStream: Failed to start output for %s: %s", dest.name.c_str(),
		     error ? error : "Unknown error");
		dest.output = nullptr;
		dest.service = nullptr;
		dest.encoder = nullptr;
		if (dest.croppedVideo) {
			obs_remove_video_mix(dest.croppedVideo);
			dest.croppedVideo = nullptr;
		}
		return false;
	}

	blog(LOG_INFO, "MultiStream: Started streaming to %s (%s)", dest.name.c_str(), dest.server.c_str());
	return true;
}

MultiStreamStartResult MultiStreamOutput::Start(obs_encoder_t *videoEncoder, obs_encoder_t *audioEncoder,
						const std::string &primaryServer, const std::string &primaryKey)
{
	LoadConfig();

	MultiStreamStartResult result;

	/* Track stream keys already in use so the same destination is never
	 * started twice — this is what prevents "streaming to YouTube twice"
	 * when YouTube is also the main service. */
	std::vector<std::string> usedKeys;
	if (!primaryKey.empty())
		usedKeys.push_back(primaryKey);

	for (auto &dest : destinations) {
		/* Release any output left over from a previous session before
		 * deciding what to do this time. obs_output_stop() is async, so
		 * the old object may have lingered after the last Stop(); by the
		 * time the user starts again it has finished and is safe to free.
		 */
		dest.output = nullptr;
		dest.service = nullptr;
		dest.encoder = nullptr;
		if (dest.croppedVideo) {
			obs_remove_video_mix(dest.croppedVideo);
			dest.croppedVideo = nullptr;
		}

		if (!dest.enabled)
			continue;

		if (dest.key.empty()) {
			result.missingKey.push_back(dest.name);
			continue;
		}

		bool duplicate = std::find(usedKeys.begin(), usedKeys.end(), dest.key) != usedKeys.end();
		if (duplicate) {
			blog(LOG_INFO, "MultiStream: Skipping %s (same stream key as another destination)",
			     dest.name.c_str());
			result.duplicate.push_back(dest.name);
			continue;
		}

		if (StartDestination(dest, videoEncoder, audioEncoder)) {
			result.started.push_back(dest.name);
			usedKeys.push_back(dest.key);
		} else {
			result.failed.push_back(dest.name);
		}
	}

	return result;
}

void MultiStreamOutput::Stop(bool force)
{
	/* Signal each output to stop but keep our reference alive.
	 * obs_output_stop() is asynchronous; releasing the output here would
	 * tear it down mid-shutdown and cut the final packets. The references
	 * are released on the next Start() (or when this object is destroyed). */
	for (auto &dest : destinations) {
		if (dest.output) {
			if (force)
				obs_output_force_stop(dest.output);
			else
				obs_output_stop(dest.output);
		}
	}
}

bool MultiStreamOutput::AnyActive() const
{
	for (const auto &dest : destinations) {
		if (dest.output && obs_output_active(dest.output))
			return true;
	}
	return false;
}

bool MultiStreamOutput::AnyEnabled()
{
	LoadConfig();
	for (const auto &dest : destinations) {
		if (dest.enabled)
			return true;
	}
	return false;
}

#include "MultiStreamOutput.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs.h>
#include <util/config-file.h>

const char *const MultiStreamOutput::PLATFORM_NAMES[MAX_DESTINATIONS] = {"Twitch", "TikTok"};

const char *const MultiStreamOutput::CONFIG_SECTIONS[MAX_DESTINATIONS] = {"MultiStreamTwitch", "MultiStreamTikTok"};

const char *const MultiStreamOutput::DEFAULT_SERVERS[MAX_DESTINATIONS] = {
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
	/* Force-stop and release any active secondary outputs while libobs is
	 * still alive. This must happen before obs_shutdown(), which is why
	 * OBSBasic explicitly resets this object during applicationShutdown(). */
	Stop(true);
}

void MultiStreamOutput::LoadConfig()
{
	config_t *config = main->Config();

	for (size_t i = 0; i < MAX_DESTINATIONS; i++) {
		const char *section = CONFIG_SECTIONS[i];

		destinations[i].enabled = config_get_bool(config, section, "Enabled");
		destinations[i].server = DEFAULT_SERVERS[i];

		const char *key = config_get_string(config, section, "Key");
		destinations[i].key = key ? key : "";
	}
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

	obs_output_set_video_encoder(dest.output, videoEncoder);
	obs_output_set_audio_encoder(dest.output, audioEncoder, 0);
	obs_output_set_service(dest.output, dest.service);
	obs_output_set_reconnect_settings(dest.output, 20, 2);

	if (!obs_output_start(dest.output)) {
		const char *error = obs_output_get_last_error(dest.output);
		blog(LOG_WARNING, "MultiStream: Failed to start output for %s: %s", dest.name.c_str(),
		     error ? error : "Unknown error");
		dest.output = nullptr;
		dest.service = nullptr;
		return false;
	}

	blog(LOG_INFO, "MultiStream: Started streaming to %s (%s)", dest.name.c_str(), dest.server.c_str());
	return true;
}

bool MultiStreamOutput::Start(obs_encoder_t *videoEncoder, obs_encoder_t *audioEncoder)
{
	/* Defensive: tear down any outputs left over from a previous session
	 * (e.g. a reconnect that re-entered Start without an intervening Stop)
	 * so we never orphan a running output by overwriting its handle. */
	Stop(true);

	LoadConfig();

	bool anyStarted = false;
	for (auto &dest : destinations) {
		if (dest.enabled) {
			bool ok = StartDestination(dest, videoEncoder, audioEncoder);
			anyStarted = anyStarted || ok;
		}
	}
	return anyStarted;
}

void MultiStreamOutput::Stop(bool force)
{
	for (auto &dest : destinations) {
		if (dest.output) {
			if (force)
				obs_output_force_stop(dest.output);
			else
				obs_output_stop(dest.output);
			dest.output = nullptr;
		}
		dest.service = nullptr;
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

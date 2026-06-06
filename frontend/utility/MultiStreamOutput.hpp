#pragma once

#include <obs.hpp>

#include <array>
#include <string>

class OBSBasic;

struct MultiStreamDestination {
	std::string name;
	std::string server;
	std::string key;
	bool enabled = false;

	OBSOutputAutoRelease output;
	OBSServiceAutoRelease service;
};

class MultiStreamOutput {
public:
	static constexpr size_t MAX_DESTINATIONS = 3;

	static const char *const PLATFORM_NAMES[MAX_DESTINATIONS];
	static const char *const CONFIG_SECTIONS[MAX_DESTINATIONS];
	static const char *const DEFAULT_SERVERS[MAX_DESTINATIONS];

	explicit MultiStreamOutput(OBSBasic *main);
	~MultiStreamOutput() = default;

	void LoadConfig();
	bool Start(obs_encoder_t *videoEncoder, obs_encoder_t *audioEncoder);
	void Stop(bool force = false);
	bool AnyActive() const;

	std::array<MultiStreamDestination, MAX_DESTINATIONS> destinations;

private:
	OBSBasic *main;

	bool StartDestination(MultiStreamDestination &dest, obs_encoder_t *videoEncoder,
			      obs_encoder_t *audioEncoder);
};

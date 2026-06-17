#pragma once

#include <obs.hpp>

#include <array>
#include <string>
#include <vector>

class OBSBasic;

struct MultiStreamDestination {
	std::string name;
	std::string server;
	std::string key;
	bool enabled = false;

	OBSOutputAutoRelease output;
	OBSServiceAutoRelease service;
};

/* Outcome of a Start() call, so the caller can tell the user exactly what
 * happened with each enabled destination instead of failing silently. */
struct MultiStreamStartResult {
	std::vector<std::string> started;   // connecting now
	std::vector<std::string> failed;    // had a key but obs_output_start() failed
	std::vector<std::string> missingKey; // enabled but no stream key entered
	std::vector<std::string> duplicate; // same stream key as the main stream / another dest
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

	/* primaryServer/primaryKey identify the main stream so we never
	 * accidentally stream to the same destination twice. */
	MultiStreamStartResult Start(obs_encoder_t *videoEncoder, obs_encoder_t *audioEncoder,
				     const std::string &primaryServer, const std::string &primaryKey);
	void Stop(bool force = false);
	bool AnyActive() const;
	bool AnyEnabled();

	std::array<MultiStreamDestination, MAX_DESTINATIONS> destinations;

private:
	OBSBasic *main;

	bool StartDestination(MultiStreamDestination &dest, obs_encoder_t *videoEncoder,
			      obs_encoder_t *audioEncoder);
};

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

	/* Video settings for this destination. When customVideoSettings is
	 * false, the destination shares the primary stream's encoder as-is.
	 * When true, width/height/bitrate (any of which may be 0 to mean
	 * "keep the primary's value") are applied to a dedicated encoder
	 * created just for this destination. */
	bool customVideoSettings = false;
	int width = 0;
	int height = 0;
	int bitrate = 0;

	OBSOutputAutoRelease output;
	OBSServiceAutoRelease service;
	/* Only set when customVideoSettings is in effect; kept alive here
	 * since obs_output_set_video_encoder() does not take ownership. */
	OBSEncoderAutoRelease encoder;
	/* Only set when customVideoSettings is in effect: a dedicated video
	 * mix that center-crops the canvas to this destination's aspect ratio
	 * before scaling, so the custom resolution crops to fill rather than
	 * stretching/distorting. Released via obs_remove_video_mix(). */
	video_t *croppedVideo = nullptr;
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
	~MultiStreamOutput();

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

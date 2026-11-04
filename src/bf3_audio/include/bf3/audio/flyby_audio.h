#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/present/presentation_events.h"
#include "bf3/present/presented.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::audio {

struct FlyBySound {
	std::shared_ptr<const core::audio::SoundProgram> program;
	float radius_m = 0.0f;
	float min_speed = 0.0f;
};

inline constexpr uint32_t kFlyByOrdinal = 0x80000000u;
inline constexpr size_t kMaxFlyByTracks = 4 * present::kMaxPresentedBullets;
inline constexpr uint32_t kFlyByTrackFrames = 30;
inline constexpr size_t kFlyByOwnHits = 64;

struct FlyByAudioStats {
	uint64_t bullets = 0;
	uint64_t silent = 0;
	uint64_t starts = 0;
	uint64_t slow = 0;
	uint64_t receding = 0;
	uint64_t stopped = 0;
	uint64_t own_hits = 0;
	uint64_t refused = 0;
	uint64_t untracked = 0;
	float farthest_start_m = 0.0f;
};
static_assert(std::is_trivially_copyable_v<FlyByAudioStats>);

float closest_fraction(const Vec3 &p_from, const Vec3 &p_to, const Vec3 &p_point);

class FlyByAudio {
public:
	FlyByAudio(MixerCore &r_mixer, std::vector<FlyBySound> p_sounds,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, OneShotPool &r_pool);
	~FlyByAudio();
	FlyByAudio(const FlyByAudio &) = delete;
	FlyByAudio &operator=(const FlyByAudio &) = delete;

	void set_listener(const ListenerState &p_listener);
	void on_events(std::span<const present::PresentEvent> p_events);
	void on_bullets(std::span<const present::PresentedBullet> p_bullets, uint64_t p_match_seed, SimMixerClock &r_clock);
	const FlyByAudioStats &stats() const;
	size_t tracked() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}

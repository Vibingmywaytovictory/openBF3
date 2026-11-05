#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/gameplay/soldier_sound.h"
#include "bf3/sim/soldier_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::audio {

inline constexpr size_t kMaxFootstepSoldiers = 128;
inline constexpr size_t kFootstepMaterialIds = 16;
inline constexpr size_t kMaxFootstepTags = 4;
inline constexpr uint8_t kFootPlantLeftBit = 1u << 0;
inline constexpr uint8_t kFootPlantRightBit = 1u << 1;
inline constexpr uint8_t kFootPlantsAnimatedBit = 1u << 2;

struct FootstepParams {
	float walk_threshold = 0.0f;
	float run_threshold = 0.0f;
	float walk_period = 0.0f;
	float run_period = 0.0f;
	float start_above = 0.0f;
	float stop_at_or_below = 0.0f;
	float speed_scale = 0.0f;
	uint32_t speed_input = 0;
	float radius_m = 0.0f;
	std::array<core::gameplay::SoldierAntStep, core::gameplay::kMaxSoldierAntSteps> ant_steps{};
	uint8_t ant_step_count = 0;
	bool local_player_closes_period = false;
};

float audible_radius(const core::audio::SoundProgram &p_program);

FootstepParams footstep_params(
		const core::gameplay::SoldierSoundDef &p_def, const core::audio::SoundProgram &p_footsteps);

enum class FootstepEvent : uint8_t { Start, Stop, Jump, Land, Left, Right, EnterAir, Animated };
inline constexpr size_t kFootstepEventCount = 8;

uint32_t footstep_event_hash(FootstepEvent p_event);
bool footstep_plant(FootstepEvent p_event);

std::optional<uint32_t> animated_step_event(const FootstepParams &p_params, int32_t p_tag);

struct FootstepSoldier {
	uint8_t slot = 0;
	uint8_t generation = 0;
	Vec3 position;
	Vec3 velocity;
	uint8_t pose = 0;
	sim::MoveMode mode = sim::MoveMode::OnGround;
	bool first_person = false;
	bool local_player = false;
	bool animated = false;
	uint8_t plants = 0;
	uint8_t tag_count = 0;
	std::array<int32_t, kMaxFootstepTags> tags{};
};
static_assert(std::is_trivially_copyable_v<FootstepSoldier>);

struct FootstepPost {
	uint8_t slot = 0;
	FootstepEvent event = FootstepEvent::Left;
	uint32_t hash = 0;
	float speed = 0.0f;
	float distance_m = 0.0f;
	bool local_period = false;
	bool animated_plant = false;
};
static_assert(std::is_trivially_copyable_v<FootstepPost>);

class FootstepScheduler {
public:
	explicit FootstepScheduler(const FootstepParams &p_params);

	void step(std::span<const FootstepSoldier> p_soldiers, const Vec3 &p_listener, double p_dt,
			std::vector<FootstepPost> &r_out);
	size_t tracked() const { return tracks_.size(); }
	uint64_t unknown_tags() const { return unknown_tags_; }
	const FootstepParams &params() const { return params_; }

private:
	struct Track {
		uint16_t key = 0;
		uint64_t seen = 0;
		double phase = 0.0;
		float last_speed = 0.0f;
		bool moving = false;
		bool left_next = true;
		sim::MoveMode mode = sim::MoveMode::OnGround;
	};
	FootstepParams params_;
	std::vector<Track> tracks_;
	uint64_t frame_ = 0;
	uint64_t unknown_tags_ = 0;
};

struct FootstepStats {
	uint64_t frames = 0;
	std::array<uint64_t, kFootstepEventCount> posts{};
	uint64_t refused = 0;
	uint64_t no_ground = 0;
	uint64_t local_period = 0;
	uint64_t animated_plants = 0;
	uint64_t unknown_tags = 0;
	uint64_t joined = 0;
	std::array<uint64_t, kFootstepMaterialIds + 1> by_material{};
	uint32_t tracked = 0;
	uint32_t peak_tracked = 0;
	float farthest_m = 0.0f;
};
static_assert(std::is_trivially_copyable_v<FootstepStats>);

using FootstepGround = std::function<std::optional<float>(const Vec3 &p_feet)>;

class FootstepDriver {
public:
	FootstepDriver(MixerCore &r_mixer, std::shared_ptr<const core::audio::SoundProgram> p_program,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, OneShotPool &r_pool,
			const FootstepParams &p_params, FootstepGround p_ground);
	~FootstepDriver();
	FootstepDriver(const FootstepDriver &) = delete;
	FootstepDriver &operator=(const FootstepDriver &) = delete;

	void step(std::span<const FootstepSoldier> p_soldiers, const ListenerState &p_listener, double p_dt,
			uint64_t p_frame);
	const FootstepStats &stats() const;
	std::span<const FootstepPost> last_posts() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}

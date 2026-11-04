#pragma once

#include "bf3/audio/weapon_audio.h"
#include "bf3/fx/triggers.h"
#include "bf3/net/protocol.h"
#include "bf3/present/presentation_events.h"
#include "bf3/present/presented.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace bf3::godot_side {

inline constexpr double kLateJoinLoopWindowS = 0.5;
inline constexpr double kLateJoinPlaySeconds = 6.0;
inline constexpr double kLateJoinEorSeconds = 20.0;
inline constexpr double kLateJoinFirstSnapshotS = 180.0;
inline constexpr double kLateJoinPresentS = 60.0;
inline constexpr uint32_t kLateJoinDetonationKey = 0x40000000u;
inline constexpr size_t kLateJoinMaxNotes = 24;

enum class LateJoinMode : uint8_t { InRound, RoundOver };

struct LateJoinFrame {
	double now_s = 0.0;
	sim::Tick first_snapshot_tick = 0;
	bool match_valid = false;
	net::MatchPhase phase = net::MatchPhase::Loading;
	uint8_t own_slot = 0xFF;
	std::span<const present::PresentEvent> events;
	std::span<const present::PresentedOtherWeapon> others;
	std::span<const fx::TriggerStart> effect_starts;
	std::span<const audio::WeaponSoundStart> sound_starts;
	bool frame_valid = false;
	fx::FxTriggerStats effects{};
	uint64_t router_stale_dropped = 0;
	uint64_t impact_stale_dropped = 0;
	uint32_t weapon_voices = 0;
	bool eor_visible = false;
	std::span<const std::string> flow_log;
	std::span<const std::string> screens_log;
};

class LateJoinCheck {
public:
	explicit LateJoinCheck(LateJoinMode p_mode) : mode_(p_mode) {}
	static std::optional<LateJoinMode> parse_mode(const std::string &p_text);

	bool frame(const LateJoinFrame &p_frame);
	int exit_code() const { return failure_.empty() ? 0 : 1; }
	std::string report() const;
	bool new_effect_frame(uint64_t p_frames);
	std::vector<audio::WeaponSoundStart> &sound_buffer() { return sounds_; }

private:
	using EventKey = std::tuple<uint8_t, uint8_t, uint32_t>;
	using SoundKey = std::pair<uint8_t, uint32_t>;
	using BurstKey = std::pair<uint8_t, uint16_t>;

	void note_events(const LateJoinFrame &p_frame);
	void note_starts(const LateJoinFrame &p_frame);
	void note_join(const LateJoinFrame &p_frame);
	void note_logs(const LateJoinFrame &p_frame);
	bool verdict();
	bool in_loop_window(double p_now_s) const;
	void fail(std::string p_why);

	LateJoinMode mode_;
	double first_frame_s_ = -1.0;
	double join_s_ = -1.0;
	double present_s_ = -1.0;
	sim::Tick join_tick_ = 0;
	net::MatchPhase join_phase_ = net::MatchPhase::Loading;
	bool join_phase_valid_ = false;
	std::set<EventKey> stale_events_;
	std::set<SoundKey> stale_sounds_;
	std::set<BurstKey> stale_bursts_;
	std::set<BurstKey> previous_stale_bursts_;
	std::map<uint8_t, double> firing_at_join_;
	std::map<uint8_t, double> fire_sound_s_;
	std::vector<uint8_t> silent_;
	uint64_t render_events_ = 0;
	uint64_t stale_count_ = 0;
	uint64_t effect_starts_ = 0;
	uint64_t stale_effect_starts_ = 0;
	uint64_t sound_starts_ = 0;
	uint64_t stale_sound_starts_ = 0;
	uint64_t stale_burst_starts_ = 0;
	fx::FxTriggerStats effects_{};
	bool frame_valid_at_join_ = false;
	size_t others_at_join_ = 0;
	std::set<uint8_t> firing_seen_;
	uint64_t state_bursts_ = 0;
	uint64_t remote_bursts_ = 0;
	uint32_t window_voices_ = 0;
	uint64_t router_stale_dropped_ = 0;
	uint64_t impact_stale_dropped_ = 0;
	double eor_after_s_ = -1.0;
	uint64_t effect_frames_ = 0;
	std::vector<audio::WeaponSoundStart> sounds_;
	std::vector<std::string> notes_;
	bool done_ = false;
	std::string failure_;
};

}

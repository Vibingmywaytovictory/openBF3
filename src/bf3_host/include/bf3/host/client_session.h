#pragma once

#include "bf3/core/result.h"
#include "bf3/host/level_load.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/soldier_state.h"

#include "bf3/host/client_projectiles.h"
#include "bf3/host/kits.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/present/presented.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::host {

class ClientTransport;

enum class PoseRequest : uint8_t {
	Keep = 0,
	Set = 1,
};

struct InputSample {
	float move_forward = 0.0f;
	float move_right = 0.0f;
	float yaw = 0.0f;
	float pitch = 0.0f;
	uint32_t held_buttons = 0;
	uint32_t jump_presses = 0;
	PoseRequest pose_request = PoseRequest::Keep;
	sim::Pose pose = sim::Pose::Stand;
	bool free_camera = false;

	uint32_t life = 0;

	uint32_t fire_presses = 0;
	bool view_valid = false;
	uint8_t view_quality = 0;
	uint16_t view_seq = 0;
	double view_render_ticks = 0.0;
};

struct ClientSessionConfig {
	std::string host = "127.0.0.1";
	uint16_t port = 0;
	std::string name = "player";
	std::optional<uint64_t> game_build;
	net::NetSimConfig netsim;
	float interp_min_ms = 75.0f;
	float interp_max_ms = 150.0f;
	float extrapolate_max_ms = 250.0f;
	float error_decay_ms = 100.0f;
	float snap_below_m = 0.0025f;
	float teleport_above_m = 2.0f;
	float position_epsilon_m = 1e-4f;
	float velocity_epsilon = 1e-3f;

	uint32_t target_buffer_ticks = 2;
	uint32_t max_buffer_ticks = 6;
	float target_hold_s = 2.0f;
	float max_clock_adjust = 0.05f;
	float clock_time_constant_s = 1.5f;
	float clock_deadband_ticks = 0.5f;
	float clock_reset_ms = 250.0f;
	uint32_t prediction_ring = 128;
	uint32_t connect_timeout_ms = 5000;
	bool request_dev_commands = false;
	bool debug_hash_compare = false;

	int32_t initial_clock_offset_ticks = 0;

	std::string mechanics_cfg = "config/weapon_mechanics.cfg";

	std::shared_ptr<const struct ClientGunplayInputs> gunplay;
	bool gunplay_from_level = true;

	int32_t own_test_max_rewind_ticks = -1;
	bool record_predicted = false;
	bool send_kit_select = false;
	net::KitSelect kit_select;

	uint32_t peer_timeout_min_ms = 0;
	uint32_t peer_timeout_max_ms = 0;
};

using present::PresentedSoldier;

struct ClientStats {
	bool connected = false;
	bool spawned = false;
	uint8_t slot = 0;
	sim::Tick client_tick = 0;
	sim::Tick last_server_tick = 0;
	double rtt_ms = 0.0;
	double rtt_min_ms = 0.0;
	double enet_rtt_ms = 0.0;
	int32_t command_buffer_ticks = 0;
	uint32_t target_buffer_ticks = 0;
	float clock_adjust = 0.0f;
	uint64_t clock_resets = 0;
	uint64_t motor_hash_mismatch = 0;
	uint64_t snapshots_received = 0;
	uint64_t snapshots_lost = 0;
	uint64_t snapshots_late = 0;
	uint64_t commands_sent = 0;
	uint64_t reconciliations = 0;
	sim::Tick last_reconcile_tick = 0;
	uint64_t last_reconcile_ns = 0;
	uint64_t ring_misses = 0;
	uint64_t spawns = 0;
	uint64_t replayed_ticks = 0;
	uint64_t base_fills = 0;
	uint64_t forced_forward = 0;
	uint64_t step_precondition_failures = 0;
	uint64_t corrections_over_1mm = 0;
	float max_correction_m = 0.0f;
	uint64_t teleports = 0;
	uint64_t hash_mismatches = 0;
	double interp_delay_ms = 0.0;
	net::NetSimStats netsim_out;
	net::NetSimStats netsim_in;
	std::array<char, 128> last_error{};

	bool refused = false;
	bool transport_disconnected = false;
	bool gunplay = false;
	uint64_t weapon_hash_mismatch = 0;
	uint64_t kit_hash_mismatch = 0;
	uint64_t weapon_reconciliations = 0;
	uint64_t weapon_ring_misses = 0;
	uint64_t health_rebases = 0;
	uint64_t predicted_shots = 0;
	uint64_t predicted_timed = 0;
	uint64_t no_view_shots = 0;
	uint64_t snapshot_others_received = 0;
	uint64_t remote_shots = 0;
	uint64_t combat_hits = 0, combat_deaths = 0, combat_detonations = 0;
	uint64_t bad_gunplay_datagrams = 0;
	uint64_t bad_weapon_wires = 0;
	uint64_t bad_gunplay_events = 0;
	uint64_t late_sample = 0;
	uint64_t generation_reorder = 0;
	uint64_t others_ahead = 0;
	uint64_t track_timeout = 0;
	uint64_t events_overflow = 0;
	sim::WeaponLaneStats lane;
	ClientProjectilesStats projectiles;

	uint64_t match_states = 0;
	uint64_t player_lists = 0;
	uint64_t soldier_gears = 0;
	uint64_t revives = 0;
	uint64_t bad_tdm_datagrams = 0;
	uint64_t restricted_commands = 0;
	uint64_t unspawned_ticks = 0;
	uint64_t spawn_requests = 0;
	uint64_t own_shot_id_resets = 0;
	bool rules_mismatch = false;
	uint64_t logic_full_syncs = 0;
	uint64_t logic_deltas = 0;
	uint64_t logic_events = 0;
	uint64_t logic_stale = 0;
	uint64_t logic_early_events = 0;
	uint64_t logic_dropped_events = 0;
	uint64_t bad_logic_datagrams = 0;

	bool kicked = false;
	net::KickReason kick_reason = net::KickReason::Protocol;
	std::array<char, 128> kick_detail{};
	uint8_t mismatch_components = 0;
};
static_assert(std::is_trivially_copyable_v<ClientStats>);

struct ClientLogicFeed {
	uint64_t ghost_revision = 0;
	std::optional<sim::logic::LogicGhost> ghost;
	std::vector<sim::logic::LogicNetworkedEvent> events;
};

inline constexpr size_t kOtherSamples = 3;
using OtherSample = net::TimedOther;
struct OtherTrack {
	uint8_t slot = 0;
	uint8_t count = 0;
	std::array<OtherSample, kOtherSamples> samples{};
	sim::Tick track_newest_tick = 0;
};

inline constexpr std::array<net::SoldierGear, net::kMaxSlots> kNoGear = [] {
	std::array<net::SoldierGear, net::kMaxSlots> a{};
	for (size_t i = 0; i < a.size(); ++i) {
		a[i].slot = uint8_t(i);
	}
	return a;
}();

struct PresentationFrame {
	uint64_t serial = 0;
	bool valid = false;
	sim::Tick tick = 0;
	double tick_fraction = 0.0;
	uint64_t publish_ns = 0;

	double own_ticks_per_ns = double(sim::kSimHz) * 1e-9;
	sim::SoldierState own_prev;
	sim::SoldierState own_curr;
	sim::Command own_command;
	sim::Vec3 visual_offset{};
	float eye_height_prev = 1.6f;
	float eye_height_curr = 1.6f;

	bool server_own_valid = false;
	sim::SoldierState server_own;

	uint16_t server_events = 0;
	uint32_t events = 0;

	double render_ticks_at_publish = 0.0;
	double ticks_per_ns = double(sim::kSimHz) * 1e-9;
	float extrapolate_max_ms = 250.0f;

	std::array<float, sim::kPoseCount> pose_eye_heights{ 1.6f, 1.0f, 0.3f };
	uint32_t other_count = 0;
	std::array<OtherTrack, net::kMaxSlots> others{};
	ClientStats stats;

	uint16_t newest_seq = 0;

	bool own_weapons_valid = false;
	net::WeaponsWire own_weapons_prev;
	net::WeaponsWire own_weapons_curr;
	uint64_t match_seed = 0;
	sim::Tick newest_tick = 0;
	uint8_t own_kit = 0;
	uint8_t own_team = 0;

	bool match_valid = false;
	net::MatchState match;
	uint8_t player_count = 0;
	std::array<net::PlayerRow, net::kMaxPlayerRows> players{};
	sim::Tick own_respawn_tick = 0;
	sim::Tick own_revive_until = 0;
	bool own_unspawned = false;
	std::array<net::SoldierGear, net::kMaxSlots> gear = kNoGear;
};
static_assert(std::is_trivially_copyable_v<PresentationFrame>);

size_t present_others(const PresentationFrame &p_frame, uint64_t p_now_ns, std::span<PresentedSoldier> r_out, uint64_t *r_extrapolated = nullptr);

double present_own_fraction(const PresentationFrame &p_frame, uint64_t p_now_ns);

class ClientSession {
public:
	ClientSession();
	~ClientSession();
	ClientSession(const ClientSession &) = delete;
	ClientSession &operator=(const ClientSession &) = delete;

	core::Status start(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level);
	core::Status start(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level,
			std::unique_ptr<ClientTransport> p_transport);
	core::Status attach_level(std::shared_ptr<const SharedLevel> p_level);
	bool lobby_match(net::MatchInfo &r_match) const;
	void stop();
	bool running() const;

	void set_input(const InputSample &p_input);

	bool read_presentation(PresentationFrame &r_frame) const;
	ClientStats stats() const;

	void send_debug(const net::DebugRequest &p_request);
	bool take_debug_report(net::DebugReport &r_report);

	void debug_shift_clock(int32_t p_ticks);
	void debug_stall(uint32_t p_ms);

	PresentationEvents &events();
	void take_predicted(std::vector<struct PredictedShot> &r_shots, std::vector<struct PredictedTimed> &r_timed);
	void take_own_test_samples(std::vector<OwnTestSample> &r_samples);
	void take_logic(ClientLogicFeed &r_feed);

	void send_kit_select(const net::KitSelect &p_select);

	void send_spawn_request(uint8_t p_kit);
	void send_suicide_request();
	void send_loadout_request(const net::LoadoutRequest &p_request);

	static size_t stop_all();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct ClientGunplayInputs {
	std::shared_ptr<const sim::weapons::WeaponTable> table;
	std::shared_ptr<const core::gameplay::CapsulePoseTable> capsules;
	KitTable kits;
	net::ShotQuantization shot_frame;

	std::function<core::Result<sim::weapons::Mechanics>(const std::string &p_path)> load_mechanics;
};

core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> build_client_weapon_table(const ClientGunplayInputs &p_inputs,
		const std::string &p_mechanics_cfg, uint8_t p_weapon_flags);

uint64_t client_weapon_hash(const sim::weapons::WeaponTable &p_table, const core::gameplay::CapsulePoseTable &p_capsules);

core::Result<std::shared_ptr<const ClientGunplayInputs>> client_gunplay_inputs(const SharedLevel &p_level);

struct PredictedShot {
	sim::Tick tick = 0;
	sim::weapons::ShotEvent shot;
};
struct PredictedTimed {
	sim::Tick tick = 0;
	sim::weapons::TimedWeaponEvent event;
};

struct PresentOthersCounts {
	uint64_t extrapolated = 0;
	uint64_t window_extrapolated = 0;
	uint64_t held = 0;
};
size_t present_others(const PresentationFrame &p_frame, uint64_t p_now_ns, std::span<PresentedSoldier> r_out, PresentOthersCounts &r_counts);

double present_render_ticks(const PresentationFrame &p_frame, uint64_t p_now_ns);

sim::ViewQuality present_view_quality(const PresentationFrame &p_frame, double p_render_ticks);

void set_view_sample(InputSample &r_input, const PresentationFrame &p_frame, uint64_t p_now_ns);

}

namespace bf3::host {

class OwnShotIds {
public:
	static constexpr size_t kSegments = 16;
	static constexpr size_t kRecent = 256;
	uint32_t emit(uint32_t p_shot_index);
	uint32_t lookup(uint32_t p_shot_index) const;
	uint64_t resets() const { return resets_; }
	size_t segments() const { return segment_count_; }

private:
	struct Segment {
		uint32_t from = 0;
		uint32_t offset = 0;
	};
	struct Pair {
		uint32_t real = 0;
		uint32_t presented = 0;
	};
	std::array<Segment, kSegments> segments_{};
	size_t segment_count_ = 0;
	std::array<Pair, kRecent> recent_{};
	size_t recent_next_ = 0;
	size_t recent_count_ = 0;
	bool any_ = false;
	uint32_t last_real_ = 0;
	uint64_t next_presented_ = 0;
	uint64_t resets_ = 0;
};

bool present_own_shot_id(OwnShotIds &r_ids, uint8_t p_own_slot, PresentEvent &r_event);

struct PlayerTable {
	std::array<net::PlayerRow, net::kMaxPlayerRows> rows{};
	std::array<bool, net::kMaxPlayerRows> present{};
};
void apply_player_list(PlayerTable &r_table, const net::PlayerList &p_list);

uint8_t player_rows(const PlayerTable &p_table, std::array<net::PlayerRow, net::kMaxPlayerRows> &r_rows);

constexpr bool own_unspawned_at(bool p_match_valid, const net::MatchState &p_match, sim::Tick p_life_spawn_tick, sim::Tick p_tick) {
	return p_match_valid && p_match.unspawn_tick != 0 && p_life_spawn_tick < p_match.unspawn_tick && p_tick >= p_match.unspawn_tick;
}

constexpr bool match_end_of_round_at(bool p_match_valid, const net::MatchState &p_match, sim::Tick p_tick) {
	return p_match_valid && p_match.round_end_tick != 0 && p_match.unspawn_tick >= p_match.round_end_tick &&
			p_tick >= p_match.unspawn_tick;
}

bool present_own_alive(const PresentationFrame &p_frame);

const char *rules_refusal(const net::MatchInfo &p_match, bool p_has_logic, uint64_t p_logic_digest);
std::string level_refusal(const net::MatchInfo &p_match, std::string_view p_level, std::string_view p_mode);
}

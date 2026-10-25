#pragma once

#include "bf3/core/cache/digest.h"
#include "bf3/core/result.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::net {

inline constexpr uint16_t kProtocolVersion = 3;
inline constexpr uint8_t kChannelUnreliable = 0;
inline constexpr uint8_t kChannelReliable = 1;
inline constexpr uint8_t kChannelBulk = 2;
inline constexpr size_t kChannelCount = 3;
inline constexpr size_t kMaxDatagramBytes = 1200;
inline constexpr size_t kCommandsPerPacket = 4;
inline constexpr size_t kMaxSlots = 128;
inline constexpr size_t kMaxNameBytes = 32;

inline constexpr uint8_t kMaxKits = 4;
inline constexpr uint8_t kMaxTeam = 2;
inline constexpr uint16_t kMaxWireDefs = 255;
inline constexpr size_t kMaxTargetRows = 128;

inline constexpr size_t kMaxDebugReportBytes = 2 * 1024 * 1024;

enum class MessageType : uint8_t {

	Snapshot = 1,
	CommandPacket = 2,

	Hello = 16,
	MatchInfo = 17,
	Ready = 18,
	Spawned = 19,
	Loading = 20,
	Kick = 21,
	Bye = 22,
	Debug = 30,
	DebugReport = 31,

	SnapshotOthers = 3,
	ShotEvents = 4,
	CombatEvents = 23,
	KitSelect = 24,

	MatchState = 25,
	PlayerList = 26,
	SpawnRequest = 27,
};

std::optional<MessageType> peek_type(std::span<const uint8_t> p_datagram);

struct Hello {
	uint16_t protocol = kProtocolVersion;
	std::string name;
};

enum class MatchPhase : uint8_t { Loading = 0, PreRound = 1, InRound = 2, RoundOver = 3, Restarting = 4 };

struct MatchInfo {
	uint16_t protocol = kProtocolVersion;
	std::string level;
	std::string mode;
	uint16_t sim_hz = uint16_t(sim::kSimHz);
	uint8_t snapshot_divisor = 2;
	uint8_t slot = 0;
	sim::Tick server_tick = 0;
	MatchPhase phase = MatchPhase::InRound;
	std::optional<core::Sha256Digest> source_digest;

	uint64_t motor_hash = 0;

	uint64_t match_seed = 0;
	uint64_t weapon_hash = 0;
	uint8_t weapon_flags = 0;
	uint64_t kit_hash = 0;
	uint8_t max_rewind_ticks = 0;

	uint64_t logic_digest = 0;
	uint8_t rules_flags = 0;
};

namespace weapon_flags {
inline constexpr uint8_t kExactAim = 1u << 0;
inline constexpr uint8_t kInfiniteMagazine = 1u << 1;
}

struct Ready {
	std::optional<core::Sha256Digest> source_digest;
};

struct Spawned {
	uint8_t slot = 0;
	uint32_t spawn_index = 0;
	sim::SoldierState state;

	uint8_t kit = 0;
	uint8_t team = 1;

	bool revived = false;
};

struct Loading {
	uint8_t stage = 0;
	uint16_t permille = 0;
};

struct Kick {
	std::string reason;
};

enum class DebugKind : uint8_t {

	Teleport = 1,
	SpawnAt = 2,

	SpawnSweep = 3,
	SetSeed = 4,

	Trace = 5,

	SpawnTarget = 6,

	TargetInfo = 7,
};
inline constexpr uint32_t kMaxTraceTicks = 1200;

struct DebugRequest {
	DebugKind kind = DebugKind::Teleport;
	uint32_t id = 0;
	sim::Vec3 position{};
	uint16_t yaw = 0;
	uint32_t index = 0;
	uint32_t ticks = 60;

	uint8_t slot = 0;
	sim::Pose pose = sim::Pose::Stand;
};

struct TargetInfoRow {
	uint8_t slot = 0;
	sim::Pose pose = sim::Pose::Stand;
	sim::Vec3 feet{};
	uint16_t yaw = 0;
	sim::weapons::HitTarget target;
};

inline constexpr size_t kMaxTraceShots = 8;
inline constexpr size_t kMaxTraceProjectiles = 8;
inline constexpr size_t kMaxTraceDamage = 8;
inline constexpr size_t kMaxTraceDetonations = 2;
inline constexpr size_t kMaxTraceDeaths = 4;
struct TraceDeath {
	uint8_t victim = 0;
	uint8_t killer = 0;
	uint32_t shot_index = 0;
};
struct TraceGunplayTick {
	sim::Tick tick = 0;
	std::vector<sim::weapons::ShotEvent> shots;
	std::vector<sim::Vec3> projectiles;
	std::vector<uint32_t> projectile_shots;
	std::vector<sim::weapons::DamageEvent> damage;
	std::vector<sim::weapons::DetonationEvent> detonations;
	std::vector<TraceDeath> deaths;
};

struct SpawnSweepRow {
	uint32_t index = 0;
	bool ok = false;
	sim::Vec3 point{};
	sim::Vec3 settled{};
	float lift = 0, shift = 0, gap = 0;
	bool grounded = false, overlapping = false;
};

struct DebugReport {
	uint32_t id = 0;
	bool ok = false;
	std::string text;
	std::vector<SpawnSweepRow> sweep;
	std::vector<sim::SoldierState> trace;

	std::vector<TargetInfoRow> targets;
	std::vector<TraceGunplayTick> trace_gunplay;
	uint32_t trace_dropped = 0;
};

std::vector<uint8_t> encode_hello(const Hello &p_message);
std::vector<uint8_t> encode_match_info(const MatchInfo &p_message);
std::vector<uint8_t> encode_ready(const Ready &p_message);
std::vector<uint8_t> encode_spawned(const Spawned &p_message);
std::vector<uint8_t> encode_loading(const Loading &p_message);
std::vector<uint8_t> encode_kick(const Kick &p_message);
std::vector<uint8_t> encode_bye();
std::vector<uint8_t> encode_debug(const DebugRequest &p_message);
std::vector<uint8_t> encode_debug_report(const DebugReport &p_message);
core::Result<Hello> decode_hello(std::span<const uint8_t> p_datagram);
core::Result<MatchInfo> decode_match_info(std::span<const uint8_t> p_datagram);
core::Result<Ready> decode_ready(std::span<const uint8_t> p_datagram);
core::Result<Spawned> decode_spawned(std::span<const uint8_t> p_datagram);
core::Result<Loading> decode_loading(std::span<const uint8_t> p_datagram);
core::Result<Kick> decode_kick(std::span<const uint8_t> p_datagram);
core::Result<DebugRequest> decode_debug(std::span<const uint8_t> p_datagram);
core::Result<DebugReport> decode_debug_report(std::span<const uint8_t> p_datagram);

struct CommandPacket {
	uint16_t newest_snapshot = 0;
	uint32_t snapshot_ack_bits = 0;
	std::array<sim::Command, kCommandsPerPacket> commands{};
	uint8_t count = 0;

	uint32_t others_ack_bits = 0;
};

bool snapshot_acked(const CommandPacket &p_packet, uint16_t p_seq);
bool others_acked(const CommandPacket &p_packet, uint16_t p_seq);
std::vector<uint8_t> encode_command_packet(const CommandPacket &p_packet);
core::Result<CommandPacket> decode_command_packet(std::span<const uint8_t> p_datagram);

struct OtherSoldier {
	uint8_t slot = 0;
	uint8_t generation = 0;
	sim::Vec3 position{};
	sim::Vec3 velocity{};
	uint16_t yaw = 0;
	int16_t pitch = 0;
	sim::Pose pose = sim::Pose::Stand;
	sim::Pose target_pose = sim::Pose::Stand;
	uint8_t transition = 0;
	sim::MoveMode mode = sim::MoveMode::OnGround;
	uint8_t flags = 0;

	uint8_t weapon = 0xFF;
	uint8_t weapon_phase = 0;
	bool firing = false;
	bool ads = false;
	uint8_t reload_seq = 0;
	uint8_t burst_seq = 0;

	uint8_t team = 0;

	bool operator==(const OtherSoldier &) const = default;
};

OtherSoldier quantize_other(uint8_t p_slot, const sim::SoldierState &p_state);

void set_other_weapon(OtherSoldier &r_other, uint16_t p_def, sim::weapons::WeaponPhase p_phase, bool p_firing, bool p_ads, uint32_t p_reload_count,
		uint32_t p_burst_count);

void set_other_team(OtherSoldier &r_other, uint8_t p_team);

struct Snapshot {
	uint16_t seq = 0;
	sim::Tick server_tick = 0;
	uint32_t applied_sequence = 0;
	bool applied_repeated = false;
	int8_t command_buffer_ticks = 0;
	uint16_t sequence_echo = 0;
	uint16_t echo_hold_100us = 0;
	uint16_t own_events = 0;
	std::optional<sim::SoldierState> own;
	std::vector<OtherSoldier> others;

	std::optional<WeaponsWire> own_weapons;
};

std::vector<uint8_t> encode_snapshot(const Snapshot &p_snapshot, size_t *r_dropped = nullptr);

constexpr bool seq_newer(uint16_t p_a, uint16_t p_b) {
	return p_a != p_b && uint16_t(p_a - p_b) < 0x8000u;
}
core::Result<Snapshot> decode_snapshot(std::span<const uint8_t> p_datagram);

struct SnapshotOthers {
	uint16_t seq = 0;
	sim::Tick server_tick = 0;
	std::vector<OtherSoldier> others;
};
std::vector<uint8_t> encode_snapshot_others(const SnapshotOthers &p_message, size_t *r_dropped = nullptr);
core::Result<SnapshotOthers> decode_snapshot_others(std::span<const uint8_t> p_datagram);

struct RemoteShot {
	uint8_t slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = sim::weapons::kNoWeapon;
	sim::weapons::SimTime time = 0;
	sim::Vec3 origin{};
	sim::Vec3 direction{};
	uint8_t pellets = 1;
	bool tracer = false;
	sim::Vec3 aim{};
	float pellet_cone = 0;
	float power = 1;
};
inline constexpr size_t kMaxShotsPerDatagram = 128;
struct ShotEvents {
	sim::Tick server_tick = 0;
	std::vector<RemoteShot> shots;
};

struct ShotQuantization {
	sim::Vec3 origin_min{};
};

inline constexpr uint32_t kMaxShotTicksBack = 7;
std::vector<uint8_t> encode_shot_events(const ShotEvents &p_message, const ShotQuantization &p_frame, const sim::Vec3 &p_receiver_eye,
		size_t *r_dropped = nullptr);
core::Result<ShotEvents> decode_shot_events(std::span<const uint8_t> p_datagram, const ShotQuantization &p_frame);

std::optional<RemoteShot> quantize_remote_shot(const RemoteShot &p_shot, const ShotQuantization &p_frame, sim::Tick p_server_tick);

RemoteShot remote_shot_of(const sim::weapons::ShotEvent &p_shot);

struct HitEvent {
	uint8_t victim = 0;
	uint8_t shooter = 0;
	uint32_t shot_index = 0;
	uint8_t pellet = 0;
	sim::weapons::HitZone zone = sim::weapons::HitZone::Body;
	float damage = 0;
	float health_after = 0;
	sim::Vec3 position{};
	sim::Vec3 normal{};
	sim::weapons::SimTime time = 0;
};
struct DeathEvent {
	uint8_t victim = 0;
	uint8_t killer = 0;
	uint16_t def = sim::weapons::kNoWeapon;
	sim::weapons::HitZone zone = sim::weapons::HitZone::Body;
	uint32_t shot_index = 0;
	bool headshot = false;
	sim::weapons::SimTime time = 0;

	sim::Tick respawn_tick = 0;
	sim::Tick revive_until = 0;
	float killer_health = 0;
};

struct DetonationWire {
	uint8_t shooter = 0;
	uint32_t shot_index = 0;
	uint16_t def = sim::weapons::kNoWeapon;
	sim::weapons::SimTime time = 0;
	sim::Vec3 position{};
	sim::Vec3 normal{};
	bool dud = false;
};

struct ReviveEvent {
	uint8_t reviver = 0;
	uint8_t revived = 0;
	sim::Tick tick = 0;
};
inline constexpr size_t kMaxCombatEventsPerMessage = 256;
struct CombatEvents {
	sim::Tick server_tick = 0;
	std::vector<HitEvent> hits;
	std::vector<DeathEvent> deaths;
	std::vector<DetonationWire> detonations;
	std::vector<ReviveEvent> revives;
};

std::vector<uint8_t> encode_combat_events(const CombatEvents &p_message, const ShotQuantization &p_frame, size_t *r_clamped = nullptr);
core::Result<CombatEvents> decode_combat_events(std::span<const uint8_t> p_datagram, const ShotQuantization &p_frame);

struct KitSelect {
	uint8_t kit = 0;
	uint8_t team = 0;
};
std::vector<uint8_t> encode_kit_select(const KitSelect &p_message);
core::Result<KitSelect> decode_kit_select(std::span<const uint8_t> p_datagram);

namespace rules_flags {
inline constexpr uint8_t kTdm = 1u << 0;
inline constexpr uint8_t kDeployScreen = 1u << 1;
inline constexpr uint8_t kRevive = 1u << 2;
}

namespace restrict_bits {
inline constexpr uint32_t kThrottle = 1u << 0;
inline constexpr uint32_t kStrafe = 1u << 1;
inline constexpr uint32_t kYaw = 1u << 2;
inline constexpr uint32_t kPitch = 1u << 3;
inline constexpr uint32_t kJump = 1u << 4;
inline constexpr uint32_t kCrouch = 1u << 5;
inline constexpr uint32_t kProne = 1u << 6;
inline constexpr uint32_t kFire = 1u << 7;
inline constexpr uint32_t kZoom = 1u << 8;
inline constexpr uint32_t kReload = 1u << 9;
inline constexpr uint32_t kSprint = 1u << 10;
inline constexpr uint32_t kMelee = 1u << 11;
inline constexpr uint32_t kThrowGrenade = 1u << 12;
inline constexpr uint32_t kAll = (1u << 13) - 1;
}

constexpr sim::Command restrict_command(sim::Command p_command, sim::Tick p_restrict_until_tick, uint32_t p_allowed) {
	if (p_restrict_until_tick == 0 || p_command.tick >= p_restrict_until_tick) {
		return p_command;
	}
	if ((p_allowed & restrict_bits::kThrottle) == 0) {
		p_command.move_forward = 0;
	}
	if ((p_allowed & restrict_bits::kStrafe) == 0) {
		p_command.move_right = 0;
	}
	uint32_t refused = 0;
	if ((p_allowed & restrict_bits::kJump) == 0) {
		refused |= sim::buttons::kJump;
	}
	if ((p_allowed & restrict_bits::kFire) == 0) {
		refused |= sim::buttons::kFire;
	}
	if ((p_allowed & restrict_bits::kZoom) == 0) {
		refused |= sim::buttons::kZoom;
	}
	if ((p_allowed & restrict_bits::kReload) == 0) {
		refused |= sim::buttons::kReload;
	}
	if ((p_allowed & restrict_bits::kSprint) == 0) {
		refused |= sim::buttons::kSprint;
	}
	p_command.buttons &= ~refused;
	return p_command;
}

struct MatchState {
	MatchPhase phase = MatchPhase::Loading;
	sim::Tick round_start_tick = 0;
	sim::Tick countdown_end_tick = 0;
	std::array<int32_t, 2> kills{};
	int32_t kill_limit = 0;
	uint8_t winner = 0;
	bool killer_camera = true;
	sim::Tick round_end_tick = 0;
	sim::Tick unspawn_tick = 0;
	sim::Tick restart_tick = 0;
	sim::Tick restrict_until_tick = 0;
	uint32_t restrict_mask = restrict_bits::kAll;
	uint16_t players_needed = 0;
};
static_assert(std::is_trivially_copyable_v<MatchState>);

inline constexpr size_t kMaxPlayerRows = 64;
inline constexpr size_t kMaxPlayerNameBytes = 16;
inline constexpr uint32_t kPlayerListIntervalTicks = 15;

struct PlayerRow {
	uint8_t slot = 0;
	uint8_t team = 0;
	uint8_t kit = 0;
	bool bot = false;
	bool alive = false;
	uint8_t name_bytes = 0;
	uint16_t ping_ms = 0;
	uint16_t kills = 0;
	uint16_t deaths = 0;
	int32_t score = 0;
	std::array<char, kMaxPlayerNameBytes> name{};
};
static_assert(std::is_trivially_copyable_v<PlayerRow>);
struct PlayerList {
	sim::Tick server_tick = 0;
	bool full = false;
	std::vector<PlayerRow> rows;
	std::vector<uint8_t> removed;
};

struct SpawnRequest {
	uint8_t kit = 0;
	uint8_t spawn_choice = 0;
};

std::vector<uint8_t> encode_match_state(const MatchState &p_message);
core::Result<MatchState> decode_match_state(std::span<const uint8_t> p_datagram);

std::vector<uint8_t> encode_player_list(const PlayerList &p_message, size_t *r_clamped = nullptr);
core::Result<PlayerList> decode_player_list(std::span<const uint8_t> p_datagram);
std::vector<uint8_t> encode_spawn_request(const SpawnRequest &p_message);
core::Result<SpawnRequest> decode_spawn_request(std::span<const uint8_t> p_datagram);

bool set_player_name(PlayerRow &r_row, std::string_view p_name);
std::string_view player_name(const PlayerRow &p_row);

}

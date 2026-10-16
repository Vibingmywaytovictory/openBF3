#pragma once
// The M1 wire protocol (plan §2.7, §3.5; topics/05 §4.1-§4.2, §7.1-§7.2, §7.7; docs/design/m1.md §7). Godot-free.
// The messages travel over the existing ENet path (enet_host.h): in M1 the in-process client connects to the
// listen server on 127.0.0.1; M3 binds the same server to a public address and nothing here changes.
//
// Channels (topics/05 §7.1):
//   0 unreliable, unsequenced  (ENET_PACKET_FLAG_UNSEQUENCED: enet_host.cpp sends flag 0 = sequenced today; T3
//                              adds the unsequenced send mode, docs §7.1) S->C Snapshot (one datagram, <= kMaxDatagramBytes); C->S CommandPacket (60 Hz,
//                              the last kCommandsPerPacket commands, so a lost datagram loses no command)
//   1 reliable, ordered        control: Hello, MatchInfo, Ready, Spawned, Loading, Kick, Debug*, Bye
//   2 reliable, ordered        bulk (M3+: FullSync of destruction rows and the logic ghost); unused in M1
// Every datagram starts with a u8 message type, then the message's bit-packed body (bit_stream.h). Decoders take
// bytes from the network: they bound every count, check every enum and fail on a short or over-long body.

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
#include <vector>

namespace bf3::net {

inline constexpr uint16_t kProtocolVersion = 1; // bump on any wire change
inline constexpr uint8_t kChannelUnreliable = 0;
inline constexpr uint8_t kChannelReliable = 1;
inline constexpr uint8_t kChannelBulk = 2;
inline constexpr size_t kChannelCount = 3;
inline constexpr size_t kMaxDatagramBytes = 1200; // below ENet's 1392 MTU: no unreliable fragmentation
inline constexpr size_t kCommandsPerPacket = 4; // newest first (topics/05 §4.1: 3-4 redundant)
inline constexpr size_t kMaxSlots = 128; // soldier slots a snapshot can name (64 players + walkers / bots)
inline constexpr size_t kMaxNameBytes = 32;

enum class MessageType : uint8_t {
	// ch0
	Snapshot = 1,
	CommandPacket = 2,
	// ch1 control
	Hello = 16, // C->S
	MatchInfo = 17, // S->C
	Ready = 18, // C->S: loaded, spawn me
	Spawned = 19, // S->C: the client's soldier exists (slot, life, full state)
	Loading = 20, // C->S: load stage / progress (M3 slot hold; M1 sends it only when connected while loading)
	Kick = 21, // S->C: reason text, then the server disconnects
	Bye = 22, // either way: orderly leave
	Debug = 30, // C->S: dev commands (teleport, spawn at, spawn sweep); refused unless the server allows them
	DebugReport = 31, // S->C: the answer to a Debug request
	// M2a protocol v2 (docs/design/m2a.md §7; declared in W2.0, wire formats and codecs are nt's). The names are the
	// contract; nt may renumber the values before its merge.
	SnapshotOthers = 3, // ch0 S->C: the others that did not fit the Snapshot of the same seq (§7.3)
	ShotEvents = 4, // ch0 S->C: remote shots since the previous snapshot (§7.4)
	CombatEvents = 23, // ch1 S->C: hits, deaths, detonations (§7.4)
	KitSelect = 24, // ch1 C->S: kit and team for the next spawn (§5.6)
};
// The type byte of a datagram, or nullopt when it is empty or unknown.
std::optional<MessageType> peek_type(std::span<const uint8_t> p_datagram);

// ---- control (ch1) ---------------------------------------------------------------------------------------------

struct Hello {
	uint16_t protocol = kProtocolVersion;
	std::string name; // <= kMaxNameBytes UTF-8
};

enum class MatchPhase : uint8_t { Loading = 0, PreRound = 1, InRound = 2, RoundOver = 3 };

struct MatchInfo {
	uint16_t protocol = kProtocolVersion;
	std::string level; // MP_Subway
	std::string mode; // TeamDeathMatch0
	uint16_t sim_hz = uint16_t(sim::kSimHz);
	uint8_t snapshot_divisor = 2; // snapshots every 2nd tick = 30 Hz
	uint8_t slot = 0; // the client's soldier slot
	sim::Tick server_tick = 0; // at send time (the client's first clock estimate)
	MatchPhase phase = MatchPhase::InRound;
	std::optional<core::Sha256Digest> source_digest; // level source digest (level/source_digest.h); M3 compares it
	// sim::motor_params_hash of the server's MotorParams (SoldierDef numbers + MotorTuning). A client whose own
	// hash differs would mispredict every tick: M1 logs it, M3 kicks (docs §7.2).
	uint64_t motor_hash = 0;
	// M2a v2 (docs/design/m2a.md §4.4, §5.5, §7.1). For weapon_hash and kit_hash the client disconnects with a reason
	// on receiving MatchInfo, before Ready ("weapon table mismatch"; cl).
	uint64_t match_seed = 0; // splitmix64(GameServerConfig::seed ^ level hash): shot seeds, bot brains, cosmetics
	uint64_t weapon_hash = 0; // weapon_table_hash (table + mechanics + exact_aim / infinite_magazine) ^ capsule table hash
	uint8_t weapon_flags = 0; // weapon_flags:: the match options; the client builds its table with them
	uint64_t kit_hash = 0; // host::kit_table_hash
	uint8_t max_rewind_ticks = 0; // host::LagCompConfig::max_rewind_ticks (21 = 350 ms, m2a.md §16 Q1)
};
// M2a: MatchInfo::weapon_flags bits (docs/design/m2a.md §7.1).
namespace weapon_flags {
inline constexpr uint8_t kExactAim = 1u << 0;
inline constexpr uint8_t kInfiniteMagazine = 1u << 1;
} // namespace weapon_flags

struct Ready {
	std::optional<core::Sha256Digest> source_digest;
};

// The client's soldier exists. `state` is stamped at the server tick S of the spawn (state.tick == S, full spawn
// protection, sim::stamp_spawn). From S + 1 until the first real command of the client arrives, the server steps
// the slot with repeated_command(sim::hold_command(state), tick) (idle, spawn yaw, pitch 0, Stand, kRepeated); the
// client's catch-up (docs §9.2) steps the same commands from S up to its own clock and writes those ticks into its
// prediction ring, so the first snapshots compare equal.
struct Spawned {
	uint8_t slot = 0;
	uint32_t spawn_index = 0;
	sim::SoldierState state; // full precision, as the own-state block
	// M2a v2 (docs/design/m2a.md §5.6, §7.4): the life's kit (host::KitRole) and team (1, 2); the client calls
	// spawn_soldier_weapons with them and keeps the spawn tick (state.tick) for step_weapons_m2a.
	uint8_t kit = 0;
	uint8_t team = 1;
};

struct Loading {
	uint8_t stage = 0; // bf3::host::LoadStage
	uint16_t permille = 0;
};

struct Kick {
	std::string reason;
};

enum class DebugKind : uint8_t {
	// Put the soldier at position/yaw: a fresh standing state, velocity 0, same life, protect_ticks 0 (a debug
	// placement is not a spawn, so the E1d fall-damage drops are not masked by spawn protection, docs §6.8).
	// Applied in the server's rules step of tick N and stamped N, like a spawn (game_server.h).
	Teleport = 1,
	SpawnAt = 2, // respawn the soldier at spawn point `index` (settle included)
	// One idle dummy per spawn point, placed with sim::place_spawn, stepped over `ticks` live server ticks with
	// the idle command, judged with sim::judge_settle (exactly settle_spawn's pieces, so the rows equal the
	// headless sim.motor rows). Runs in batches of the free slots; dummies never count for choose_spawn.
	SpawnSweep = 3,
	SetSeed = 4, // reseed the server's spawn choice
	// Record the requesting client's soldier for `ticks` server ticks (<= kMaxTraceTicks), starting with the tick
	// after the request is handled: one authoritative SoldierState per tick, returned in DebugReport::trace when
	// done. E1 through the full path is measured on it (every tick, not the 30 Hz snapshots; docs §14).
	Trace = 5,
	// M2a v2 (docs/design/m2a.md §7.4, §10.1; dev only; the server's handling is sv's). Place target bot `slot` at
	// position / yaw in `pose` (applied in the rules step, clears spawn protection; GameServer::place_soldier's code).
	SpawnTarget = 6,
	// Report target bot `slot`'s server capsules (DebugReport::targets, one TargetInfoRow).
	TargetInfo = 7,
};
inline constexpr uint32_t kMaxTraceTicks = 1200; // 20 s; ~92 KiB on ch1 (reliable, fragments)

struct DebugRequest {
	DebugKind kind = DebugKind::Teleport;
	uint32_t id = 0; // echoed in the report
	sim::Vec3 position{};
	uint16_t yaw = 0;
	uint32_t index = 0;
	uint32_t ticks = 60;
	// M2a v2: SpawnTarget / TargetInfo.
	uint8_t slot = 0;
	sim::Pose pose = sim::Pose::Stand;
};

// M2a v2: TargetInfo's answer (docs/design/m2a.md §7.4): a target's server pose and the capsules the server tests.
struct TargetInfoRow {
	uint8_t slot = 0;
	sim::Pose pose = sim::Pose::Stand;
	sim::Vec3 feet{};
	uint16_t yaw = 0;
	sim::weapons::HitTarget target; // the capsules as the server's provider answers for the present tick
};

// M2a v2: the gunplay part of one traced tick (docs/design/m2a.md §1 "Server trace"). Bounded per tick; the excess is
// dropped and counted in DebugReport::trace_dropped.
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
	std::vector<sim::weapons::ShotEvent> shots; // the traced shooter's
	std::vector<sim::Vec3> projectiles; // positions of the traced shooter's live projectiles at the tick's end
	std::vector<uint32_t> projectile_shots; // their shot indices (same order)
	std::vector<sim::weapons::DamageEvent> damage;
	std::vector<sim::weapons::DetonationEvent> detonations;
	std::vector<TraceDeath> deaths;
};

struct SpawnSweepRow {
	uint32_t index = 0;
	bool ok = false; // the server's settle verdict (sim spawn.h)
	sim::Vec3 point{};
	sim::Vec3 settled{}; // feet after the idle ticks
	float lift = 0, shift = 0, gap = 0;
	bool grounded = false, overlapping = false;
};

struct DebugReport {
	uint32_t id = 0;
	bool ok = false;
	std::string text; // human-readable result line
	std::vector<SpawnSweepRow> sweep; // SpawnSweep only; sent on ch1 (reliable, may fragment)
	std::vector<sim::SoldierState> trace; // Trace only: consecutive ticks, oldest first, full precision
	// M2a v2.
	std::vector<TargetInfoRow> targets; // TargetInfo only
	std::vector<TraceGunplayTick> trace_gunplay; // Trace only: one per traced tick, the same ticks as `trace`
	uint32_t trace_dropped = 0; // trace additions beyond the per-tick bounds
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

// ---- commands (ch0, C->S) -----------------------------------------------------------------------------------------
// Body: u16 newest snapshot seq received | u32 ack bits of the 32 before it | u8 count (1..4) | the newest
// command in full: u32 tick, u32 sequence, 24 buttons, 8+8 move, 16 yaw, 14 pitch, 2 pose; each older one as a
// delta: tick and sequence implied (-1 each), 1 bit "same as the newer" or the full fields above minus tick and
// sequence. The flags byte is server-side only and never sent. Size with the type byte: 26 bytes when the 3 older
// commands equal the newest, 53 bytes when all 4 differ. The encoder cuts count short at the first older command
// whose tick or sequence is not exactly one less (after a clock reset), because the decoder implies both.

struct CommandPacket {
	uint16_t newest_snapshot = 0;
	uint32_t snapshot_ack_bits = 0; // bit i = snapshot newest_snapshot - 1 - i arrived
	std::array<sim::Command, kCommandsPerPacket> commands{}; // [0] newest; consecutive ticks and sequences
	uint8_t count = 0;
	// M2a v2 (docs/design/m2a.md §7.2): bit i = the SnapshotOthers of seq newest_snapshot - i arrived. It differs from
	// snapshot_ack_bits by one ON PURPOSE: bit 0 is the newest seq itself, because the main Snapshot's arrival does not
	// imply its SnapshotOthers. newest_snapshot keeps M1's meaning (the newest Snapshot that arrived), so a
	// SnapshotOthers newer than it cannot be acked yet (the client counts others_ahead). Pinned by m2a.contract.
	uint32_t others_ack_bits = 0;
};
// M2a v2 (docs/design/m2a.md §7.2; nt defines them): the two ack conventions as predicates (the server's
// host::LagHistory::ack and the client use them). snapshot_acked: p_seq == newest_snapshot, or bit
// (newest_snapshot - 1 - p_seq) of snapshot_ack_bits; others_acked: bit (newest_snapshot - p_seq) of others_ack_bits.
// A seq newer than newest_snapshot (seq_newer), or beyond the 32 bits of its field, is not acked.
bool snapshot_acked(const CommandPacket &p_packet, uint16_t p_seq);
bool others_acked(const CommandPacket &p_packet, uint16_t p_seq);
std::vector<uint8_t> encode_command_packet(const CommandPacket &p_packet);
core::Result<CommandPacket> decode_command_packet(std::span<const uint8_t> p_datagram);

// ---- snapshots (ch0, S->C, every snapshot_divisor ticks) ---------------------------------------------------------
// Body: u16 seq | u32 server tick | u32 sequence of the command applied at the server tick to this client's
// soldier, 1 bit repeated | i8 command buffer ticks (tick of the highest-sequence command received - server
// tick; time sync §7.5)
// | u16 echo of the newest command sequence received | u16 echo hold (time from receiving that command to this
// send, 0.1 ms units, so RTT = now - send time of the echoed command - hold) | u16 own events (OR of the own
// soldier's soldier_events over every tick since the previous snapshot to this client: events of odd ticks are
// not lost) | own-state block | u8 others count | others.
// Own-state block: 1 bit present; then every SoldierState field at full precision (floats as f32 bit patterns,
// integers in their own widths, the reserved fields left out): 77 bytes. Header + own state ~96 bytes.
// Other soldier (M1: absolute, no baseline; M3 adds a 1-bit "delta vs acked baseline" in front without moving
// the rest): u8 slot | u8 life generation (life & 0xFF, wraps) | position 21+18+21 bits (quant::kPosition*) |
// velocity 3 x 11 bits | yaw 14 | pitch 12 | pose 2 | target pose 2 | transition progress 8 (0..255 of the total)
// | mode 4 | flags 8 (alive, grounded, sprinting): 159 bits, ~20 bytes, so ~55 others fit one datagram. A full
// 64-player snapshot (63 others, ~1,250 B) does not fit: M3 needs priority and baselines from its start.

struct OtherSoldier {
	uint8_t slot = 0;
	uint8_t generation = 0; // the low 8 bits of SoldierState::life (a respawn changes it; 256 lives wrap)
	sim::Vec3 position{}; // after quantisation (what the receiver gets)
	sim::Vec3 velocity{};
	uint16_t yaw = 0; // 16-bit Command scale, low 2 bits zero
	int16_t pitch = 0; // Command scale, low 2 bits zero
	sim::Pose pose = sim::Pose::Stand;
	sim::Pose target_pose = sim::Pose::Stand;
	uint8_t transition = 0; // 0 = start, 255 = done
	sim::MoveMode mode = sim::MoveMode::OnGround;
	uint8_t flags = 0; // low 8 bits of soldier_flags::
	// M2a v2 (docs/design/m2a.md §7.3): 19 more bits per other: the 3p weapon, firing, ADS, and reloads and bursts
	// as sequence counters, so a lost snapshot cannot hide one (the burst sequence is au's audio seed, §4.4).
	uint8_t weapon = 0xFF; // index into the match's def list (WeaponTable def id), 255 = none
	uint8_t weapon_phase = 0; // sim::weapons::WeaponPhase (3 bits)
	bool firing = false; // trigger down and a shot this interval
	bool ads = false;
	uint8_t reload_seq = 0; // 2 bits: counts reload starts
	uint8_t burst_seq = 0; // 4 bits: counts bursts
};
// The quantised view of a server state, exactly as the receiver decodes it (the server's interpolation tests
// compare against this).
OtherSoldier quantize_other(uint8_t p_slot, const sim::SoldierState &p_state);

// Invariant (docs §9.2): the snapshot is sent after server tick `server_tick` ran, so own->tick == server_tick,
// and the command applied to the own soldier at that tick is the client's command for that tick
// (applied_repeated false) or a repeat (applied_repeated true; applied_sequence is then the sequence of the command
// it repeats, 0 for the hold command after a spawn). The client keys its prediction ring by tick
// (SoldierState::tick == Command::tick), never by sequence. Decoders fail a snapshot whose own->tick differs.
struct Snapshot {
	uint16_t seq = 0;
	sim::Tick server_tick = 0;
	uint32_t applied_sequence = 0;
	bool applied_repeated = false;
	int8_t command_buffer_ticks = 0;
	uint16_t sequence_echo = 0;
	uint16_t echo_hold_100us = 0; // how long the server held the echoed command before this send (0.1 ms)
	uint16_t own_events = 0; // soldier_events:: of the own soldier since the previous snapshot to this client
	std::optional<sim::SoldierState> own; // the receiving client's soldier after server_tick
	std::vector<OtherSoldier> others; // every other live soldier, nearest first, as many as fit
	// M2a v2 (docs/design/m2a.md §7.3): the own soldier's weapons at full precision (weapons_wire.h), with `own`.
	std::optional<WeaponsWire> own_weapons;
};
// Fills at most kMaxDatagramBytes; others that do not fit are left out (r_dropped counts them). M1's soldier
// counts never come near the budget; M3 replaces the "nearest first" order with ghost priority.
// Sequence numbers (Snapshot::seq, CommandPacket::newest_snapshot) wrap at 2^16: compare with seq_newer.
std::vector<uint8_t> encode_snapshot(const Snapshot &p_snapshot, size_t *r_dropped = nullptr);
// Whether u16 sequence p_a is newer than p_b under wrap-around (the half-range rule).
constexpr bool seq_newer(uint16_t p_a, uint16_t p_b) {
	return p_a != p_b && uint16_t(p_a - p_b) < 0x8000u;
}
core::Result<Snapshot> decode_snapshot(std::span<const uint8_t> p_datagram);

// ---- M2a protocol v2 (docs/design/m2a.md §7; declared in W2.0, defined by nt) --------------------------------------
// nt bumps kProtocolVersion to 2 with the codecs. Decoders take bytes from the network: counts are bounded
// (shots <= kMaxShotsPerDatagram, others <= kMaxSlots - 1, combat events <= kMaxCombatEventsPerMessage), defs below
// the table size are the receiver's check, floats finite; a malformed message is refused and counted (E17).

// The others that did not fit the Snapshot of the same seq (unreliable). It carries the seq and server_tick of its
// Snapshot, so its records are usable when that Snapshot is lost; nearest first after the Snapshot's own.
struct SnapshotOthers {
	uint16_t seq = 0;
	sim::Tick server_tick = 0;
	std::vector<OtherSoldier> others;
};
std::vector<uint8_t> encode_snapshot_others(const SnapshotOthers &p_message, size_t *r_dropped = nullptr);
core::Result<SnapshotOthers> decode_snapshot_others(std::span<const uint8_t> p_datagram);

// One remote shot as decoded (the owner never receives its own shots: it predicted them). seed and spawn_time are
// derived by the receiver (shot_seed(match_seed, slot, shot_index); time + the def's SpawnDelay), never sent.
struct RemoteShot {
	uint8_t slot = 0;
	uint32_t shot_index = 0; // full 32 bits for a shooter's first shot in the datagram, then 4-bit deltas
	uint16_t def = sim::weapons::kNoWeapon;
	sim::weapons::SimTime time = 0; // 1/65,536 tick within the datagram's server tick on the wire
	sim::Vec3 origin{}; // 3 x 20 bits, 1/256 m from ShotQuantization::origin_min (a 4,096 m span per axis)
	sim::Vec3 direction{}; // octahedral 2 x 16
	uint8_t pellets = 1; // 5 bits
	bool tracer = false;
	sim::Vec3 aim{}; // with tracer: octahedral 2 x 12 (the tracer converges on it)
	float pellet_cone = 0; // with pellets > 1: 12 bits, 1/64 degree
	float power = 1; // hold-and-release defs: 8 bits, 1/255
};
inline constexpr size_t kMaxShotsPerDatagram = 128;
struct ShotEvents {
	sim::Tick server_tick = 0;
	std::vector<RemoteShot> shots;
};
// The level's quantisation frame for shot origins and combat positions (the static world's bounds' minimum).
struct ShotQuantization {
	sim::Vec3 origin_min{};
};
// At most kMaxDatagramBytes: further shots are dropped farthest-first (from p_receiver_eye) and counted in r_dropped;
// the GameServer sends at most two datagrams per snapshot.
std::vector<uint8_t> encode_shot_events(const ShotEvents &p_message, const ShotQuantization &p_frame, const sim::Vec3 &p_receiver_eye,
		size_t *r_dropped = nullptr);
core::Result<ShotEvents> decode_shot_events(std::span<const uint8_t> p_datagram, const ShotQuantization &p_frame);

// Combat events (reliable, to every client; docs/design/m2a.md §4.3). Damage and health travel in 1/100 hp,
// positions as shot origins, normals octahedral 16.
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
	uint8_t killer = 0; // == victim for fall damage
	uint16_t def = sim::weapons::kNoWeapon; // kNoWeapon: not a weapon (fall damage)
	sim::weapons::HitZone zone = sim::weapons::HitZone::Body;
	uint32_t shot_index = 0;
	bool headshot = false;
	sim::weapons::SimTime time = 0;
};
// From wp's DetonationEvent, so every client places a grenade's explosion where the server's flew.
struct DetonationWire {
	uint8_t shooter = 0;
	uint32_t shot_index = 0;
	uint16_t def = sim::weapons::kNoWeapon;
	sim::weapons::SimTime time = 0;
	sim::Vec3 position{};
	sim::Vec3 normal{};
	bool dud = false;
};
inline constexpr size_t kMaxCombatEventsPerMessage = 256;
struct CombatEvents {
	sim::Tick server_tick = 0;
	std::vector<HitEvent> hits;
	std::vector<DeathEvent> deaths;
	std::vector<DetonationWire> detonations;
};
std::vector<uint8_t> encode_combat_events(const CombatEvents &p_message, const ShotQuantization &p_frame);
core::Result<CombatEvents> decode_combat_events(std::span<const uint8_t> p_datagram, const ShotQuantization &p_frame);

// Client -> server: kit (< 4) and team (0 keep, 1, 2; a change needs dev_commands in M2a) for the next spawn.
struct KitSelect {
	uint8_t kit = 0;
	uint8_t team = 0;
};
std::vector<uint8_t> encode_kit_select(const KitSelect &p_message);
core::Result<KitSelect> decode_kit_select(std::span<const uint8_t> p_datagram);

} // namespace bf3::net

#pragma once
// The M1 wire protocol (plan §2.7, §3.5; topics/05 §4.1-§4.2, §7.1-§7.2, §7.7; docs/design/m1.md §7). Godot-free.
// The messages travel over the existing ENet path (enet_host.h): in M1 the in-process client connects to the
// listen server on 127.0.0.1; M3 binds the same server to a public address and nothing here changes.
//
// Channels (topics/05 §7.1):
//   0 unreliable, unsequenced  S->C Snapshot (one datagram, <= kMaxDatagramBytes); C->S CommandPacket (60 Hz,
//                              the last kCommandsPerPacket commands, so a lost datagram loses no command)
//   1 reliable, ordered        control: Hello, MatchInfo, Ready, Spawned, Loading, Kick, Debug*, Bye
//   2 reliable, ordered        bulk (M3+: FullSync of destruction rows and the logic ghost); unused in M1
// Every datagram starts with a u8 message type, then the message's bit-packed body (bit_stream.h). Decoders take
// bytes from the network: they bound every count, check every enum and fail on a short or over-long body.

#include "bf3/core/cache/digest.h"
#include "bf3/core/result.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

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
};

struct Ready {
	std::optional<core::Sha256Digest> source_digest;
};

struct Spawned {
	uint8_t slot = 0;
	uint32_t spawn_index = 0;
	sim::SoldierState state; // full precision, as the own-state block
};

struct Loading {
	uint8_t stage = 0; // bf3::host::LoadStage
	uint16_t permille = 0;
};

struct Kick {
	std::string reason;
};

enum class DebugKind : uint8_t {
	Teleport = 1, // put the soldier at position/yaw (a fresh standing state, velocity 0)
	SpawnAt = 2, // respawn the soldier at spawn point `index` (settle included)
	SpawnSweep = 3, // spawn one idle dummy at every spawn point, report each settle after `ticks`
	SetSeed = 4, // reseed the server's spawn choice
};

struct DebugRequest {
	DebugKind kind = DebugKind::Teleport;
	uint32_t id = 0; // echoed in the report
	sim::Vec3 position{};
	uint16_t yaw = 0;
	uint32_t index = 0;
	uint32_t ticks = 60;
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
// sequence. The flags byte is server-side only and never sent. A typical 4-command packet is 18-30 bytes.

struct CommandPacket {
	uint16_t newest_snapshot = 0;
	uint32_t snapshot_ack_bits = 0; // bit i = snapshot newest_snapshot - 1 - i arrived
	std::array<sim::Command, kCommandsPerPacket> commands{}; // [0] newest; consecutive ticks and sequences
	uint8_t count = 0;
};
std::vector<uint8_t> encode_command_packet(const CommandPacket &p_packet);
core::Result<CommandPacket> decode_command_packet(std::span<const uint8_t> p_datagram);

// ---- snapshots (ch0, S->C, every snapshot_divisor ticks) ---------------------------------------------------------
// Body: u16 seq | u32 server tick | u32 last command tick and u32 last command sequence applied to this client's
// soldier, 1 bit repeated | i8 command buffer ticks (newest command tick received - server tick; time sync §7.5)
// | u16 echo of the newest command sequence received (RTT) | own-state block | u8 others count | others.
// Own-state block: 1 bit present; then every SoldierState field at full precision (floats as f32 bit patterns,
// integers in their own widths, the reserved fields left out): 77 bytes. Header + own state ~95 bytes.
// Other soldier (M1: absolute, no baseline; M3 adds a 1-bit "delta vs acked baseline" in front without moving
// the rest): u8 slot | u32 life | position 20+18+20 bits (quant::kPosition*) | velocity 3 x 11 bits | yaw 14 |
// pitch 12 | pose 2 | target pose 2 | transition progress 8 (0..255 of the total) | mode 4 | flags 8 (alive,
// grounded, sprinting): 181 bits, ~23 bytes, so ~47 others fit one datagram.

struct OtherSoldier {
	uint8_t slot = 0;
	uint32_t life = 0;
	sim::Vec3 position{}; // after quantisation (what the receiver gets)
	sim::Vec3 velocity{};
	uint16_t yaw = 0; // 16-bit Command scale, low 2 bits zero
	int16_t pitch = 0; // Command scale, low 2 bits zero
	sim::Pose pose = sim::Pose::Stand;
	sim::Pose target_pose = sim::Pose::Stand;
	uint8_t transition = 0; // 0 = start, 255 = done
	sim::MoveMode mode = sim::MoveMode::OnGround;
	uint8_t flags = 0; // low 8 bits of soldier_flags::
};
// The quantised view of a server state, exactly as the receiver decodes it (the server's interpolation tests
// compare against this).
OtherSoldier quantize_other(uint8_t p_slot, const sim::SoldierState &p_state);

struct Snapshot {
	uint16_t seq = 0;
	sim::Tick server_tick = 0;
	sim::Tick last_command_tick = 0;
	uint32_t last_command_sequence = 0;
	bool last_command_repeated = false;
	int8_t command_buffer_ticks = 0;
	uint16_t sequence_echo = 0;
	std::optional<sim::SoldierState> own; // the receiving client's soldier after server_tick
	std::vector<OtherSoldier> others; // every other live soldier, nearest first, as many as fit
};
// Fills at most kMaxDatagramBytes; others that do not fit are left out (r_dropped counts them). M1's soldier
// counts never come near the budget; M3 replaces the "nearest first" order with ghost priority.
std::vector<uint8_t> encode_snapshot(const Snapshot &p_snapshot, size_t *r_dropped = nullptr);
core::Result<Snapshot> decode_snapshot(std::span<const uint8_t> p_datagram);

} // namespace bf3::net

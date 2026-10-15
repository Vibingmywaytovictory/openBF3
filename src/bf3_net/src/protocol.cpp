// The M1 wire protocol (protocol.h; docs/design/m1.md §7.2-§7.3, §7.7). Every encoder writes through a capped
// BitWriter; every decoder reads through a BitReader, bounds every count by both its limit and the bits left
// before it allocates, checks every enum and float, and fails on a short body or on trailing bytes. Datagrams
// come from the network, so a decoder never trusts a count, a length or an enum value.
#include "bf3/net/protocol.h"

#include "bf3/net/bit_stream.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace bf3::net {

namespace {

// Limits of the variable-length parts. They are internal: every encoder clamps to them, every decoder refuses more.
constexpr size_t kMaxIdentBytes = 128; // MatchInfo level / mode names (MP_Subway, TeamDeathMatch0)
constexpr size_t kMaxTextBytes = 2048; // Kick reason, DebugReport text
constexpr size_t kMaxSweepRows = 1024; // DebugReport::sweep (Metro TDM has 102 points)
// The reliable channel may fragment, so its messages are capped by the largest legal message instead of the
// datagram: a DebugReport with kMaxTraceTicks states (77 B each), kMaxSweepRows rows and kMaxTextBytes of text.
constexpr size_t kMaxReliableBytes = 256 * 1024;
constexpr float kMaxDebugCoordinate = 1.0e6f; // a teleport target beyond this is refused (hostile, or a bug)

constexpr unsigned kOwnStateBits = 77 * 8;
constexpr unsigned kCommandFieldBits = 24 + 8 + 8 + 16 + quant::kCommandPitchBits + 2; // 72
constexpr unsigned kOtherBits = 8 + 8 + quant::kPositionXZ.bits + quant::kPositionY.bits + quant::kPositionXZ.bits +
		3 * quant::kSoldierVelocity.bits + quant::kSoldierYawBits + quant::kSoldierPitchBits + 2 + 2 + 8 + 4 + 8;
static_assert(kOtherBits == 159); // docs §7.3 table
static_assert(kCommandFieldBits == 72); // + 1 "same" bit = the 73 bits of docs §7.3
constexpr int32_t kOtherPitchMaxCode = (1 << (quant::kSoldierPitchBits - 1)) - 1; // 2047: 8188 Command steps

core::Error fail(const char *p_what, const char *p_why) {
	return core::make_error(std::string(p_what) + ": " + p_why);
}

bool valid_pose(uint64_t p_value) {
	return p_value < sim::kPoseCount;
}

// ---- strings ----------------------------------------------------------------------------------------------------

// Strict UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool valid_utf8(std::string_view p_text) {
	size_t i = 0;
	const size_t n = p_text.size();
	while (i < n) {
		const uint8_t c = uint8_t(p_text[i]);
		if (c < 0x80) {
			++i;
			continue;
		}
		size_t len = 0;
		uint32_t cp = 0;
		uint32_t min = 0;
		if ((c & 0xE0) == 0xC0) {
			len = 2;
			cp = c & 0x1F;
			min = 0x80;
		} else if ((c & 0xF0) == 0xE0) {
			len = 3;
			cp = c & 0x0F;
			min = 0x800;
		} else if ((c & 0xF8) == 0xF0) {
			len = 4;
			cp = c & 0x07;
			min = 0x10000;
		} else {
			return false;
		}
		if (i + len > n) {
			return false;
		}
		for (size_t k = 1; k < len; ++k) {
			const uint8_t d = uint8_t(p_text[i + k]);
			if ((d & 0xC0) != 0x80) {
				return false;
			}
			cp = (cp << 6) | (d & 0x3F);
		}
		if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
			return false;
		}
		i += len;
	}
	return true;
}

// The longest prefix of p_text that has at most p_max bytes and does not end inside a multi-byte sequence.
std::string_view clamp_utf8(std::string_view p_text, size_t p_max) {
	if (p_text.size() <= p_max) {
		return p_text;
	}
	size_t cut = p_max;
	while (cut > 0 && (uint8_t(p_text[cut]) & 0xC0) == 0x80) {
		--cut;
	}
	return p_text.substr(0, cut);
}

void write_string(BitWriter &r_w, std::string_view p_text, size_t p_max) {
	const std::string_view t = clamp_utf8(p_text, p_max);
	r_w.write_varint(t.size());
	for (const char c : t) {
		r_w.write_bits(uint8_t(c), 8);
	}
}

bool read_string(BitReader &r_r, size_t p_max, std::string &r_text) {
	const uint64_t len = r_r.read_varint();
	if (r_r.failed() || len > p_max || len * 8 > r_r.bits_left()) {
		return false;
	}
	r_text.resize(size_t(len));
	for (char &c : r_text) {
		c = char(r_r.read_bits(8));
	}
	return !r_r.failed() && valid_utf8(r_text);
}

// ---- small helpers ------------------------------------------------------------------------------------------------

void write_digest(BitWriter &r_w, const std::optional<core::Sha256Digest> &p_digest) {
	r_w.write_bool(p_digest.has_value());
	if (p_digest) {
		for (const uint8_t b : p_digest->bytes) {
			r_w.write_bits(b, 8);
		}
	}
}

void read_digest(BitReader &r_r, std::optional<core::Sha256Digest> &r_digest) {
	if (!r_r.read_bool()) {
		r_digest.reset();
		return;
	}
	core::Sha256Digest d;
	for (uint8_t &b : d.bytes) {
		b = uint8_t(r_r.read_bits(8));
	}
	r_digest = d;
}

void write_vec3(BitWriter &r_w, const sim::Vec3 &p_v) {
	for (const float f : p_v) {
		r_w.write_f32(f);
	}
}

bool read_vec3(BitReader &r_r, sim::Vec3 &r_v) {
	bool finite = true;
	for (float &f : r_v) {
		f = r_r.read_f32();
		finite = finite && std::isfinite(f);
	}
	return finite;
}

BitWriter begin(MessageType p_type, size_t p_cap = kMaxReliableBytes) {
	BitWriter w(p_cap);
	w.write_bits(uint8_t(p_type), 8);
	return w;
}

std::vector<uint8_t> finish(BitWriter &r_w) {
	if (r_w.overflowed()) {
		return {}; // only reachable through a bug: every variable part is clamped to a limit the cap covers
	}
	return r_w.take();
}

// Checks the type byte; on success the reader stands after it.
bool open(BitReader &r_r, MessageType p_type) {
	return uint8_t(r_r.read_bits(8)) == uint8_t(p_type) && !r_r.failed();
}

template <class T>
core::Result<T> close(BitReader &r_r, T p_value, const char *p_what) {
	if (r_r.failed()) {
		return fail(p_what, "truncated");
	}
	if (!r_r.at_end()) {
		return fail(p_what, "trailing bytes");
	}
	return p_value;
}

// ---- full-precision soldier state (the own-state block, Spawned, Trace) ----------------------------------------

void write_state(BitWriter &r_w, const sim::SoldierState &p_s) {
	r_w.write_bits(p_s.tick, 32);
	r_w.write_bits(p_s.life, 32);
	write_vec3(r_w, p_s.position);
	write_vec3(r_w, p_s.velocity);
	write_vec3(r_w, p_s.ground_normal);
	r_w.write_bits(p_s.yaw, 16);
	r_w.write_signed(p_s.pitch, 16);
	r_w.write_bits(uint8_t(p_s.pose), 8);
	r_w.write_bits(uint8_t(p_s.target_pose), 8);
	r_w.write_bits(p_s.transition_ticks, 16);
	r_w.write_bits(p_s.transition_total, 16);
	r_w.write_bits(uint8_t(p_s.mode), 8);
	r_w.write_bits(p_s.flags, 16);
	r_w.write_bits(p_s.events, 16);
	r_w.write_bits(p_s.jump_penalty_ticks, 16);
	r_w.write_bits(p_s.air_ticks, 16);
	r_w.write_bits(p_s.protect_ticks, 16);
	r_w.write_f32(p_s.health);
	r_w.write_f32(p_s.impact_speed);
	r_w.write_f32(p_s.damage);
}

// Fills r_s; false when a value is out of its range (the caller still checks failed()).
bool read_state(BitReader &r_r, sim::SoldierState &r_s) {
	r_s = sim::SoldierState{};
	r_s.tick = sim::Tick(r_r.read_bits(32));
	r_s.life = uint32_t(r_r.read_bits(32));
	bool ok = read_vec3(r_r, r_s.position);
	ok = read_vec3(r_r, r_s.velocity) && ok;
	ok = read_vec3(r_r, r_s.ground_normal) && ok;
	r_s.yaw = uint16_t(r_r.read_bits(16));
	r_s.pitch = int16_t(r_r.read_signed(16));
	const uint64_t pose = r_r.read_bits(8);
	const uint64_t target = r_r.read_bits(8);
	r_s.transition_ticks = uint16_t(r_r.read_bits(16));
	r_s.transition_total = uint16_t(r_r.read_bits(16));
	const uint64_t mode = r_r.read_bits(8);
	r_s.flags = uint16_t(r_r.read_bits(16));
	r_s.events = uint16_t(r_r.read_bits(16));
	r_s.jump_penalty_ticks = uint16_t(r_r.read_bits(16));
	r_s.air_ticks = uint16_t(r_r.read_bits(16));
	r_s.protect_ticks = uint16_t(r_r.read_bits(16));
	r_s.health = r_r.read_f32();
	r_s.impact_speed = r_r.read_f32();
	r_s.damage = r_r.read_f32();
	ok = ok && valid_pose(pose) && valid_pose(target) && mode < sim::kMoveModeCount;
	ok = ok && r_s.pitch >= -sim::kPitchLimit && r_s.pitch <= sim::kPitchLimit;
	ok = ok && std::isfinite(r_s.health) && std::isfinite(r_s.impact_speed) && std::isfinite(r_s.damage);
	r_s.pose = sim::Pose(ok ? pose : 0);
	r_s.target_pose = sim::Pose(ok ? target : 0);
	r_s.mode = sim::MoveMode(ok ? mode : 0);
	return ok;
}

// ---- commands ----------------------------------------------------------------------------------------------------

// The command as it goes on the wire: the encoder clamps to the ranges command.h promises (pitch +-kPitchLimit,
// axes -127..127, 24 button bits), so the decoder can refuse anything outside them.
struct WireCommand {
	uint32_t buttons = 0;
	int8_t forward = 0;
	int8_t right = 0;
	uint16_t yaw = 0;
	int16_t pitch = 0;
	uint8_t pose = 0;
	bool operator==(const WireCommand &) const = default;
};

int8_t clamp_axis(int8_t p_axis) {
	return p_axis < -sim::kAxisMax ? int8_t(-sim::kAxisMax) : p_axis;
}

WireCommand wire_of(const sim::Command &p_c) {
	WireCommand w;
	w.buttons = p_c.buttons & sim::buttons::kWireMask;
	w.forward = clamp_axis(p_c.move_forward);
	w.right = clamp_axis(p_c.move_right);
	w.yaw = p_c.yaw;
	w.pitch = std::clamp<int16_t>(p_c.pitch, int16_t(-sim::kPitchLimit), sim::kPitchLimit);
	w.pose = uint8_t(p_c.pose) & 3;
	return w;
}

void write_command_fields(BitWriter &r_w, const WireCommand &p_c) {
	r_w.write_bits(p_c.buttons, 24);
	r_w.write_signed(p_c.forward, 8);
	r_w.write_signed(p_c.right, 8);
	r_w.write_bits(p_c.yaw, 16);
	r_w.write_signed(p_c.pitch, quant::kCommandPitchBits);
	r_w.write_bits(p_c.pose, 2);
}

bool read_command_fields(BitReader &r_r, sim::Command &r_c) {
	r_c.buttons = uint32_t(r_r.read_bits(24));
	r_c.move_forward = int8_t(r_r.read_signed(8));
	r_c.move_right = int8_t(r_r.read_signed(8));
	r_c.yaw = uint16_t(r_r.read_bits(16));
	r_c.pitch = int16_t(r_r.read_signed(quant::kCommandPitchBits));
	const uint64_t pose = r_r.read_bits(2);
	r_c.flags = 0; // server-side only, never on the wire
	if (!valid_pose(pose) || r_c.move_forward < -sim::kAxisMax || r_c.move_right < -sim::kAxisMax ||
			r_c.pitch < -sim::kPitchLimit || r_c.pitch > sim::kPitchLimit) {
		return false;
	}
	r_c.pose = sim::Pose(pose);
	return true;
}

// ---- others ------------------------------------------------------------------------------------------------------

uint32_t other_yaw_code(uint16_t p_yaw) {
	return ((uint32_t(p_yaw) + 2u) >> 2) & ((1u << quant::kSoldierYawBits) - 1u); // nearest, wraps at a turn
}

int32_t other_pitch_code(int16_t p_pitch) {
	const int32_t code = (int32_t(p_pitch) + 2) >> 2; // nearest (arithmetic shift = floor)
	return std::clamp(code, -kOtherPitchMaxCode, kOtherPitchMaxCode);
}

uint8_t transition_progress(const sim::SoldierState &p_s) {
	if (p_s.transition_ticks == 0 || p_s.transition_total == 0) {
		return 255;
	}
	if (p_s.transition_ticks >= p_s.transition_total) {
		return 0;
	}
	const uint32_t done = uint32_t(p_s.transition_total - p_s.transition_ticks);
	return uint8_t((done * 255u + p_s.transition_total / 2u) / p_s.transition_total);
}

void write_other(BitWriter &r_w, const OtherSoldier &p_o) {
	r_w.write_bits(p_o.slot, 8);
	r_w.write_bits(p_o.generation, 8);
	r_w.write_bits(quant::kPositionXZ.encode(p_o.position[0]), quant::kPositionXZ.bits);
	r_w.write_bits(quant::kPositionY.encode(p_o.position[1]), quant::kPositionY.bits);
	r_w.write_bits(quant::kPositionXZ.encode(p_o.position[2]), quant::kPositionXZ.bits);
	for (const float v : p_o.velocity) {
		r_w.write_bits(quant::kSoldierVelocity.encode(v), quant::kSoldierVelocity.bits);
	}
	r_w.write_bits(other_yaw_code(p_o.yaw), quant::kSoldierYawBits);
	r_w.write_signed(other_pitch_code(p_o.pitch), quant::kSoldierPitchBits);
	r_w.write_bits(uint8_t(p_o.pose) & 3, 2);
	r_w.write_bits(uint8_t(p_o.target_pose) & 3, 2);
	r_w.write_bits(p_o.transition, 8);
	r_w.write_bits(uint8_t(p_o.mode) & 15, 4);
	r_w.write_bits(p_o.flags, 8);
}

bool read_other(BitReader &r_r, OtherSoldier &r_o) {
	r_o = OtherSoldier{};
	const uint64_t slot = r_r.read_bits(8);
	r_o.generation = uint8_t(r_r.read_bits(8));
	r_o.position[0] = quant::kPositionXZ.decode(uint32_t(r_r.read_bits(quant::kPositionXZ.bits)));
	r_o.position[1] = quant::kPositionY.decode(uint32_t(r_r.read_bits(quant::kPositionY.bits)));
	r_o.position[2] = quant::kPositionXZ.decode(uint32_t(r_r.read_bits(quant::kPositionXZ.bits)));
	for (float &v : r_o.velocity) {
		v = quant::kSoldierVelocity.decode(uint32_t(r_r.read_bits(quant::kSoldierVelocity.bits)));
	}
	r_o.yaw = uint16_t(r_r.read_bits(quant::kSoldierYawBits) << 2);
	const int64_t pitch = r_r.read_signed(quant::kSoldierPitchBits);
	const uint64_t pose = r_r.read_bits(2);
	const uint64_t target = r_r.read_bits(2);
	r_o.transition = uint8_t(r_r.read_bits(8));
	const uint64_t mode = r_r.read_bits(4);
	r_o.flags = uint8_t(r_r.read_bits(8));
	if (slot >= kMaxSlots || pitch < -kOtherPitchMaxCode || pitch > kOtherPitchMaxCode || !valid_pose(pose) ||
			!valid_pose(target) || mode >= sim::kMoveModeCount) {
		return false;
	}
	r_o.slot = uint8_t(slot);
	r_o.pitch = int16_t(pitch * 4);
	r_o.pose = sim::Pose(pose);
	r_o.target_pose = sim::Pose(target);
	r_o.mode = sim::MoveMode(mode);
	return true;
}

} // namespace

// ---- type byte ---------------------------------------------------------------------------------------------------

std::optional<MessageType> peek_type(std::span<const uint8_t> p_datagram) {
	if (p_datagram.empty()) {
		return std::nullopt;
	}
	switch (MessageType(p_datagram[0])) {
		case MessageType::Snapshot:
		case MessageType::CommandPacket:
		case MessageType::Hello:
		case MessageType::MatchInfo:
		case MessageType::Ready:
		case MessageType::Spawned:
		case MessageType::Loading:
		case MessageType::Kick:
		case MessageType::Bye:
		case MessageType::Debug:
		case MessageType::DebugReport:
			return MessageType(p_datagram[0]);
	}
	return std::nullopt;
}

// ---- control -----------------------------------------------------------------------------------------------------

std::vector<uint8_t> encode_hello(const Hello &p_message) {
	BitWriter w = begin(MessageType::Hello);
	w.write_bits(p_message.protocol, 16);
	write_string(w, p_message.name, kMaxNameBytes);
	return finish(w);
}

core::Result<Hello> decode_hello(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::Hello)) {
		return fail("Hello", "wrong type");
	}
	Hello m;
	m.protocol = uint16_t(r.read_bits(16));
	if (!read_string(r, kMaxNameBytes, m.name)) {
		return fail("Hello", "bad name");
	}
	return close(r, std::move(m), "Hello");
}

std::vector<uint8_t> encode_match_info(const MatchInfo &p_message) {
	BitWriter w = begin(MessageType::MatchInfo);
	w.write_bits(p_message.protocol, 16);
	write_string(w, p_message.level, kMaxIdentBytes);
	write_string(w, p_message.mode, kMaxIdentBytes);
	w.write_bits(p_message.sim_hz, 16);
	w.write_bits(p_message.snapshot_divisor, 8);
	w.write_bits(p_message.slot, 8);
	w.write_bits(p_message.server_tick, 32);
	w.write_bits(uint8_t(p_message.phase), 8);
	write_digest(w, p_message.source_digest);
	w.write_bits(p_message.motor_hash, 64);
	return finish(w);
}

core::Result<MatchInfo> decode_match_info(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::MatchInfo)) {
		return fail("MatchInfo", "wrong type");
	}
	MatchInfo m;
	m.protocol = uint16_t(r.read_bits(16));
	if (!read_string(r, kMaxIdentBytes, m.level) || !read_string(r, kMaxIdentBytes, m.mode)) {
		return fail("MatchInfo", "bad level or mode name");
	}
	m.sim_hz = uint16_t(r.read_bits(16));
	m.snapshot_divisor = uint8_t(r.read_bits(8));
	const uint64_t slot = r.read_bits(8);
	m.server_tick = sim::Tick(r.read_bits(32));
	const uint64_t phase = r.read_bits(8);
	read_digest(r, m.source_digest);
	m.motor_hash = r.read_bits(64);
	if (r.failed()) {
		return fail("MatchInfo", "truncated");
	}
	if (m.sim_hz == 0 || m.snapshot_divisor == 0) {
		return fail("MatchInfo", "zero tick rate or snapshot divisor");
	}
	if (slot >= kMaxSlots) {
		return fail("MatchInfo", "slot out of range");
	}
	if (phase > uint8_t(MatchPhase::RoundOver)) {
		return fail("MatchInfo", "unknown phase");
	}
	m.slot = uint8_t(slot);
	m.phase = MatchPhase(phase);
	return close(r, std::move(m), "MatchInfo");
}

std::vector<uint8_t> encode_ready(const Ready &p_message) {
	BitWriter w = begin(MessageType::Ready);
	write_digest(w, p_message.source_digest);
	return finish(w);
}

core::Result<Ready> decode_ready(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::Ready)) {
		return fail("Ready", "wrong type");
	}
	Ready m;
	read_digest(r, m.source_digest);
	return close(r, std::move(m), "Ready");
}

std::vector<uint8_t> encode_spawned(const Spawned &p_message) {
	BitWriter w = begin(MessageType::Spawned);
	w.write_bits(p_message.slot, 8);
	w.write_bits(p_message.spawn_index, 32);
	write_state(w, p_message.state);
	return finish(w);
}

core::Result<Spawned> decode_spawned(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::Spawned)) {
		return fail("Spawned", "wrong type");
	}
	Spawned m;
	const uint64_t slot = r.read_bits(8);
	m.spawn_index = uint32_t(r.read_bits(32));
	const bool state_ok = read_state(r, m.state);
	if (r.failed()) {
		return fail("Spawned", "truncated");
	}
	if (slot >= kMaxSlots) {
		return fail("Spawned", "slot out of range");
	}
	if (!state_ok) {
		return fail("Spawned", "state field out of range");
	}
	m.slot = uint8_t(slot);
	return close(r, std::move(m), "Spawned");
}

std::vector<uint8_t> encode_loading(const Loading &p_message) {
	BitWriter w = begin(MessageType::Loading);
	w.write_bits(p_message.stage, 8);
	w.write_bits(std::min<uint16_t>(p_message.permille, 1000), 16);
	return finish(w);
}

core::Result<Loading> decode_loading(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::Loading)) {
		return fail("Loading", "wrong type");
	}
	Loading m;
	m.stage = uint8_t(r.read_bits(8));
	m.permille = uint16_t(r.read_bits(16));
	if (!r.failed() && m.permille > 1000) {
		return fail("Loading", "progress above 1000 permille");
	}
	return close(r, std::move(m), "Loading");
}

std::vector<uint8_t> encode_kick(const Kick &p_message) {
	BitWriter w = begin(MessageType::Kick);
	write_string(w, p_message.reason, kMaxTextBytes);
	return finish(w);
}

core::Result<Kick> decode_kick(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::Kick)) {
		return fail("Kick", "wrong type");
	}
	Kick m;
	if (!read_string(r, kMaxTextBytes, m.reason)) {
		return fail("Kick", "bad reason text");
	}
	return close(r, std::move(m), "Kick");
}

std::vector<uint8_t> encode_bye() {
	BitWriter w = begin(MessageType::Bye);
	return finish(w);
}

std::vector<uint8_t> encode_debug(const DebugRequest &p_message) {
	BitWriter w = begin(MessageType::Debug);
	w.write_bits(uint8_t(p_message.kind), 8);
	w.write_bits(p_message.id, 32);
	write_vec3(w, p_message.position);
	w.write_bits(p_message.yaw, 16);
	w.write_bits(p_message.index, 32);
	w.write_bits(p_message.ticks, 32);
	return finish(w);
}

core::Result<DebugRequest> decode_debug(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::Debug)) {
		return fail("Debug", "wrong type");
	}
	DebugRequest m;
	const uint64_t kind = r.read_bits(8);
	m.id = uint32_t(r.read_bits(32));
	const bool finite = read_vec3(r, m.position);
	m.yaw = uint16_t(r.read_bits(16));
	m.index = uint32_t(r.read_bits(32));
	m.ticks = uint32_t(r.read_bits(32));
	if (r.failed()) {
		return fail("Debug", "truncated");
	}
	if (kind < uint8_t(DebugKind::Teleport) || kind > uint8_t(DebugKind::Trace)) {
		return fail("Debug", "unknown kind");
	}
	if (!finite || std::fabs(m.position[0]) > kMaxDebugCoordinate || std::fabs(m.position[1]) > kMaxDebugCoordinate ||
			std::fabs(m.position[2]) > kMaxDebugCoordinate) {
		return fail("Debug", "position out of range");
	}
	if (m.ticks > kMaxTraceTicks) {
		return fail("Debug", "tick count above kMaxTraceTicks");
	}
	m.kind = DebugKind(kind);
	return close(r, std::move(m), "Debug");
}

std::vector<uint8_t> encode_debug_report(const DebugReport &p_message) {
	BitWriter w = begin(MessageType::DebugReport);
	w.write_bits(p_message.id, 32);
	w.write_bool(p_message.ok);
	write_string(w, p_message.text, kMaxTextBytes);
	const size_t rows = std::min(p_message.sweep.size(), kMaxSweepRows);
	w.write_varint(rows);
	for (size_t i = 0; i < rows; ++i) {
		const SpawnSweepRow &row = p_message.sweep[i];
		w.write_bits(row.index, 32);
		w.write_bool(row.ok);
		write_vec3(w, row.point);
		write_vec3(w, row.settled);
		w.write_f32(row.lift);
		w.write_f32(row.shift);
		w.write_f32(row.gap);
		w.write_bool(row.grounded);
		w.write_bool(row.overlapping);
	}
	const size_t states = std::min<size_t>(p_message.trace.size(), kMaxTraceTicks);
	w.write_varint(states);
	for (size_t i = 0; i < states; ++i) {
		write_state(w, p_message.trace[i]);
	}
	return finish(w);
}

core::Result<DebugReport> decode_debug_report(std::span<const uint8_t> p_datagram) {
	constexpr size_t kRowBits = 32 + 1 + 9 * 32 + 2;
	BitReader r(p_datagram);
	if (!open(r, MessageType::DebugReport)) {
		return fail("DebugReport", "wrong type");
	}
	DebugReport m;
	m.id = uint32_t(r.read_bits(32));
	m.ok = r.read_bool();
	if (!read_string(r, kMaxTextBytes, m.text)) {
		return fail("DebugReport", "bad text");
	}
	const uint64_t rows = r.read_varint();
	if (r.failed() || rows > kMaxSweepRows || rows * kRowBits > r.bits_left()) {
		return fail("DebugReport", "bad sweep row count");
	}
	m.sweep.resize(size_t(rows));
	for (SpawnSweepRow &row : m.sweep) {
		row.index = uint32_t(r.read_bits(32));
		row.ok = r.read_bool();
		bool finite = read_vec3(r, row.point);
		finite = read_vec3(r, row.settled) && finite;
		row.lift = r.read_f32();
		row.shift = r.read_f32();
		row.gap = r.read_f32();
		row.grounded = r.read_bool();
		row.overlapping = r.read_bool();
		if (!finite || !std::isfinite(row.lift) || !std::isfinite(row.shift) || !std::isfinite(row.gap)) {
			return fail("DebugReport", "non-finite sweep value");
		}
	}
	const uint64_t states = r.read_varint();
	if (r.failed() || states > kMaxTraceTicks || states * kOwnStateBits > r.bits_left()) {
		return fail("DebugReport", "bad trace count");
	}
	m.trace.resize(size_t(states));
	for (sim::SoldierState &s : m.trace) {
		if (!read_state(r, s)) {
			return fail("DebugReport", r.failed() ? "truncated" : "trace state field out of range");
		}
	}
	return close(r, std::move(m), "DebugReport");
}

// ---- commands ----------------------------------------------------------------------------------------------------

std::vector<uint8_t> encode_command_packet(const CommandPacket &p_packet) {
	size_t count = std::min<size_t>(p_packet.count, kCommandsPerPacket);
	if (count == 0) {
		return {}; // a packet carries at least one command; the decoder refuses count 0
	}
	// The decoder implies tick and sequence of each older command (one less each), so cut at the first gap.
	for (size_t i = 1; i < count; ++i) {
		const sim::Command &newer = p_packet.commands[i - 1];
		const sim::Command &older = p_packet.commands[i];
		if (newer.tick == 0 || newer.sequence == 0 || older.tick != newer.tick - 1 || older.sequence != newer.sequence - 1) {
			count = i;
			break;
		}
	}
	BitWriter w = begin(MessageType::CommandPacket, kMaxDatagramBytes);
	w.write_bits(p_packet.newest_snapshot, 16);
	w.write_bits(p_packet.snapshot_ack_bits, 32);
	w.write_bits(count, 8);
	const sim::Command &newest = p_packet.commands[0];
	w.write_bits(newest.tick, 32);
	w.write_bits(newest.sequence, 32);
	WireCommand prev = wire_of(newest);
	write_command_fields(w, prev);
	for (size_t i = 1; i < count; ++i) {
		const WireCommand c = wire_of(p_packet.commands[i]);
		const bool same = c == prev;
		w.write_bool(same);
		if (!same) {
			write_command_fields(w, c);
		}
		prev = c;
	}
	return finish(w);
}

core::Result<CommandPacket> decode_command_packet(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::CommandPacket)) {
		return fail("CommandPacket", "wrong type");
	}
	CommandPacket p;
	p.newest_snapshot = uint16_t(r.read_bits(16));
	p.snapshot_ack_bits = uint32_t(r.read_bits(32));
	const uint64_t count = r.read_bits(8);
	if (r.failed()) {
		return fail("CommandPacket", "truncated");
	}
	if (count == 0 || count > kCommandsPerPacket) {
		return fail("CommandPacket", "command count out of range");
	}
	p.count = uint8_t(count);
	sim::Command &newest = p.commands[0];
	newest.tick = sim::Tick(r.read_bits(32));
	newest.sequence = uint32_t(r.read_bits(32));
	if (!read_command_fields(r, newest)) {
		return fail("CommandPacket", r.failed() ? "truncated" : "command field out of range");
	}
	for (size_t i = 1; i < count; ++i) {
		const sim::Command &newer = p.commands[i - 1];
		sim::Command &c = p.commands[i];
		if (newer.tick == 0 || newer.sequence == 0) {
			return fail("CommandPacket", "implied tick or sequence below 0");
		}
		if (r.read_bool()) {
			c = newer;
		} else if (!read_command_fields(r, c)) {
			return fail("CommandPacket", r.failed() ? "truncated" : "command field out of range");
		}
		c.tick = newer.tick - 1;
		c.sequence = newer.sequence - 1;
		c.flags = 0;
	}
	return close(r, std::move(p), "CommandPacket");
}

// ---- snapshots ---------------------------------------------------------------------------------------------------

OtherSoldier quantize_other(uint8_t p_slot, const sim::SoldierState &p_state) {
	OtherSoldier o;
	o.slot = p_slot;
	o.generation = uint8_t(p_state.life & 0xFF);
	o.position[0] = quant::kPositionXZ.decode(quant::kPositionXZ.encode(p_state.position[0]));
	o.position[1] = quant::kPositionY.decode(quant::kPositionY.encode(p_state.position[1]));
	o.position[2] = quant::kPositionXZ.decode(quant::kPositionXZ.encode(p_state.position[2]));
	for (size_t i = 0; i < 3; ++i) {
		o.velocity[i] = quant::kSoldierVelocity.decode(quant::kSoldierVelocity.encode(p_state.velocity[i]));
	}
	o.yaw = uint16_t(other_yaw_code(p_state.yaw) << 2);
	o.pitch = int16_t(other_pitch_code(p_state.pitch) * 4);
	o.pose = p_state.pose;
	o.target_pose = p_state.target_pose;
	o.transition = transition_progress(p_state);
	o.mode = p_state.mode;
	o.flags = uint8_t(p_state.flags & 0xFF);
	return o;
}

std::vector<uint8_t> encode_snapshot(const Snapshot &p_snapshot, size_t *r_dropped) {
	BitWriter w = begin(MessageType::Snapshot, kMaxDatagramBytes);
	w.write_bits(p_snapshot.seq, 16);
	w.write_bits(p_snapshot.server_tick, 32);
	w.write_bits(p_snapshot.applied_sequence, 32);
	w.write_bool(p_snapshot.applied_repeated);
	w.write_signed(p_snapshot.command_buffer_ticks, 8);
	w.write_bits(p_snapshot.sequence_echo, 16);
	w.write_bits(p_snapshot.echo_hold_100us, 16);
	w.write_bits(p_snapshot.own_events, 16);
	w.write_bool(p_snapshot.own.has_value());
	if (p_snapshot.own) {
		write_state(w, *p_snapshot.own);
	}
	// As many others as fit beside the count byte, in the caller's (nearest-first) order.
	const size_t used = w.bit_count() + 8;
	const size_t room = used < kMaxDatagramBytes * 8 ? (kMaxDatagramBytes * 8 - used) / kOtherBits : 0;
	const size_t count = std::min({ p_snapshot.others.size(), room, kMaxSlots });
	w.write_bits(count, 8);
	for (size_t i = 0; i < count; ++i) {
		write_other(w, p_snapshot.others[i]);
	}
	if (r_dropped) {
		*r_dropped = p_snapshot.others.size() - count;
	}
	return finish(w);
}

core::Result<Snapshot> decode_snapshot(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::Snapshot)) {
		return fail("Snapshot", "wrong type");
	}
	Snapshot s;
	s.seq = uint16_t(r.read_bits(16));
	s.server_tick = sim::Tick(r.read_bits(32));
	s.applied_sequence = uint32_t(r.read_bits(32));
	s.applied_repeated = r.read_bool();
	s.command_buffer_ticks = int8_t(r.read_signed(8));
	s.sequence_echo = uint16_t(r.read_bits(16));
	s.echo_hold_100us = uint16_t(r.read_bits(16));
	s.own_events = uint16_t(r.read_bits(16));
	if (r.read_bool()) {
		sim::SoldierState own;
		const bool ok = read_state(r, own);
		if (r.failed()) {
			return fail("Snapshot", "truncated");
		}
		if (!ok) {
			return fail("Snapshot", "own state field out of range");
		}
		if (own.tick != s.server_tick) {
			return fail("Snapshot", "own state tick differs from the server tick");
		}
		s.own = own;
	}
	const uint64_t count = r.read_bits(8);
	if (r.failed()) {
		return fail("Snapshot", "truncated");
	}
	if (count > kMaxSlots || count * kOtherBits > r.bits_left()) {
		return fail("Snapshot", "others count out of range");
	}
	std::array<bool, kMaxSlots> seen{};
	s.others.resize(size_t(count));
	for (OtherSoldier &o : s.others) {
		if (!read_other(r, o)) {
			return fail("Snapshot", r.failed() ? "truncated" : "other soldier field out of range");
		}
		if (seen[o.slot]) {
			return fail("Snapshot", "slot named twice");
		}
		seen[o.slot] = true;
	}
	return close(r, std::move(s), "Snapshot");
}

} // namespace bf3::net

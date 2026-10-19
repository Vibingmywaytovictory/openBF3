// The M1 wire protocol (protocol.h; docs/design/m1.md §7.2-§7.3, §7.7). Every encoder writes through a capped
// BitWriter; every decoder reads through a BitReader, bounds every count by both its limit and the bits left
// before it allocates, checks every enum and float, and fails on a short body or on trailing bytes. Datagrams
// come from the network, so a decoder never trusts a count, a length or an enum value.
#include "bf3/net/protocol.h"

#include "bf3/net/bit_stream.h"
#include "bf3/net/weapons_wire.h"

#include <algorithm>
#include <array>
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
// M2a v2 (nt; docs/design/m2a.md §6.2, §7.2, §7.3).
constexpr unsigned kViewSampleBits = 16 + 2 + 8; // view_back_q8, view_quality, view_seq8
constexpr unsigned kOtherWeaponBits = 8 + 3 + 1 + 1 + 2 + 4; // weapon, phase, firing, ads, reload seq, burst seq
constexpr unsigned kOtherTeamBits = 2; // M2a w3 (G1, docs/design/m2a-w3.md): OtherSoldier::team
constexpr unsigned kOtherBitsV2 = kOtherBits + kOtherWeaponBits + kOtherTeamBits;
static_assert(kOtherWeaponBits == 19 && kOtherBitsV2 == 180); // m2a.md §7.3: 19 more bits; W3.0 G1: + 2 team bits, 180 per other
constexpr uint8_t kKnownWeaponFlags = weapon_flags::kExactAim | weapon_flags::kInfiniteMagazine;
constexpr unsigned kWireSlotBits = 7; // shot and combat slots < kMaxSlots
static_assert((size_t(1) << kWireSlotBits) == kMaxSlots);
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
	// v2: the view sample (m2a.md §6.2).
	uint16_t view_back_q8 = 0;
	uint8_t view_quality = 0; // 2 bits
	uint8_t view_seq8 = 0;
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
	w.view_back_q8 = p_c.view_back_q8;
	w.view_quality = uint8_t(p_c.view_quality & 3);
	w.view_seq8 = p_c.view_seq8;
	return w;
}

void write_command_fields(BitWriter &r_w, const WireCommand &p_c) {
	r_w.write_bits(p_c.buttons, 24);
	r_w.write_signed(p_c.forward, 8);
	r_w.write_signed(p_c.right, 8);
	r_w.write_bits(p_c.yaw, 16);
	r_w.write_signed(p_c.pitch, quant::kCommandPitchBits);
	r_w.write_bits(p_c.pose, 2);
	r_w.write_bits(p_c.view_back_q8, 16);
	r_w.write_bits(p_c.view_quality, 2);
	r_w.write_bits(p_c.view_seq8, 8);
}

bool read_command_fields(BitReader &r_r, sim::Command &r_c) {
	r_c.buttons = uint32_t(r_r.read_bits(24));
	r_c.move_forward = int8_t(r_r.read_signed(8));
	r_c.move_right = int8_t(r_r.read_signed(8));
	r_c.yaw = uint16_t(r_r.read_bits(16));
	r_c.pitch = int16_t(r_r.read_signed(quant::kCommandPitchBits));
	const uint64_t pose = r_r.read_bits(2);
	r_c.view_back_q8 = uint16_t(r_r.read_bits(16));
	r_c.view_quality = uint8_t(r_r.read_bits(2)); // ViewQuality: every 2-bit code is a value
	r_c.view_seq8 = uint8_t(r_r.read_bits(8));
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

// ---- M2a v2 quantisers (nt) ------------------------------------------------------------------------------------

// Octahedral unit vectors (x, y on the square, z folded): p_bits per axis. Encoded in double; the decoded vector is
// normalised and re-encodes to a code with the same decoded vector (the fuzz idempotence check covers it).
struct Octa {
	uint32_t u = 0;
	uint32_t v = 0;
};

double sign_nonneg(double p_x) {
	return p_x < 0.0 ? -1.0 : 1.0;
}

Octa octa_encode(const sim::Vec3 &p_dir, unsigned p_bits) {
	const double top = double((1u << p_bits) - 1u);
	double x = p_dir[0], y = p_dir[1], z = p_dir[2];
	const double n = std::fabs(x) + std::fabs(y) + std::fabs(z);
	if (!(n > 0.0) || !std::isfinite(n)) {
		x = 0.0;
		y = 0.0;
		z = 1.0;
	} else {
		x /= n;
		y /= n;
		z /= n;
	}
	if (z < 0.0) {
		const double fx = (1.0 - std::fabs(y)) * sign_nonneg(x);
		const double fy = (1.0 - std::fabs(x)) * sign_nonneg(y);
		x = fx;
		y = fy;
	}
	const auto code = [&](double p_c) {
		const double t = (std::clamp(p_c, -1.0, 1.0) + 1.0) * 0.5 * top + 0.5;
		return uint32_t(std::min(t, top));
	};
	return { code(x), code(y) };
}

sim::Vec3 octa_decode(Octa p_code, unsigned p_bits) {
	const uint32_t top_code = (1u << p_bits) - 1u;
	const double top = double(top_code);
	double x = double(std::min(p_code.u, top_code)) / top * 2.0 - 1.0;
	double y = double(std::min(p_code.v, top_code)) / top * 2.0 - 1.0;
	const double z = 1.0 - std::fabs(x) - std::fabs(y);
	if (z < 0.0) {
		const double fx = (1.0 - std::fabs(y)) * sign_nonneg(x);
		const double fy = (1.0 - std::fabs(x)) * sign_nonneg(y);
		x = fx;
		y = fy;
	}
	const double len = std::sqrt(x * x + y * y + z * z); // >= 1/sqrt(3) on the octahedron
	return { float(x / len), float(y / len), float(z / len) };
}

void write_octa(BitWriter &r_w, const sim::Vec3 &p_dir, unsigned p_bits) {
	const Octa o = octa_encode(p_dir, p_bits);
	r_w.write_bits(o.u, p_bits);
	r_w.write_bits(o.v, p_bits);
}

sim::Vec3 read_octa(BitReader &r_r, unsigned p_bits) {
	Octa o;
	o.u = uint32_t(r_r.read_bits(p_bits));
	o.v = uint32_t(r_r.read_bits(p_bits));
	return octa_decode(o, p_bits);
}

// Shot origins and combat positions: 20 bits per axis, 1/256 m from the frame's minimum (a 4,096 m cube).
constexpr unsigned kFramePositionBits = 20;
constexpr double kFrameStep = 1.0 / 256.0;
constexpr uint32_t kFrameMaxCode = (1u << kFramePositionBits) - 1u;

// The codes of p_v in the frame; false when an axis lies outside it (r_codes then holds the clamped codes).
bool frame_codes(const sim::Vec3 &p_v, const ShotQuantization &p_frame, std::array<uint32_t, 3> &r_codes) {
	bool inside = true;
	for (size_t i = 0; i < 3; ++i) {
		const double t = (double(p_v[i]) - double(p_frame.origin_min[i])) / kFrameStep + 0.5;
		if (!(t >= 0.0) || !(t < double(kFrameMaxCode) + 1.0)) {
			inside = false;
		}
		r_codes[i] = !(t > 0.0) ? 0u : t >= double(kFrameMaxCode) ? kFrameMaxCode : uint32_t(t);
	}
	return inside;
}

sim::Vec3 frame_decode(const std::array<uint32_t, 3> &p_codes, const ShotQuantization &p_frame) {
	sim::Vec3 v{};
	for (size_t i = 0; i < 3; ++i) {
		v[i] = float(double(p_frame.origin_min[i]) + double(std::min(p_codes[i], kFrameMaxCode)) * kFrameStep);
	}
	return v;
}

void write_frame(BitWriter &r_w, const std::array<uint32_t, 3> &p_codes) {
	for (const uint32_t c : p_codes) {
		r_w.write_bits(c, kFramePositionBits);
	}
}

sim::Vec3 read_frame(BitReader &r_r, const ShotQuantization &p_frame) {
	std::array<uint32_t, 3> c{};
	for (uint32_t &x : c) {
		x = uint32_t(r_r.read_bits(kFramePositionBits));
	}
	return frame_decode(c, p_frame);
}

// Event times: whole ticks back from the message's server tick, and the place in that tick in 1/65,536.
constexpr unsigned kTickFractionBits = 16;
constexpr int64_t kTickFractionScale = int64_t(1) << kTickFractionBits;

struct WireTime {
	uint32_t back = 0; // ticks before the message's server tick (0 = that tick)
	uint32_t fraction = 0; // 0 .. 65535
};

// False when p_time lies after the server tick's end or before tick 1's start (time 0); r_time is then clamped.
bool wire_time(sim::weapons::SimTime p_time, sim::Tick p_server_tick, WireTime &r_time) {
	using sim::weapons::kTimeUnitsPerTick;
	r_time = {};
	if (p_server_tick == 0) {
		return false;
	}
	const sim::weapons::SimTime end = sim::weapons::tick_end(p_server_tick);
	bool inside = true;
	sim::weapons::SimTime t = p_time;
	if (t >= end) {
		t = end - 1;
		inside = false;
	}
	if (t < 0) {
		t = 0;
		inside = false;
	}
	const sim::Tick tick = sim::weapons::tick_containing(t);
	r_time.back = uint32_t(p_server_tick - std::min(tick, p_server_tick));
	const int64_t within = t - sim::weapons::tick_start(p_server_tick - r_time.back);
	const int64_t f = (within * kTickFractionScale + kTimeUnitsPerTick / 2) / kTimeUnitsPerTick;
	r_time.fraction = uint32_t(std::clamp<int64_t>(f, 0, kTickFractionScale - 1));
	return inside;
}

sim::weapons::SimTime time_of(const WireTime &p_time, sim::Tick p_server_tick) {
	using sim::weapons::kTimeUnitsPerTick;
	return sim::weapons::tick_start(p_server_tick - p_time.back) +
			(int64_t(p_time.fraction) * kTimeUnitsPerTick + kTickFractionScale / 2) / kTickFractionScale;
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
	// v2: the 3p weapon (m2a.md §7.3).
	r_w.write_bits(p_o.weapon, 8);
	r_w.write_bits(p_o.weapon_phase, 3);
	r_w.write_bool(p_o.firing);
	r_w.write_bool(p_o.ads);
	r_w.write_bits(p_o.reload_seq, 2);
	r_w.write_bits(p_o.burst_seq, 4);
	r_w.write_bits(p_o.team, 2); // M2a w3 (G1)
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
	r_o.weapon = uint8_t(r_r.read_bits(8));
	r_o.weapon_phase = uint8_t(r_r.read_bits(3));
	r_o.firing = r_r.read_bool();
	r_o.ads = r_r.read_bool();
	r_o.reload_seq = uint8_t(r_r.read_bits(2));
	r_o.burst_seq = uint8_t(r_r.read_bits(4));
	const uint64_t team = r_r.read_bits(2); // M2a w3 (G1)
	if (team > kMaxTeam) {
		return false; // M2a w3 (G1): team 3 does not exist
	}
	r_o.team = uint8_t(team); // M2a w3 (G1)
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

// ---- M2a v2 dev payloads at full precision (DebugReport: TargetInfo rows, the Trace's gunplay ticks) -------------

bool finite3(const sim::Vec3 &p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
}

void write_hit_target(BitWriter &r_w, const sim::weapons::HitTarget &p_t) {
	r_w.write_bits(p_t.id, 32);
	r_w.write_bits(p_t.team, 32);
	write_vec3(r_w, p_t.bound_center);
	r_w.write_f32(p_t.bound_radius);
	r_w.write_bool(p_t.spawn_protected);
	const uint8_t count = uint8_t(std::min<size_t>(p_t.capsule_count, sim::weapons::kMaxZoneCapsules));
	r_w.write_bits(count, 5);
	for (uint8_t i = 0; i < count; ++i) {
		const sim::weapons::ZoneCapsule &c = p_t.capsules[i];
		write_vec3(r_w, c.a);
		write_vec3(r_w, c.b);
		r_w.write_f32(c.radius);
		r_w.write_bits(uint8_t(c.zone) & 3, 2);
		r_w.write_bits(c.capsule, 8);
		r_w.write_bits(c.property, 8);
		r_w.write_bits(c.flags, 8);
	}
}

bool read_hit_target(BitReader &r_r, sim::weapons::HitTarget &r_t) {
	r_t = sim::weapons::HitTarget{};
	r_t.id = uint32_t(r_r.read_bits(32));
	r_t.team = uint32_t(r_r.read_bits(32));
	bool ok = read_vec3(r_r, r_t.bound_center);
	r_t.bound_radius = r_r.read_f32();
	r_t.spawn_protected = r_r.read_bool();
	const uint64_t count = r_r.read_bits(5);
	ok = ok && std::isfinite(r_t.bound_radius) && count <= sim::weapons::kMaxZoneCapsules;
	if (!ok || r_r.failed()) {
		return false;
	}
	r_t.capsule_count = uint8_t(count);
	for (uint8_t i = 0; i < r_t.capsule_count; ++i) {
		sim::weapons::ZoneCapsule &c = r_t.capsules[i];
		ok = read_vec3(r_r, c.a) && ok;
		ok = read_vec3(r_r, c.b) && ok;
		c.radius = r_r.read_f32();
		c.zone = sim::weapons::HitZone(r_r.read_bits(2)); // 4 zones, every code valid
		c.capsule = uint8_t(r_r.read_bits(8));
		c.property = uint8_t(r_r.read_bits(8));
		c.flags = uint8_t(r_r.read_bits(8));
		ok = ok && std::isfinite(c.radius);
	}
	return ok;
}
static_assert(sim::weapons::kHitZoneCount == 4 && sim::weapons::kMaxZoneCapsules < 32);

constexpr size_t kTargetRowMinBits = 8 + 8 + 96 + 16 + 32 + 32 + 96 + 32 + 1 + 5;

void write_shot_full(BitWriter &r_w, const sim::weapons::ShotEvent &p_s) {
	r_w.write_bits(uint64_t(p_s.time), 64);
	r_w.write_bits(uint64_t(p_s.spawn_time), 64);
	r_w.write_bits(p_s.seed, 64);
	r_w.write_bits(p_s.player_slot, 32);
	r_w.write_bits(p_s.shot_index, 32);
	r_w.write_bits(p_s.def, 16);
	r_w.write_bits(p_s.weapon_slot, 8);
	r_w.write_bits(p_s.pellets, 8);
	write_vec3(r_w, p_s.origin);
	write_vec3(r_w, p_s.aim);
	write_vec3(r_w, p_s.direction);
	r_w.write_f32(p_s.spread);
	r_w.write_f32(p_s.pellet_cone);
	r_w.write_f32(p_s.recoil.pitch);
	r_w.write_f32(p_s.recoil.yaw);
	r_w.write_bool(p_s.tracer);
	r_w.write_f32(p_s.power);
}
constexpr size_t kShotFullBits = 64 * 3 + 32 * 2 + 16 + 8 + 8 + 9 * 32 + 4 * 32 + 1 + 32;

bool read_shot_full(BitReader &r_r, sim::weapons::ShotEvent &r_s) {
	r_s = sim::weapons::ShotEvent{};
	r_s.time = int64_t(r_r.read_bits(64));
	r_s.spawn_time = int64_t(r_r.read_bits(64));
	r_s.seed = r_r.read_bits(64);
	r_s.player_slot = uint32_t(r_r.read_bits(32));
	r_s.shot_index = uint32_t(r_r.read_bits(32));
	r_s.def = uint16_t(r_r.read_bits(16));
	r_s.weapon_slot = uint8_t(r_r.read_bits(8));
	r_s.pellets = uint8_t(r_r.read_bits(8));
	bool ok = read_vec3(r_r, r_s.origin);
	ok = read_vec3(r_r, r_s.aim) && ok;
	ok = read_vec3(r_r, r_s.direction) && ok;
	r_s.spread = r_r.read_f32();
	r_s.pellet_cone = r_r.read_f32();
	r_s.recoil.pitch = r_r.read_f32();
	r_s.recoil.yaw = r_r.read_f32();
	r_s.tracer = r_r.read_bool();
	r_s.power = r_r.read_f32();
	return ok && std::isfinite(r_s.spread) && std::isfinite(r_s.pellet_cone) && std::isfinite(r_s.recoil.pitch) && std::isfinite(r_s.recoil.yaw) &&
			std::isfinite(r_s.power) && r_s.weapon_slot < sim::weapons::kMaxWeaponSlots && r_s.pellets >= 1 &&
			r_s.pellets <= sim::weapons::kMaxPellets;
}

void write_damage_full(BitWriter &r_w, const sim::weapons::DamageEvent &p_d) {
	r_w.write_bits(uint64_t(p_d.time), 64);
	r_w.write_bits(p_d.attacker_slot, 32);
	r_w.write_bits(p_d.target, 32);
	r_w.write_bits(p_d.shot_index, 32);
	r_w.write_bits(p_d.def, 16);
	r_w.write_bits(p_d.pellet, 8);
	r_w.write_bits(uint8_t(p_d.kind) & 3, 2);
	r_w.write_bits(uint8_t(p_d.zone) & 3, 2);
	r_w.write_bits(p_d.capsule, 8);
	r_w.write_f32(p_d.distance);
	r_w.write_f32(p_d.base);
	r_w.write_f32(p_d.multiplier);
	r_w.write_f32(p_d.penetration);
	r_w.write_f32(p_d.amount);
}
constexpr size_t kDamageFullBits = 64 + 32 * 3 + 16 + 8 + 2 + 2 + 8 + 5 * 32;

bool read_damage_full(BitReader &r_r, sim::weapons::DamageEvent &r_d) {
	r_d = sim::weapons::DamageEvent{};
	r_d.time = int64_t(r_r.read_bits(64));
	r_d.attacker_slot = uint32_t(r_r.read_bits(32));
	r_d.target = uint32_t(r_r.read_bits(32));
	r_d.shot_index = uint32_t(r_r.read_bits(32));
	r_d.def = uint16_t(r_r.read_bits(16));
	r_d.pellet = uint8_t(r_r.read_bits(8));
	r_d.kind = sim::weapons::DamageKind(r_r.read_bits(2)); // 4 kinds, every code valid
	r_d.zone = sim::weapons::HitZone(r_r.read_bits(2));
	r_d.capsule = uint8_t(r_r.read_bits(8));
	r_d.distance = r_r.read_f32();
	r_d.base = r_r.read_f32();
	r_d.multiplier = r_r.read_f32();
	r_d.penetration = r_r.read_f32();
	r_d.amount = r_r.read_f32();
	return std::isfinite(r_d.distance) && std::isfinite(r_d.base) && std::isfinite(r_d.multiplier) && std::isfinite(r_d.penetration) &&
			std::isfinite(r_d.amount);
}
static_assert(uint8_t(sim::weapons::DamageKind::DudBlast) == 3);

void write_detonation_full(BitWriter &r_w, const sim::weapons::DetonationEvent &p_d) {
	r_w.write_bits(uint64_t(p_d.time), 64);
	write_vec3(r_w, p_d.position);
	write_vec3(r_w, p_d.normal);
	r_w.write_bits(p_d.player_slot, 32);
	r_w.write_bits(p_d.shot_index, 32);
	r_w.write_bits(p_d.def, 16);
	r_w.write_bool(p_d.dud);
	r_w.write_bool(p_d.timeout);
}
constexpr size_t kDetonationFullBits = 64 + 6 * 32 + 32 + 32 + 16 + 2;

bool read_detonation_full(BitReader &r_r, sim::weapons::DetonationEvent &r_d) {
	r_d = sim::weapons::DetonationEvent{};
	r_d.time = int64_t(r_r.read_bits(64));
	bool ok = read_vec3(r_r, r_d.position);
	ok = read_vec3(r_r, r_d.normal) && ok;
	r_d.player_slot = uint32_t(r_r.read_bits(32));
	r_d.shot_index = uint32_t(r_r.read_bits(32));
	r_d.def = uint16_t(r_r.read_bits(16));
	r_d.dud = r_r.read_bool();
	r_d.timeout = r_r.read_bool();
	return ok;
}

// One traced tick's gunplay. Counts in 4 / 4 / 4 / 2 / 3 bits, each bounded by its kMaxTrace*; the encoder cuts a
// longer list and adds what it cut to r_dropped (the per-tick bounds of m2a.md §1).
constexpr size_t kGunplayTickMinBits = 32 + 4 + 4 + 4 + 2 + 3;
static_assert(kMaxTraceShots < 16 && kMaxTraceProjectiles < 16 && kMaxTraceDamage < 16 && kMaxTraceDetonations < 4 && kMaxTraceDeaths < 8);

void write_gunplay_tick(BitWriter &r_w, const TraceGunplayTick &p_t, uint64_t &r_dropped) {
	const size_t shots = std::min(p_t.shots.size(), kMaxTraceShots);
	const size_t both = std::min(p_t.projectiles.size(), p_t.projectile_shots.size());
	const size_t projectiles = std::min(both, kMaxTraceProjectiles);
	const size_t damage = std::min(p_t.damage.size(), kMaxTraceDamage);
	const size_t detonations = std::min(p_t.detonations.size(), kMaxTraceDetonations);
	const size_t deaths = std::min(p_t.deaths.size(), kMaxTraceDeaths);
	r_dropped += (p_t.shots.size() - shots) + (std::max(p_t.projectiles.size(), p_t.projectile_shots.size()) - projectiles) + (p_t.damage.size() - damage) +
			(p_t.detonations.size() - detonations) + (p_t.deaths.size() - deaths);
	r_w.write_bits(p_t.tick, 32);
	r_w.write_bits(shots, 4);
	r_w.write_bits(projectiles, 4);
	r_w.write_bits(damage, 4);
	r_w.write_bits(detonations, 2);
	r_w.write_bits(deaths, 3);
	for (size_t i = 0; i < shots; ++i) {
		write_shot_full(r_w, p_t.shots[i]);
	}
	for (size_t i = 0; i < projectiles; ++i) {
		write_vec3(r_w, p_t.projectiles[i]);
		r_w.write_bits(p_t.projectile_shots[i], 32);
	}
	for (size_t i = 0; i < damage; ++i) {
		write_damage_full(r_w, p_t.damage[i]);
	}
	for (size_t i = 0; i < detonations; ++i) {
		write_detonation_full(r_w, p_t.detonations[i]);
	}
	for (size_t i = 0; i < deaths; ++i) {
		r_w.write_bits(p_t.deaths[i].victim, 8);
		r_w.write_bits(p_t.deaths[i].killer, 8);
		r_w.write_bits(p_t.deaths[i].shot_index, 32);
	}
}

bool read_gunplay_tick(BitReader &r_r, TraceGunplayTick &r_t) {
	r_t = TraceGunplayTick{};
	r_t.tick = sim::Tick(r_r.read_bits(32));
	const size_t shots = size_t(r_r.read_bits(4));
	const size_t projectiles = size_t(r_r.read_bits(4));
	const size_t damage = size_t(r_r.read_bits(4));
	const size_t detonations = size_t(r_r.read_bits(2));
	const size_t deaths = size_t(r_r.read_bits(3));
	if (r_r.failed() || shots > kMaxTraceShots || projectiles > kMaxTraceProjectiles || damage > kMaxTraceDamage || detonations > kMaxTraceDetonations ||
			deaths > kMaxTraceDeaths) {
		return false;
	}
	const size_t need = shots * kShotFullBits + projectiles * (96 + 32) + damage * kDamageFullBits + detonations * kDetonationFullBits + deaths * 48;
	if (need > r_r.bits_left()) {
		return false;
	}
	bool ok = true;
	r_t.shots.resize(shots);
	for (sim::weapons::ShotEvent &s : r_t.shots) {
		ok = read_shot_full(r_r, s) && ok;
	}
	r_t.projectiles.resize(projectiles);
	r_t.projectile_shots.resize(projectiles);
	for (size_t i = 0; i < projectiles; ++i) {
		ok = read_vec3(r_r, r_t.projectiles[i]) && ok;
		r_t.projectile_shots[i] = uint32_t(r_r.read_bits(32));
	}
	r_t.damage.resize(damage);
	for (sim::weapons::DamageEvent &d : r_t.damage) {
		ok = read_damage_full(r_r, d) && ok;
	}
	r_t.detonations.resize(detonations);
	for (sim::weapons::DetonationEvent &d : r_t.detonations) {
		ok = read_detonation_full(r_r, d) && ok;
	}
	r_t.deaths.resize(deaths);
	for (TraceDeath &d : r_t.deaths) {
		d.victim = uint8_t(r_r.read_bits(8));
		d.killer = uint8_t(r_r.read_bits(8));
		d.shot_index = uint32_t(r_r.read_bits(32));
	}
	return ok && !r_r.failed();
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
		case MessageType::SnapshotOthers: // v2
		case MessageType::ShotEvents:
		case MessageType::CombatEvents:
		case MessageType::KitSelect:
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
	// v2 (m2a.md §7.1).
	w.write_bits(p_message.match_seed, 64);
	w.write_bits(p_message.weapon_hash, 64);
	w.write_bits(p_message.weapon_flags, 8);
	w.write_bits(p_message.kit_hash, 64);
	w.write_bits(p_message.max_rewind_ticks, 8);
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
	m.match_seed = r.read_bits(64);
	m.weapon_hash = r.read_bits(64);
	m.weapon_flags = uint8_t(r.read_bits(8));
	m.kit_hash = r.read_bits(64);
	m.max_rewind_ticks = uint8_t(r.read_bits(8));
	if (r.failed()) {
		return fail("MatchInfo", "truncated");
	}
	if ((m.weapon_flags & ~kKnownWeaponFlags) != 0) {
		return fail("MatchInfo", "unknown weapon flags");
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
	w.write_bits(p_message.kit, 8); // v2 (m2a.md §7.4)
	w.write_bits(p_message.team, 8);
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
	const uint64_t kit = r.read_bits(8);
	const uint64_t team = r.read_bits(8);
	if (r.failed()) {
		return fail("Spawned", "truncated");
	}
	if (slot >= kMaxSlots) {
		return fail("Spawned", "slot out of range");
	}
	if (!state_ok) {
		return fail("Spawned", "state field out of range");
	}
	if (kit >= kMaxKits || team > kMaxTeam) {
		return fail("Spawned", "kit or team out of range");
	}
	m.slot = uint8_t(slot);
	m.kit = uint8_t(kit);
	m.team = uint8_t(team);
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
	w.write_bits(p_message.slot, 8); // v2: SpawnTarget / TargetInfo (m2a.md §7.4)
	w.write_bits(uint8_t(p_message.pose), 8);
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
	const uint64_t slot = r.read_bits(8);
	const uint64_t pose = r.read_bits(8);
	if (r.failed()) {
		return fail("Debug", "truncated");
	}
	if (kind < uint8_t(DebugKind::Teleport) || kind > uint8_t(DebugKind::TargetInfo)) {
		return fail("Debug", "unknown kind");
	}
	if (slot >= kMaxSlots || !valid_pose(pose)) {
		return fail("Debug", "slot or pose out of range");
	}
	m.slot = uint8_t(slot);
	m.pose = sim::Pose(pose);
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
	BitWriter w = begin(MessageType::DebugReport, kMaxDebugReportBytes);
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
	// v2 (m2a.md §7.4): TargetInfo rows, then the Trace's gunplay ticks and the count of what was dropped.
	const size_t targets = std::min(p_message.targets.size(), kMaxTargetRows);
	w.write_varint(targets);
	for (size_t i = 0; i < targets; ++i) {
		const TargetInfoRow &row = p_message.targets[i];
		w.write_bits(row.slot, 8);
		w.write_bits(uint8_t(row.pose), 8);
		write_vec3(w, row.feet);
		w.write_bits(row.yaw, 16);
		write_hit_target(w, row.target);
	}
	const size_t ticks = std::min<size_t>(p_message.trace_gunplay.size(), kMaxTraceTicks);
	uint64_t dropped = p_message.trace_dropped;
	w.write_varint(ticks);
	for (size_t i = 0; i < ticks; ++i) {
		write_gunplay_tick(w, p_message.trace_gunplay[i], dropped);
	}
	w.write_bits(std::min<uint64_t>(dropped, 0xFFFFFFFFull), 32);
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
	const uint64_t targets = r.read_varint();
	if (r.failed() || targets > kMaxTargetRows || targets * kTargetRowMinBits > r.bits_left()) {
		return fail("DebugReport", "bad target row count");
	}
	m.targets.resize(size_t(targets));
	for (TargetInfoRow &row : m.targets) {
		const uint64_t slot = r.read_bits(8);
		const uint64_t pose = r.read_bits(8);
		bool ok = read_vec3(r, row.feet);
		row.yaw = uint16_t(r.read_bits(16));
		ok = read_hit_target(r, row.target) && ok;
		if (r.failed()) {
			return fail("DebugReport", "truncated");
		}
		if (!ok || slot >= kMaxSlots || !valid_pose(pose)) {
			return fail("DebugReport", "target row field out of range");
		}
		row.slot = uint8_t(slot);
		row.pose = sim::Pose(pose);
	}
	const uint64_t ticks = r.read_varint();
	if (r.failed() || ticks > kMaxTraceTicks || ticks * kGunplayTickMinBits > r.bits_left()) {
		return fail("DebugReport", "bad gunplay trace count");
	}
	m.trace_gunplay.resize(size_t(ticks));
	for (TraceGunplayTick &t : m.trace_gunplay) {
		if (!read_gunplay_tick(r, t)) {
			return fail("DebugReport", r.failed() ? "truncated" : "gunplay trace field out of range");
		}
	}
	m.trace_dropped = uint32_t(r.read_bits(32));
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
	w.write_bits(p_packet.others_ack_bits, 32); // v2 (m2a.md §7.2)
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
	p.others_ack_bits = uint32_t(r.read_bits(32));
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
		// v2: the own weapons travel with the own state (m2a.md §7.3).
		w.write_bool(p_snapshot.own_weapons.has_value());
		if (p_snapshot.own_weapons) {
			write_weapons_wire(w, *p_snapshot.own_weapons);
		}
	}
	// As many others as fit beside the count byte, in the caller's (nearest-first) order.
	const size_t used = w.bit_count() + 8;
	const size_t room = used < kMaxDatagramBytes * 8 ? (kMaxDatagramBytes * 8 - used) / kOtherBitsV2 : 0;
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
		if (r.read_bool()) {
			WeaponsWire weapons;
			const core::Status st = read_weapons_wire(r, weapons);
			if (!st.ok()) {
				return fail("Snapshot", r.failed() ? "truncated" : "own weapons field out of range");
			}
			s.own_weapons = weapons;
		}
	}
	const uint64_t count = r.read_bits(8);
	if (r.failed()) {
		return fail("Snapshot", "truncated");
	}
	if (count > kMaxSlots || count * kOtherBitsV2 > r.bits_left()) {
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


// ---- M2a protocol v2 (nt; docs/design/m2a.md §7) ------------------------------------------------------------------

void set_other_weapon(OtherSoldier &r_other, uint16_t p_def, sim::weapons::WeaponPhase p_phase, bool p_firing, bool p_ads, uint32_t p_reload_count,
		uint32_t p_burst_count) {
	r_other.weapon = p_def < kMaxWireDefs ? uint8_t(p_def) : uint8_t(0xFF);
	r_other.weapon_phase = uint8_t(uint8_t(p_phase) & 7u);
	r_other.firing = p_firing;
	r_other.ads = p_ads;
	r_other.reload_seq = uint8_t(p_reload_count & 3u);
	r_other.burst_seq = uint8_t(p_burst_count & 15u);
}

// M2a w3 (G1, docs/design/m2a-w3.md).
void set_other_team(OtherSoldier &r_other, uint8_t p_team) {
	r_other.team = p_team >= 1 && p_team <= kMaxTeam ? p_team : uint8_t(0);
}

// ---- acks (m2a.md §7.2) ------------------------------------------------------------------------------------------

bool snapshot_acked(const CommandPacket &p_packet, uint16_t p_seq) {
	if (p_seq == p_packet.newest_snapshot) {
		return true;
	}
	if (seq_newer(p_seq, p_packet.newest_snapshot)) {
		return false;
	}
	const uint16_t back = uint16_t(p_packet.newest_snapshot - 1u - p_seq); // bit i = newest - 1 - i
	return back < 32 && ((p_packet.snapshot_ack_bits >> back) & 1u) != 0;
}

bool others_acked(const CommandPacket &p_packet, uint16_t p_seq) {
	if (seq_newer(p_seq, p_packet.newest_snapshot)) {
		return false;
	}
	const uint16_t back = uint16_t(p_packet.newest_snapshot - p_seq); // bit i = newest - i (bit 0 = newest itself)
	return back < 32 && ((p_packet.others_ack_bits >> back) & 1u) != 0;
}

// ---- SnapshotOthers (ch0) ----------------------------------------------------------------------------------------
// Body: u16 seq | u32 server tick | u8 count (<= kMaxSlots - 1) | others (178 bits each, as in the Snapshot). Up to 53
// others fit one datagram, more than the Snapshot leaves out for 128 slots.
// M2a w3 (G1): with the 2 team bits an other is 180 bits, and 52 fit.

std::vector<uint8_t> encode_snapshot_others(const SnapshotOthers &p_message, size_t *r_dropped) {
	BitWriter w = begin(MessageType::SnapshotOthers, kMaxDatagramBytes);
	w.write_bits(p_message.seq, 16);
	w.write_bits(p_message.server_tick, 32);
	const size_t used = w.bit_count() + 8;
	const size_t room = (kMaxDatagramBytes * 8 - used) / kOtherBitsV2;
	const size_t count = std::min({ p_message.others.size(), room, kMaxSlots - 1 });
	w.write_bits(count, 8);
	for (size_t i = 0; i < count; ++i) {
		write_other(w, p_message.others[i]);
	}
	if (r_dropped) {
		*r_dropped = p_message.others.size() - count;
	}
	return finish(w);
}

core::Result<SnapshotOthers> decode_snapshot_others(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::SnapshotOthers)) {
		return fail("SnapshotOthers", "wrong type");
	}
	if (p_datagram.size() > kMaxDatagramBytes) {
		return fail("SnapshotOthers", "larger than one datagram"); // unreliable ch0: one unfragmented datagram
	}
	SnapshotOthers m;
	m.seq = uint16_t(r.read_bits(16));
	m.server_tick = sim::Tick(r.read_bits(32));
	const uint64_t count = r.read_bits(8);
	if (r.failed()) {
		return fail("SnapshotOthers", "truncated");
	}
	if (count > kMaxSlots - 1 || count * kOtherBitsV2 > r.bits_left()) {
		return fail("SnapshotOthers", "others count out of range");
	}
	std::array<bool, kMaxSlots> seen{};
	m.others.resize(size_t(count));
	for (OtherSoldier &o : m.others) {
		if (!read_other(r, o)) {
			return fail("SnapshotOthers", r.failed() ? "truncated" : "other soldier field out of range");
		}
		if (seen[o.slot]) {
			return fail("SnapshotOthers", "slot named twice");
		}
		seen[o.slot] = true;
	}
	return close(r, std::move(m), "SnapshotOthers");
}

// ---- ShotEvents (ch0) --------------------------------------------------------------------------------------------
// Body: u32 server tick | u8 count (<= kMaxShotsPerDatagram) | per shot: u7 slot | shot index (u32 for a shooter's
// first shot in the datagram, else u4 delta 1..15 from its previous one, 0 = escape followed by u32) | u8 def | u3
// ticks back | u16 tick fraction | 3 x u20 origin | 2 x u16 octahedral direction | u6 pellets - 1 | u1 tracer
// [tracer: 2 x u12 octahedral aim] [pellets > 1: u13 pellet cone, 1/64 degree] | u1 power present [u12 power, 0..
// kMaxPower]. 138 bits without a tracer, 162 with one: ~17 / 20 B; 68 untraced shots fit one datagram.

namespace {

constexpr unsigned kShotDirBits = 16;
constexpr unsigned kShotAimBits = 12;
constexpr unsigned kConeBits = 13; // 1/64 degree: 0 .. 127.98, covers sim::weapons::kMaxAngle (90)
constexpr double kConeScale = 64.0;
constexpr unsigned kPowerBits = 12;
constexpr size_t kShotMinBits = 7 + 4 + 8 + 3 + kTickFractionBits + 3 * kFramePositionBits + 2 * kShotDirBits + 6 + 1 + 1;
static_assert(kShotMinBits == 138);
static_assert(sim::weapons::kMaxPellets == 64, "pellets travel as pellets - 1 in 6 bits");
static_assert(sim::weapons::kMaxAngle < double(1u << kConeBits) / kConeScale);

struct ShotCodes {
	uint8_t slot = 0;
	uint32_t index = 0;
	uint8_t def = 0;
	WireTime time;
	std::array<uint32_t, 3> origin{};
	Octa direction;
	uint8_t pellets = 1;
	bool tracer = false;
	Octa aim;
	uint32_t cone = 0;
	bool has_power = false;
	uint32_t power = 0;
};

std::optional<ShotCodes> shot_codes(const RemoteShot &p_shot, const ShotQuantization &p_frame, sim::Tick p_server_tick) {
	ShotCodes c;
	if (p_shot.slot >= kMaxSlots || p_shot.def >= kMaxWireDefs || p_shot.pellets < 1 || p_shot.pellets > sim::weapons::kMaxPellets) {
		return std::nullopt;
	}
	if (!wire_time(p_shot.time, p_server_tick, c.time) || c.time.back > kMaxShotTicksBack) {
		return std::nullopt;
	}
	if (!frame_codes(p_shot.origin, p_frame, c.origin)) {
		return std::nullopt;
	}
	c.slot = p_shot.slot;
	c.index = p_shot.shot_index;
	c.def = uint8_t(p_shot.def);
	c.direction = octa_encode(p_shot.direction, kShotDirBits);
	c.pellets = p_shot.pellets;
	c.tracer = p_shot.tracer;
	if (c.tracer) {
		c.aim = octa_encode(p_shot.aim, kShotAimBits);
	}
	if (c.pellets > 1) {
		const double cone = std::isfinite(p_shot.pellet_cone) ? std::clamp(double(p_shot.pellet_cone), 0.0, double(sim::weapons::kMaxAngle)) : 0.0;
		c.cone = std::min(uint32_t(cone * kConeScale + 0.5), (1u << kConeBits) - 1u);
	}
	c.has_power = p_shot.power != 1.0f;
	if (c.has_power) {
		const double top = double((1u << kPowerBits) - 1u);
		const double power = std::isfinite(p_shot.power) ? std::clamp(double(p_shot.power), 0.0, double(sim::weapons::kMaxPower)) : 0.0;
		c.power = std::min(uint32_t(power / double(sim::weapons::kMaxPower) * top + 0.5), (1u << kPowerBits) - 1u);
	}
	return c;
}

RemoteShot shot_from_codes(const ShotCodes &p_c, const ShotQuantization &p_frame, sim::Tick p_server_tick) {
	RemoteShot s;
	s.slot = p_c.slot;
	s.shot_index = p_c.index;
	s.def = p_c.def;
	s.time = time_of(p_c.time, p_server_tick);
	s.origin = frame_decode(p_c.origin, p_frame);
	s.direction = octa_decode(p_c.direction, kShotDirBits);
	s.pellets = p_c.pellets;
	s.tracer = p_c.tracer;
	if (p_c.tracer) {
		s.aim = octa_decode(p_c.aim, kShotAimBits);
	}
	if (p_c.pellets > 1) {
		s.pellet_cone = float(double(p_c.cone) / kConeScale);
	}
	if (p_c.has_power) {
		s.power = float(double(p_c.power) / double((1u << kPowerBits) - 1u) * double(sim::weapons::kMaxPower));
	}
	return s;
}

// The bits of one shot after p_have_prev / p_prev (the shooter's previous shot in this datagram).
size_t shot_bits(const ShotCodes &p_c, bool p_have_prev, uint32_t p_prev) {
	size_t bits = kShotMinBits - 4;
	if (!p_have_prev) {
		bits += 32;
	} else {
		const uint32_t delta = p_c.index - p_prev;
		bits += (delta >= 1 && delta <= 15) ? 4 : 4 + 32;
	}
	bits += p_c.tracer ? 2 * kShotAimBits : 0;
	bits += p_c.pellets > 1 ? kConeBits : 0;
	bits += p_c.has_power ? kPowerBits : 0;
	return bits;
}

} // namespace

RemoteShot remote_shot_of(const sim::weapons::ShotEvent &p_shot) {
	RemoteShot s;
	s.slot = uint8_t(std::min<uint32_t>(p_shot.player_slot, 0xFF));
	s.shot_index = p_shot.shot_index;
	s.def = p_shot.def;
	s.time = p_shot.time;
	s.origin = p_shot.origin;
	s.direction = p_shot.direction;
	s.pellets = p_shot.pellets;
	s.tracer = p_shot.tracer;
	s.aim = p_shot.tracer ? p_shot.aim : sim::Vec3{};
	s.pellet_cone = p_shot.pellets > 1 ? p_shot.pellet_cone : 0.0f;
	s.power = p_shot.power;
	return s;
}

std::optional<RemoteShot> quantize_remote_shot(const RemoteShot &p_shot, const ShotQuantization &p_frame, sim::Tick p_server_tick) {
	const std::optional<ShotCodes> c = shot_codes(p_shot, p_frame, p_server_tick);
	if (!c) {
		return std::nullopt;
	}
	return shot_from_codes(*c, p_frame, p_server_tick);
}

std::vector<uint8_t> encode_shot_events(const ShotEvents &p_message, const ShotQuantization &p_frame, const sim::Vec3 &p_receiver_eye, size_t *r_dropped) {
	size_t dropped = 0;
	struct Item {
		ShotCodes codes;
		size_t source = 0;
		double distance2 = 0;
	};
	std::vector<Item> items;
	items.reserve(p_message.shots.size());
	for (size_t i = 0; i < p_message.shots.size(); ++i) {
		const RemoteShot &s = p_message.shots[i];
		const std::optional<ShotCodes> c = shot_codes(s, p_frame, p_message.server_tick);
		if (!c) {
			++dropped;
			continue;
		}
		bool repeated = false; // a shot named twice would make the datagram malformed: the later copy is dropped
		for (const Item &it : items) {
			repeated = repeated || (it.codes.slot == c->slot && it.codes.index == c->index);
		}
		if (repeated) {
			++dropped;
			continue;
		}
		double d2 = 0;
		for (size_t k = 0; k < 3; ++k) {
			const double d = double(s.origin[k]) - double(p_receiver_eye[k]);
			d2 += d * d;
		}
		items.push_back({ *c, i, std::isfinite(d2) ? d2 : 1e300 });
	}
	// Shooters nearest first (a shooter's nearest shot), ties by slot; each shooter's shots in the caller's order.
	std::array<double, kMaxSlots> shooter_d2;
	shooter_d2.fill(1e301);
	for (const Item &it : items) {
		shooter_d2[it.codes.slot] = std::min(shooter_d2[it.codes.slot], it.distance2);
	}
	std::stable_sort(items.begin(), items.end(), [&](const Item &p_a, const Item &p_b) {
		const double a = shooter_d2[p_a.codes.slot], b = shooter_d2[p_b.codes.slot];
		if (a != b) {
			return a < b;
		}
		return p_a.codes.slot < p_b.codes.slot;
	});
	// The prefix that fits.
	std::array<bool, kMaxSlots> have{};
	std::array<uint32_t, kMaxSlots> prev{};
	size_t bits = 8 + 32 + 8;
	size_t count = 0;
	for (; count < items.size() && count < kMaxShotsPerDatagram; ++count) {
		const ShotCodes &c = items[count].codes;
		const size_t b = shot_bits(c, have[c.slot], prev[c.slot]);
		if (bits + b > kMaxDatagramBytes * 8) {
			break;
		}
		bits += b;
		have[c.slot] = true;
		prev[c.slot] = c.index;
	}
	dropped += items.size() - count;
	if (r_dropped) {
		*r_dropped = dropped;
	}
	BitWriter w = begin(MessageType::ShotEvents, kMaxDatagramBytes);
	w.write_bits(p_message.server_tick, 32);
	w.write_bits(count, 8);
	have.fill(false);
	for (size_t i = 0; i < count; ++i) {
		const ShotCodes &c = items[i].codes;
		w.write_bits(c.slot, kWireSlotBits);
		if (!have[c.slot]) {
			w.write_bits(c.index, 32);
		} else {
			const uint32_t delta = c.index - prev[c.slot];
			if (delta >= 1 && delta <= 15) {
				w.write_bits(delta, 4);
			} else {
				w.write_bits(0, 4);
				w.write_bits(c.index, 32);
			}
		}
		have[c.slot] = true;
		prev[c.slot] = c.index;
		w.write_bits(c.def, 8);
		w.write_bits(c.time.back, 3);
		w.write_bits(c.time.fraction, kTickFractionBits);
		write_frame(w, c.origin);
		w.write_bits(c.direction.u, kShotDirBits);
		w.write_bits(c.direction.v, kShotDirBits);
		w.write_bits(uint32_t(c.pellets - 1), 6);
		w.write_bool(c.tracer);
		if (c.tracer) {
			w.write_bits(c.aim.u, kShotAimBits);
			w.write_bits(c.aim.v, kShotAimBits);
		}
		if (c.pellets > 1) {
			w.write_bits(c.cone, kConeBits);
		}
		w.write_bool(c.has_power);
		if (c.has_power) {
			w.write_bits(c.power, kPowerBits);
		}
	}
	return finish(w);
}

core::Result<ShotEvents> decode_shot_events(std::span<const uint8_t> p_datagram, const ShotQuantization &p_frame) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::ShotEvents)) {
		return fail("ShotEvents", "wrong type");
	}
	if (p_datagram.size() > kMaxDatagramBytes) {
		return fail("ShotEvents", "larger than one datagram"); // unreliable ch0: one unfragmented datagram
	}
	ShotEvents m;
	m.server_tick = sim::Tick(r.read_bits(32));
	const uint64_t count = r.read_bits(8);
	if (r.failed()) {
		return fail("ShotEvents", "truncated");
	}
	if (count > kMaxShotsPerDatagram || count * kShotMinBits > r.bits_left()) {
		return fail("ShotEvents", "shot count out of range");
	}
	std::array<bool, kMaxSlots> have{};
	std::array<uint32_t, kMaxSlots> prev{};
	m.shots.resize(size_t(count));
	for (RemoteShot &s : m.shots) {
		ShotCodes c;
		c.slot = uint8_t(r.read_bits(kWireSlotBits));
		if (!have[c.slot]) {
			c.index = uint32_t(r.read_bits(32));
		} else {
			const uint32_t delta = uint32_t(r.read_bits(4));
			c.index = delta != 0 ? prev[c.slot] + delta : uint32_t(r.read_bits(32));
		}
		const uint64_t def = r.read_bits(8);
		c.time.back = uint32_t(r.read_bits(3));
		c.time.fraction = uint32_t(r.read_bits(kTickFractionBits));
		for (uint32_t &x : c.origin) {
			x = uint32_t(r.read_bits(kFramePositionBits));
		}
		c.direction.u = uint32_t(r.read_bits(kShotDirBits));
		c.direction.v = uint32_t(r.read_bits(kShotDirBits));
		c.pellets = uint8_t(r.read_bits(6) + 1);
		c.tracer = r.read_bool();
		if (c.tracer) {
			c.aim.u = uint32_t(r.read_bits(kShotAimBits));
			c.aim.v = uint32_t(r.read_bits(kShotAimBits));
		}
		if (c.pellets > 1) {
			c.cone = uint32_t(r.read_bits(kConeBits));
		}
		c.has_power = r.read_bool();
		if (c.has_power) {
			c.power = uint32_t(r.read_bits(kPowerBits));
		}
		if (r.failed()) {
			return fail("ShotEvents", "truncated");
		}
		if (def >= kMaxWireDefs) {
			return fail("ShotEvents", "def out of range");
		}
		if (c.time.back >= m.server_tick) {
			return fail("ShotEvents", "shot time before tick 1");
		}
		if (have[c.slot] && c.index == prev[c.slot]) {
			return fail("ShotEvents", "shot index repeated");
		}
		c.def = uint8_t(def);
		have[c.slot] = true;
		prev[c.slot] = c.index;
		s = shot_from_codes(c, p_frame, m.server_tick);
	}
	// A shot named twice (the same shooter and index anywhere in the datagram) is malformed. n <= 128: pairwise.
	for (size_t i = 0; i < m.shots.size(); ++i) {
		for (size_t j = i + 1; j < m.shots.size(); ++j) {
			if (m.shots[i].slot == m.shots[j].slot && m.shots[i].shot_index == m.shots[j].shot_index) {
				return fail("ShotEvents", "shot named twice");
			}
		}
	}
	return close(r, std::move(m), "ShotEvents");
}

// ---- CombatEvents (ch1) ------------------------------------------------------------------------------------------
// Body: u32 server tick | varint hits | varint deaths | varint detonations (<= kMaxCombatEventsPerMessage together)
// | hits: u7 victim u7 shooter u32 shot u6 pellet u2 zone u24 damage u24 health after (1/100 hp) 3 x u20 position
//   2 x u8 octahedral normal, time | deaths: u7 victim u7 killer u8 def (255 = not a weapon) u2 zone u32 shot u1 headshot,
//   time | detonations: u7 shooter u32 shot u8 def 3 x u20 position 2 x u8 normal u1 dud, time
// time = varint ticks back from server_tick | u16 tick fraction. ~26 B per hit, ~11 B per death, ~19 B per detonation.

namespace {

constexpr unsigned kHpBits = 24;
constexpr double kHpScale = 100.0;
// 100,000 hp: below 2^17 hp a float holds 1/100 hp exactly enough that decode -> encode gives the same code.
constexpr uint32_t kHpMaxCode = 10'000'000;
static_assert(kHpMaxCode < (1u << kHpBits));
constexpr unsigned kNormalBits = 8;
constexpr size_t kTimeMinBits = 8 + kTickFractionBits;
constexpr size_t kHitMinBits = 7 + 7 + 32 + 6 + 2 + 2 * kHpBits + 3 * kFramePositionBits + 2 * kNormalBits + kTimeMinBits;
constexpr size_t kDeathMinBits = 7 + 7 + 8 + 2 + 32 + 1 + kTimeMinBits;
constexpr size_t kDetonationMinBits = 7 + 32 + 8 + 3 * kFramePositionBits + 2 * kNormalBits + 1 + kTimeMinBits;

uint32_t hp_code(float p_hp, size_t &r_clamped) {
	const double t = double(p_hp) * kHpScale + 0.5;
	if (!(t >= 0.0) || !(t < double(kHpMaxCode) + 1.0)) {
		++r_clamped;
	}
	return !(t > 0.0) ? 0u : t >= double(kHpMaxCode) ? kHpMaxCode : uint32_t(t);
}

float hp_value(uint32_t p_code) {
	return float(double(p_code) / kHpScale);
}

void write_event_time(BitWriter &r_w, sim::weapons::SimTime p_time, sim::Tick p_server_tick, size_t &r_clamped) {
	WireTime t;
	if (!wire_time(p_time, p_server_tick, t)) {
		++r_clamped;
	}
	r_w.write_varint(t.back);
	r_w.write_bits(t.fraction, kTickFractionBits);
}

bool read_event_time(BitReader &r_r, sim::Tick p_server_tick, sim::weapons::SimTime &r_time) {
	const uint64_t back = r_r.read_varint();
	const uint32_t fraction = uint32_t(r_r.read_bits(kTickFractionBits));
	if (r_r.failed() || back >= p_server_tick) {
		return false; // also refuses every time for server tick 0
	}
	r_time = time_of({ uint32_t(back), fraction }, p_server_tick);
	return true;
}

void write_position(BitWriter &r_w, const sim::Vec3 &p_v, const ShotQuantization &p_frame, size_t &r_clamped) {
	std::array<uint32_t, 3> c{};
	if (!frame_codes(p_v, p_frame, c)) {
		++r_clamped;
	}
	write_frame(r_w, c);
}

} // namespace

std::vector<uint8_t> encode_combat_events(const CombatEvents &p_message, const ShotQuantization &p_frame, size_t *r_clamped) {
	size_t clamped = 0;
	// The events that can travel (slots in 7 bits, detonation defs in 8), in order, up to the message bound.
	size_t budget = kMaxCombatEventsPerMessage;
	std::vector<const HitEvent *> hits;
	std::vector<const DeathEvent *> deaths;
	std::vector<const DetonationWire *> detonations;
	for (const HitEvent &h : p_message.hits) {
		if (h.victim >= kMaxSlots || h.shooter >= kMaxSlots || budget == 0) {
			++clamped;
			continue;
		}
		hits.push_back(&h);
		--budget;
	}
	for (const DeathEvent &d : p_message.deaths) {
		if (d.victim >= kMaxSlots || d.killer >= kMaxSlots || budget == 0) {
			++clamped;
			continue;
		}
		deaths.push_back(&d);
		--budget;
	}
	for (const DetonationWire &d : p_message.detonations) {
		if (d.shooter >= kMaxSlots || d.def >= kMaxWireDefs || budget == 0) {
			++clamped;
			continue;
		}
		detonations.push_back(&d);
		--budget;
	}
	BitWriter w = begin(MessageType::CombatEvents);
	w.write_bits(p_message.server_tick, 32);
	w.write_varint(hits.size());
	w.write_varint(deaths.size());
	w.write_varint(detonations.size());
	for (const HitEvent *h : hits) {
		w.write_bits(h->victim, kWireSlotBits);
		w.write_bits(h->shooter, kWireSlotBits);
		w.write_bits(h->shot_index, 32);
		if (h->pellet >= sim::weapons::kMaxPellets) {
			++clamped;
		}
		w.write_bits(std::min<uint32_t>(h->pellet, sim::weapons::kMaxPellets - 1), 6);
		w.write_bits(uint8_t(h->zone) & 3u, 2);
		w.write_bits(hp_code(h->damage, clamped), kHpBits);
		w.write_bits(hp_code(h->health_after, clamped), kHpBits);
		write_position(w, h->position, p_frame, clamped);
		write_octa(w, h->normal, kNormalBits);
		write_event_time(w, h->time, p_message.server_tick, clamped);
	}
	for (const DeathEvent *d : deaths) {
		w.write_bits(d->victim, kWireSlotBits);
		w.write_bits(d->killer, kWireSlotBits);
		if (d->def != sim::weapons::kNoWeapon && d->def >= kMaxWireDefs) {
			++clamped;
		}
		w.write_bits(d->def < kMaxWireDefs ? d->def : 0xFFu, 8);
		w.write_bits(uint8_t(d->zone) & 3u, 2);
		w.write_bits(d->shot_index, 32);
		w.write_bool(d->headshot);
		write_event_time(w, d->time, p_message.server_tick, clamped);
	}
	for (const DetonationWire *d : detonations) {
		w.write_bits(d->shooter, kWireSlotBits);
		w.write_bits(d->shot_index, 32);
		w.write_bits(d->def, 8);
		write_position(w, d->position, p_frame, clamped);
		write_octa(w, d->normal, kNormalBits);
		w.write_bool(d->dud);
		write_event_time(w, d->time, p_message.server_tick, clamped);
	}
	if (r_clamped) {
		*r_clamped = clamped;
	}
	return finish(w);
}

core::Result<CombatEvents> decode_combat_events(std::span<const uint8_t> p_datagram, const ShotQuantization &p_frame) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::CombatEvents)) {
		return fail("CombatEvents", "wrong type");
	}
	CombatEvents m;
	m.server_tick = sim::Tick(r.read_bits(32));
	const uint64_t nh = r.read_varint();
	const uint64_t nd = r.read_varint();
	const uint64_t nt = r.read_varint();
	if (r.failed()) {
		return fail("CombatEvents", "truncated");
	}
	if (nh > kMaxCombatEventsPerMessage || nd > kMaxCombatEventsPerMessage || nt > kMaxCombatEventsPerMessage ||
			nh + nd + nt > kMaxCombatEventsPerMessage || nh * kHitMinBits + nd * kDeathMinBits + nt * kDetonationMinBits > r.bits_left()) {
		return fail("CombatEvents", "event count out of range");
	}
	m.hits.resize(size_t(nh));
	for (HitEvent &h : m.hits) {
		h.victim = uint8_t(r.read_bits(kWireSlotBits));
		h.shooter = uint8_t(r.read_bits(kWireSlotBits));
		h.shot_index = uint32_t(r.read_bits(32));
		h.pellet = uint8_t(r.read_bits(6));
		h.zone = sim::weapons::HitZone(r.read_bits(2));
		const uint32_t damage = uint32_t(r.read_bits(kHpBits));
		const uint32_t health = uint32_t(r.read_bits(kHpBits));
		if (damage > kHpMaxCode || health > kHpMaxCode) {
			return fail("CombatEvents", "hit amount out of range");
		}
		h.damage = hp_value(damage);
		h.health_after = hp_value(health);
		h.position = read_frame(r, p_frame);
		h.normal = read_octa(r, kNormalBits);
		if (!read_event_time(r, m.server_tick, h.time)) {
			return fail("CombatEvents", r.failed() ? "truncated" : "hit time out of range");
		}
	}
	m.deaths.resize(size_t(nd));
	for (DeathEvent &d : m.deaths) {
		d.victim = uint8_t(r.read_bits(kWireSlotBits));
		d.killer = uint8_t(r.read_bits(kWireSlotBits));
		const uint64_t def = r.read_bits(8);
		d.def = def == 0xFF ? sim::weapons::kNoWeapon : uint16_t(def);
		d.zone = sim::weapons::HitZone(r.read_bits(2));
		d.shot_index = uint32_t(r.read_bits(32));
		d.headshot = r.read_bool();
		if (!read_event_time(r, m.server_tick, d.time)) {
			return fail("CombatEvents", r.failed() ? "truncated" : "death time out of range");
		}
	}
	m.detonations.resize(size_t(nt));
	for (DetonationWire &d : m.detonations) {
		d.shooter = uint8_t(r.read_bits(kWireSlotBits));
		d.shot_index = uint32_t(r.read_bits(32));
		const uint64_t def = r.read_bits(8);
		d.position = read_frame(r, p_frame);
		d.normal = read_octa(r, kNormalBits);
		d.dud = r.read_bool();
		if (!read_event_time(r, m.server_tick, d.time)) {
			return fail("CombatEvents", r.failed() ? "truncated" : "detonation time out of range");
		}
		if (def >= kMaxWireDefs) {
			return fail("CombatEvents", "detonation without a weapon def");
		}
		d.def = uint16_t(def);
	}
	return close(r, std::move(m), "CombatEvents");
}

// ---- KitSelect (ch1, C->S) ---------------------------------------------------------------------------------------
// Body: u8 kit (< kMaxKits) | u8 team (0 keep, 1, 2). 3 bytes with the type byte.

std::vector<uint8_t> encode_kit_select(const KitSelect &p_message) {
	BitWriter w = begin(MessageType::KitSelect);
	w.write_bits(p_message.kit, 8);
	w.write_bits(p_message.team, 8);
	return finish(w);
}

core::Result<KitSelect> decode_kit_select(std::span<const uint8_t> p_datagram) {
	BitReader r(p_datagram);
	if (!open(r, MessageType::KitSelect)) {
		return fail("KitSelect", "wrong type");
	}
	KitSelect m;
	const uint64_t kit = r.read_bits(8);
	const uint64_t team = r.read_bits(8);
	if (r.failed()) {
		return fail("KitSelect", "truncated");
	}
	if (kit >= kMaxKits || team > kMaxTeam) {
		return fail("KitSelect", "kit or team out of range");
	}
	m.kit = uint8_t(kit);
	m.team = uint8_t(team);
	return close(r, std::move(m), "KitSelect");
}

// M2b W1.0 (docs/design/m2b.md §11 "The one production stub"; docs/design/m2b-w1.md): the v3 codecs, so ru, bb and nt
// link on parallel branches. nt replaces these bodies (R9); a sender skips an empty vector.
std::vector<uint8_t> encode_match_state(const MatchState &) { return {}; } // M2b W1.0 stub
core::Result<MatchState> decode_match_state(std::span<const uint8_t>) { return core::make_error("v3 codec: nt"); } // M2b W1.0 stub
std::vector<uint8_t> encode_player_list(const PlayerList &, size_t *) { return {}; } // M2b W1.0 stub
core::Result<PlayerList> decode_player_list(std::span<const uint8_t>) { return core::make_error("v3 codec: nt"); } // M2b W1.0 stub
std::vector<uint8_t> encode_spawn_request(const SpawnRequest &) { return {}; } // M2b W1.0 stub
core::Result<SpawnRequest> decode_spawn_request(std::span<const uint8_t>) { return core::make_error("v3 codec: nt"); } // M2b W1.0 stub

} // namespace bf3::net

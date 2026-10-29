#include "bf3/net/protocol.h"

#include "bf3/core/util/finite.h"
#include "bf3/net/bit_stream.h"
#include "bf3/net/weapons_wire.h"
#include "bit_archive.h"
#include "weapons_wire_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace bf3::net {

namespace {

using bf3::core::finite;
namespace sw = sim::weapons;

constexpr unsigned kTypeBits = 8;
constexpr size_t kMaxIdentBytes = 128;
constexpr size_t kMaxTextBytes = 2048;
constexpr size_t kMaxSweepRows = 1024;

constexpr size_t kMaxReliableBytes = 256 * 1024;
constexpr size_t kDatagramBits = kMaxDatagramBytes * 8;
constexpr float kMaxWorldCoordinate = 1.0e6f;

constexpr uint8_t kKnownWeaponFlags = weapon_flags::kExactAim | weapon_flags::kInfiniteMagazine;
constexpr uint8_t kKnownRulesFlags = rules_flags::kTdm | rules_flags::kDeployScreen | rules_flags::kRevive;
constexpr unsigned kWireSlotBits = 7;
constexpr unsigned kWireDefBits = 8;
constexpr uint32_t kNoWireDef = 0xFF;
static_assert(kNoWireDef == kMaxWireDefs);
static_assert((size_t(1) << kWireSlotBits) == kMaxSlots);
constexpr unsigned kOthersCountBits = 8;
constexpr int32_t kOtherPitchMaxCode = (1 << (quant::kSoldierPitchBits - 1)) - 1;
constexpr WeaponsWireFaults kOwnWeaponsFaults{ "own weapons field out of range", "own weapons field out of range" };

core::Error fail(const char *p_what, const char *p_why) {
	return core::make_error(std::string(p_what) + ": " + p_why);
}

constexpr bool valid_pose(uint64_t p_value) {
	return p_value < sim::kPoseCount;
}

bool in_world(const sim::Vec3 &p_position) {
	return finite(p_position) && std::fabs(p_position[0]) <= kMaxWorldCoordinate &&
			std::fabs(p_position[1]) <= kMaxWorldCoordinate && std::fabs(p_position[2]) <= kMaxWorldCoordinate;
}

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

template <class A, MaybeConst<sim::Vec3> V>
constexpr void vec3(A &r_a, V &r_v) {
	for (auto &f : r_v) {
		r_a.f32(f);
	}
}

template <class A, MaybeConst<std::string> S>
void text(A &r_a, S &r_text, size_t p_max, const char *p_why) {
	uint64_t length = clamp_utf8(r_text, p_max).size();
	r_a.varint(length);
	r_a.require(p_why, [&] { return length <= p_max && r_a.fits(length * 8); });
	length = r_a.resize(r_text, size_t(length));
	for (size_t i = 0; i < length; ++i) {
		r_a.bits(r_text[i], 8);
	}
	r_a.require(p_why, [&] { return valid_utf8(r_text); });
}

template <class A, MaybeConst<std::optional<core::Sha256Digest>> D>
void digest(A &r_a, D &r_digest) {
	r_a.optional(r_digest, [&](auto &r_value) {
		for (auto &b : r_value.bytes) {
			r_a.bits(b, 8);
		}
	});
}

BitWriter begin(MessageType p_type, size_t p_cap) {
	BitWriter w(p_cap);
	w.write_bits(uint8_t(p_type), kTypeBits);
	return w;
}

std::vector<uint8_t> finish(BitWriter &r_w) {
	if (r_w.overflowed()) {
		return {};
	}
	return r_w.take();
}

bool open(BitReader &r_r, MessageType p_type) {
	return uint8_t(r_r.read_bits(kTypeBits)) == uint8_t(p_type) && !r_r.failed();
}

template <class A, MaybeConst<sim::SoldierState> S>
constexpr bool serialize(A &r_a, S &r_s) {
	r_a.bits(r_s.tick, 32);
	r_a.bits(r_s.life, 32);
	vec3(r_a, r_s.position);
	vec3(r_a, r_s.velocity);
	vec3(r_a, r_s.ground_normal);
	r_a.bits(r_s.yaw, 16);
	r_a.bits(r_s.pitch, 16);
	r_a.bits(r_s.pose, 8);
	r_a.bits(r_s.target_pose, 8);
	r_a.bits(r_s.transition_ticks, 16);
	r_a.bits(r_s.transition_total, 16);
	r_a.bits(r_s.mode, 8);
	r_a.bits(r_s.flags, 16);
	r_a.bits(r_s.events, 16);
	r_a.bits(r_s.jump_penalty_ticks, 16);
	r_a.bits(r_s.air_ticks, 16);
	r_a.bits(r_s.protect_ticks, 16);
	r_a.f32(r_s.health);
	r_a.f32(r_s.impact_speed);
	r_a.f32(r_s.damage);
	return r_a.valid([&] {
		return finite(r_s.position) && finite(r_s.velocity) && finite(r_s.ground_normal) && valid_pose(uint8_t(r_s.pose)) &&
				valid_pose(uint8_t(r_s.target_pose)) && uint8_t(r_s.mode) < sim::kMoveModeCount && r_s.pitch >= -sim::kPitchLimit &&
				r_s.pitch <= sim::kPitchLimit && finite(r_s.health) && finite(r_s.impact_speed) && finite(r_s.damage);
	});
}

constexpr size_t kOwnStateBits = count_bits([](auto &r_a) {
	sim::SoldierState s{};
	serialize(r_a, s);
});
static_assert(kOwnStateBits == 77 * 8);

struct WireCommand {
	uint32_t buttons = 0;
	int8_t forward = 0;
	int8_t right = 0;
	uint16_t yaw = 0;
	int16_t pitch = 0;
	uint8_t pose = 0;

	uint16_t view_back_q8 = 0;
	uint8_t view_quality = 0;
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

sim::Command command_of(const WireCommand &p_w, sim::Tick p_tick, uint32_t p_sequence) {
	sim::Command c;
	c.tick = p_tick;
	c.sequence = p_sequence;
	c.buttons = p_w.buttons;
	c.move_forward = p_w.forward;
	c.move_right = p_w.right;
	c.yaw = p_w.yaw;
	c.pitch = p_w.pitch;
	c.pose = sim::Pose(p_w.pose);
	c.view_back_q8 = p_w.view_back_q8;
	c.view_quality = p_w.view_quality;
	c.view_seq8 = p_w.view_seq8;
	return c;
}

template <class A, MaybeConst<WireCommand> C>
constexpr bool serialize(A &r_a, C &r_c) {
	r_a.bits(r_c.buttons, 24);
	r_a.bits(r_c.forward, 8);
	r_a.bits(r_c.right, 8);
	r_a.bits(r_c.yaw, 16);
	r_a.bits(r_c.pitch, quant::kCommandPitchBits);
	r_a.bits(r_c.pose, 2);
	r_a.bits(r_c.view_back_q8, 16);
	r_a.bits(r_c.view_quality, 2);
	r_a.bits(r_c.view_seq8, 8);
	return r_a.valid([&] {
		return valid_pose(r_c.pose) && r_c.forward >= -sim::kAxisMax && r_c.right >= -sim::kAxisMax && r_c.pitch >= -sim::kPitchLimit &&
				r_c.pitch <= sim::kPitchLimit;
	});
}

static_assert(count_bits([](auto &r_a) {
	WireCommand c{};
	serialize(r_a, c);
}) == 98);

struct CommandPacketWire {
	uint16_t newest_snapshot = 0;
	uint32_t snapshot_ack_bits = 0;
	uint32_t others_ack_bits = 0;
	uint8_t count = 0;
	sim::Tick tick = 0;
	uint32_t sequence = 0;
	std::array<WireCommand, kCommandsPerPacket> commands{};
};

template <class A, MaybeConst<CommandPacketWire> P>
void serialize(A &r_a, P &r_p) {
	r_a.bits(r_p.newest_snapshot, 16);
	r_a.bits(r_p.snapshot_ack_bits, 32);
	r_a.bits(r_p.others_ack_bits, 32);
	r_a.bits(r_p.count, 8);
	r_a.check("command count out of range", [&] { return r_p.count != 0 && r_p.count <= kCommandsPerPacket; });
	if (!r_a.ok()) {
		return;
	}
	r_a.bits(r_p.tick, 32);
	r_a.bits(r_p.sequence, 32);
	const bool newest = serialize(r_a, r_p.commands[0]);
	r_a.check("command field out of range", [&] { return newest; });
	for (size_t i = 1; i < r_p.count && r_a.ok(); ++i) {
		r_a.check("implied tick or sequence below 0", [&] { return r_p.tick >= i && r_p.sequence >= i; });
		bool same = r_p.commands[i] == r_p.commands[i - 1];
		r_a.bits(same, 1);
		if (same) {
			r_a.store(r_p.commands[i], [&] { return r_p.commands[i - 1]; });
			continue;
		}
		const bool valid = serialize(r_a, r_p.commands[i]);
		r_a.check("command field out of range", [&] { return valid; });
	}
}

constexpr uint32_t other_yaw_code(uint16_t p_yaw) {
	return ((uint32_t(p_yaw) + 2u) >> 2) & ((1u << quant::kSoldierYawBits) - 1u);
}

constexpr int32_t other_pitch_code(int16_t p_pitch) {
	const int32_t code = (int32_t(p_pitch) + 2) >> 2;
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

template <class A, MaybeConst<float> F>
constexpr void quantized(A &r_a, F &r_value, const Quantizer &p_q) {
	r_a.coded(r_value, p_q.bits, [&](float p_v) { return p_q.encode(p_v); }, [&](uint32_t p_code) { return p_q.decode(p_code); });
}

template <class A, MaybeConst<OtherSoldier> O>
constexpr bool serialize(A &r_a, O &r_o) {
	r_a.bits(r_o.slot, 8);
	r_a.bits(r_o.generation, 8);
	quantized(r_a, r_o.position[0], quant::kPositionXZ);
	quantized(r_a, r_o.position[1], quant::kPositionY);
	quantized(r_a, r_o.position[2], quant::kPositionXZ);
	for (auto &v : r_o.velocity) {
		quantized(r_a, v, quant::kSoldierVelocity);
	}
	r_a.coded(r_o.yaw, quant::kSoldierYawBits, other_yaw_code, [](uint32_t p_code) { return uint16_t(p_code << 2); });
	r_a.coded(r_o.pitch, quant::kSoldierPitchBits, other_pitch_code, [](int32_t p_code) { return int16_t(p_code * 4); });
	r_a.bits(r_o.pose, 2);
	r_a.bits(r_o.target_pose, 2);
	r_a.bits(r_o.transition, 8);
	r_a.bits(r_o.mode, 4);
	r_a.bits(r_o.flags, 8);
	r_a.bits(r_o.weapon, 8);
	r_a.bits(r_o.weapon_phase, 3);
	r_a.bits(r_o.firing, 1);
	r_a.bits(r_o.ads, 1);
	r_a.bits(r_o.reload_seq, 2);
	r_a.bits(r_o.burst_seq, 4);
	r_a.bits(r_o.team, 2);
	return r_a.valid([&] {
		return r_o.team <= kMaxTeam && r_o.slot < kMaxSlots && r_o.pitch >= -kOtherPitchMaxCode * 4 && r_o.pitch <= kOtherPitchMaxCode * 4 &&
				valid_pose(uint8_t(r_o.pose)) && valid_pose(uint8_t(r_o.target_pose)) && uint8_t(r_o.mode) < sim::kMoveModeCount;
	});
}

constexpr size_t kOtherBits = count_bits([](auto &r_a) {
	OtherSoldier o{};
	serialize(r_a, o);
});
static_assert(kOtherBits == 180);

constexpr size_t others_room(size_t p_used_bits) {
	const size_t used = p_used_bits + kOthersCountBits;
	return used < kDatagramBits ? (kDatagramBits - used) / kOtherBits : 0;
}

class SlotSet {
public:
	bool insert(uint8_t p_slot) {
		const bool fresh = !seen_[p_slot];
		seen_[p_slot] = true;
		return fresh;
	}

private:
	std::array<bool, size_t(std::numeric_limits<uint8_t>::max()) + 1> seen_{};
};

template <class A, MaybeConst<std::vector<OtherSoldier>> V>
size_t others(A &r_a, V &r_others, size_t p_cap) {
	size_t count = std::min({ r_others.size(), others_room(r_a.bit_count()), p_cap });
	r_a.bits(count, kOthersCountBits);
	r_a.check("others count out of range", [&] { return count <= p_cap && r_a.fits(count * kOtherBits); });
	count = r_a.resize(r_others, count);
	SlotSet seen;
	for (size_t i = 0; i < count && r_a.ok(); ++i) {
		const bool valid = serialize(r_a, r_others[i]);
		r_a.check("other soldier field out of range", [&] { return valid; });
		r_a.check("slot named twice", [&] { return seen.insert(r_others[i].slot); });
	}
	return count;
}

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
	const double len = std::sqrt(x * x + y * y + z * z);
	return { float(x / len), float(y / len), float(z / len) };
}

template <class A, MaybeConst<Octa> O>
constexpr void serialize(A &r_a, O &r_o, unsigned p_bits) {
	r_a.bits(r_o.u, p_bits);
	r_a.bits(r_o.v, p_bits);
}

template <class A, MaybeConst<sim::Vec3> V>
void octa(A &r_a, V &r_dir, unsigned p_bits) {
	Octa code = r_a.encode([&] { return octa_encode(r_dir, p_bits); });
	serialize(r_a, code, p_bits);
	r_a.store(r_dir, [&] { return octa_decode(code, p_bits); });
}

constexpr unsigned kFramePositionBits = 20;
constexpr double kFrameStep = 1.0 / 256.0;
constexpr uint32_t kFrameMaxCode = (1u << kFramePositionBits) - 1u;

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

template <class A, MaybeConst<std::array<uint32_t, 3>> C>
constexpr void frame(A &r_a, C &r_codes) {
	for (auto &c : r_codes) {
		r_a.bits(c, kFramePositionBits);
	}
}

constexpr unsigned kTickFractionBits = 16;
constexpr int64_t kTickFractionScale = int64_t(1) << kTickFractionBits;

struct WireTime {
	uint32_t back = 0;
	uint32_t fraction = 0;
};

constexpr bool wire_time(sw::SimTime p_time, sim::Tick p_server_tick, WireTime &r_time) {
	using sw::kTimeUnitsPerTick;
	r_time = {};
	if (p_server_tick == 0) {
		return false;
	}
	const sw::SimTime end = sw::tick_end(p_server_tick);
	bool inside = true;
	sw::SimTime t = p_time;
	if (t >= end) {
		t = end - 1;
		inside = false;
	}
	if (t < 0) {
		t = 0;
		inside = false;
	}
	const sim::Tick tick = sw::tick_containing(t);
	r_time.back = uint32_t(p_server_tick - std::min(tick, p_server_tick));
	const int64_t within = t - sw::tick_start(p_server_tick - r_time.back);
	const int64_t f = (within * kTickFractionScale + kTimeUnitsPerTick / 2) / kTimeUnitsPerTick;
	r_time.fraction = uint32_t(std::clamp<int64_t>(f, 0, kTickFractionScale - 1));
	return inside;
}

constexpr sw::SimTime time_of(const WireTime &p_time, sim::Tick p_server_tick) {
	using sw::kTimeUnitsPerTick;
	return sw::tick_start(p_server_tick - p_time.back) + (int64_t(p_time.fraction) * kTimeUnitsPerTick + kTickFractionScale / 2) / kTickFractionScale;
}

template <class A, MaybeConst<sw::HitTarget> T>
constexpr bool serialize(A &r_a, T &r_t) {
	r_a.bits(r_t.id, 32);
	r_a.bits(r_t.team, 32);
	vec3(r_a, r_t.bound_center);
	r_a.f32(r_t.bound_radius);
	r_a.bits(r_t.spawn_protected, 1);
	uint8_t count = uint8_t(std::min<size_t>(r_t.capsule_count, sw::kMaxZoneCapsules));
	r_a.bits(count, 5);
	if (!r_a.valid([&] { return finite(r_t.bound_center) && finite(r_t.bound_radius) && count <= sw::kMaxZoneCapsules; })) {
		return false;
	}
	r_a.store(r_t.capsule_count, [&] { return count; });
	bool ok = true;
	for (uint8_t i = 0; i < count; ++i) {
		auto &c = r_t.capsules[i];
		vec3(r_a, c.a);
		vec3(r_a, c.b);
		r_a.f32(c.radius);
		r_a.bits(c.zone, 2);
		r_a.bits(c.capsule, 8);
		r_a.bits(c.property, 8);
		r_a.bits(c.flags, 8);
		ok = r_a.valid([&] { return finite(c.a) && finite(c.b) && finite(c.radius); }) && ok;
	}
	return ok;
}
static_assert(sw::kHitZoneCount == 4 && sw::kMaxZoneCapsules < 32);

template <class A, MaybeConst<TargetInfoRow> R>
constexpr bool serialize(A &r_a, R &r_row) {
	r_a.bits(r_row.slot, 8);
	r_a.bits(r_row.pose, 8);
	vec3(r_a, r_row.feet);
	r_a.bits(r_row.yaw, 16);
	const bool target = serialize(r_a, r_row.target);
	return r_a.valid([&] { return target && finite(r_row.feet) && r_row.slot < kMaxSlots && valid_pose(uint8_t(r_row.pose)); });
}

constexpr size_t kTargetRowMinBits = count_bits([](auto &r_a) {
	TargetInfoRow row{};
	serialize(r_a, row);
});

template <class A, MaybeConst<SpawnSweepRow> R>
constexpr bool serialize(A &r_a, R &r_row) {
	r_a.bits(r_row.index, 32);
	r_a.bits(r_row.ok, 1);
	vec3(r_a, r_row.point);
	vec3(r_a, r_row.settled);
	r_a.f32(r_row.lift);
	r_a.f32(r_row.shift);
	r_a.f32(r_row.gap);
	r_a.bits(r_row.grounded, 1);
	r_a.bits(r_row.overlapping, 1);
	return r_a.valid(
			[&] { return finite(r_row.point) && finite(r_row.settled) && finite(r_row.lift) && finite(r_row.shift) && finite(r_row.gap); });
}

constexpr size_t kSweepRowBits = count_bits([](auto &r_a) {
	SpawnSweepRow row{};
	serialize(r_a, row);
});

template <class A, MaybeConst<sw::ShotEvent> S>
constexpr bool serialize(A &r_a, S &r_s) {
	r_a.bits(r_s.time, 64);
	r_a.bits(r_s.spawn_time, 64);
	r_a.bits(r_s.seed, 64);
	r_a.bits(r_s.player_slot, 32);
	r_a.bits(r_s.shot_index, 32);
	r_a.bits(r_s.def, 16);
	r_a.bits(r_s.weapon_slot, 8);
	r_a.bits(r_s.pellets, 8);
	vec3(r_a, r_s.origin);
	vec3(r_a, r_s.aim);
	vec3(r_a, r_s.direction);
	r_a.f32(r_s.spread);
	r_a.f32(r_s.pellet_cone);
	r_a.f32(r_s.recoil.pitch);
	r_a.f32(r_s.recoil.yaw);
	r_a.bits(r_s.tracer, 1);
	r_a.f32(r_s.power);
	return r_a.valid([&] {
		return finite(r_s.origin) && finite(r_s.aim) && finite(r_s.direction) && finite(r_s.spread) && finite(r_s.pellet_cone) &&
				finite(r_s.recoil.pitch) && finite(r_s.recoil.yaw) && finite(r_s.power) && r_s.weapon_slot < sw::kMaxWeaponSlots && r_s.pellets >= 1 &&
				r_s.pellets <= sw::kMaxPellets;
	});
}

template <class A, MaybeConst<sw::DamageEvent> D>
constexpr bool serialize(A &r_a, D &r_d) {
	r_a.bits(r_d.time, 64);
	r_a.bits(r_d.attacker_slot, 32);
	r_a.bits(r_d.target, 32);
	r_a.bits(r_d.shot_index, 32);
	r_a.bits(r_d.def, 16);
	r_a.bits(r_d.pellet, 8);
	r_a.bits(r_d.kind, 2);
	r_a.bits(r_d.zone, 2);
	r_a.bits(r_d.capsule, 8);
	r_a.f32(r_d.distance);
	r_a.f32(r_d.base);
	r_a.f32(r_d.multiplier);
	r_a.f32(r_d.penetration);
	r_a.f32(r_d.amount);
	return r_a.valid([&] { return finite(r_d.distance) && finite(r_d.base) && finite(r_d.multiplier) && finite(r_d.penetration) && finite(r_d.amount); });
}
static_assert(uint8_t(sw::DamageKind::DudBlast) == 3);

template <class A, MaybeConst<sw::DetonationEvent> D>
constexpr bool serialize(A &r_a, D &r_d) {
	r_a.bits(r_d.time, 64);
	vec3(r_a, r_d.position);
	vec3(r_a, r_d.normal);
	r_a.bits(r_d.player_slot, 32);
	r_a.bits(r_d.shot_index, 32);
	r_a.bits(r_d.def, 16);
	r_a.bits(r_d.dud, 1);
	r_a.bits(r_d.timeout, 1);
	return r_a.valid([&] { return finite(r_d.position) && finite(r_d.normal); });
}

template <class A, MaybeConst<sim::Vec3> V, MaybeConst<uint32_t> S>
constexpr bool projectile(A &r_a, V &r_position, S &r_shot) {
	vec3(r_a, r_position);
	r_a.bits(r_shot, 32);
	return r_a.valid([&] { return finite(r_position); });
}

template <class A, MaybeConst<TraceDeath> D>
constexpr void serialize(A &r_a, D &r_d) {
	r_a.bits(r_d.victim, 8);
	r_a.bits(r_d.killer, 8);
	r_a.bits(r_d.shot_index, 32);
}

template <class T>
constexpr size_t fixed_bits() {
	return count_bits([](auto &r_a) {
		T value{};
		serialize(r_a, value);
	});
}

constexpr size_t kShotFullBits = fixed_bits<sw::ShotEvent>();
constexpr size_t kDamageFullBits = fixed_bits<sw::DamageEvent>();
constexpr size_t kDetonationFullBits = fixed_bits<sw::DetonationEvent>();
constexpr size_t kTraceDeathBits = fixed_bits<TraceDeath>();
constexpr size_t kProjectileBits = count_bits([](auto &r_a) {
	sim::Vec3 position{};
	uint32_t shot = 0;
	projectile(r_a, position, shot);
});

static_assert(kMaxTraceShots < 16 && kMaxTraceProjectiles < 16 && kMaxTraceDamage < 16 && kMaxTraceDetonations < 4 && kMaxTraceDeaths < 8);

template <class A, MaybeConst<TraceGunplayTick> T>
constexpr bool serialize(A &r_a, T &r_t, uint64_t &r_dropped) {
	r_a.bits(r_t.tick, 32);
	size_t shots = std::min(r_t.shots.size(), kMaxTraceShots);
	const size_t both = std::min(r_t.projectiles.size(), r_t.projectile_shots.size());
	size_t projectiles = std::min(both, kMaxTraceProjectiles);
	size_t damage = std::min(r_t.damage.size(), kMaxTraceDamage);
	size_t detonations = std::min(r_t.detonations.size(), kMaxTraceDetonations);
	size_t deaths = std::min(r_t.deaths.size(), kMaxTraceDeaths);
	r_dropped += (r_t.shots.size() - shots) + (std::max(r_t.projectiles.size(), r_t.projectile_shots.size()) - projectiles) + (r_t.damage.size() - damage) +
			(r_t.detonations.size() - detonations) + (r_t.deaths.size() - deaths);
	r_a.bits(shots, 4);
	r_a.bits(projectiles, 4);
	r_a.bits(damage, 4);
	r_a.bits(detonations, 2);
	r_a.bits(deaths, 3);
	if (!r_a.valid([&] {
				return shots <= kMaxTraceShots && projectiles <= kMaxTraceProjectiles && damage <= kMaxTraceDamage && detonations <= kMaxTraceDetonations &&
						deaths <= kMaxTraceDeaths &&
						r_a.fits(shots * kShotFullBits + projectiles * kProjectileBits + damage * kDamageFullBits + detonations * kDetonationFullBits +
								deaths * kTraceDeathBits);
			})) {
		return false;
	}
	shots = r_a.resize(r_t.shots, shots);
	projectiles = r_a.resize(r_t.projectiles, projectiles);
	r_a.resize(r_t.projectile_shots, projectiles);
	damage = r_a.resize(r_t.damage, damage);
	detonations = r_a.resize(r_t.detonations, detonations);
	deaths = r_a.resize(r_t.deaths, deaths);
	bool ok = true;
	for (size_t i = 0; i < shots; ++i) {
		ok = serialize(r_a, r_t.shots[i]) && ok;
	}
	for (size_t i = 0; i < projectiles; ++i) {
		ok = projectile(r_a, r_t.projectiles[i], r_t.projectile_shots[i]) && ok;
	}
	for (size_t i = 0; i < damage; ++i) {
		ok = serialize(r_a, r_t.damage[i]) && ok;
	}
	for (size_t i = 0; i < detonations; ++i) {
		ok = serialize(r_a, r_t.detonations[i]) && ok;
	}
	for (size_t i = 0; i < deaths; ++i) {
		serialize(r_a, r_t.deaths[i]);
	}
	return ok;
}

constexpr size_t kGunplayTickMinBits = count_bits([](auto &r_a) {
	TraceGunplayTick tick{};
	uint64_t dropped = 0;
	serialize(r_a, tick, dropped);
});

template <class A, MaybeConst<Hello> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.protocol, 16);
	text(r_a, r_m.name, kMaxNameBytes, "bad name");
}

template <class A, MaybeConst<MatchInfo> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.protocol, 16);
	text(r_a, r_m.level, kMaxIdentBytes, "bad level or mode name");
	text(r_a, r_m.mode, kMaxIdentBytes, "bad level or mode name");
	r_a.bits(r_m.sim_hz, 16);
	r_a.bits(r_m.snapshot_divisor, 8);
	r_a.bits(r_m.slot, 8);
	r_a.bits(r_m.server_tick, 32);
	r_a.bits(r_m.phase, 8);
	digest(r_a, r_m.source_digest);
	r_a.bits(r_m.motor_hash, 64);
	r_a.bits(r_m.match_seed, 64);
	r_a.bits(r_m.weapon_hash, 64);
	r_a.bits(r_m.weapon_flags, 8);
	r_a.bits(r_m.kit_hash, 64);
	r_a.bits(r_m.max_rewind_ticks, 8);
	r_a.bits(r_m.logic_digest, 64);
	r_a.bits(r_m.rules_flags, 8);
	r_a.check("unknown weapon flags", [&] { return (r_m.weapon_flags & ~kKnownWeaponFlags) == 0; });
	r_a.check("unknown rules flags", [&] { return (r_m.rules_flags & ~kKnownRulesFlags) == 0; });
	r_a.check("zero tick rate or snapshot divisor", [&] { return r_m.sim_hz != 0 && r_m.snapshot_divisor != 0; });
	r_a.check("slot out of range", [&] { return r_m.slot < kMaxSlots; });
	r_a.check("unknown phase", [&] { return uint8_t(r_m.phase) <= uint8_t(MatchPhase::Restarting); });
}

template <class A, MaybeConst<Ready> M>
void serialize(A &r_a, M &r_m) {
	digest(r_a, r_m.source_digest);
}

template <class A, MaybeConst<Spawned> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.slot, 8);
	r_a.bits(r_m.spawn_index, 32);
	const bool state = serialize(r_a, r_m.state);
	r_a.bits(r_m.kit, 8);
	r_a.bits(r_m.team, 8);
	r_a.bits(r_m.revived, 1);
	for (auto &def : r_m.defs) {
		r_a.coded(
				def, kWireDefBits, [](uint16_t p_def) { return p_def < kMaxWireDefs ? uint32_t(p_def) : kNoWireDef; },
				[](uint32_t p_code) { return p_code == kNoWireDef ? sw::kNoWeapon : uint16_t(p_code); });
	}
	r_a.check("slot out of range", [&] { return r_m.slot < kMaxSlots; });
	r_a.check("state field out of range", [&] { return state; });
	r_a.check("kit or team out of range", [&] { return r_m.kit < kMaxKits && r_m.team <= kMaxTeam; });
}

template <class A, MaybeConst<Loading> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.stage, 8);
	r_a.bits(r_m.permille, 16);
	r_a.check("progress above 1000 permille", [&] { return r_m.permille <= 1000; });
}

template <class A, MaybeConst<Kick> M>
void serialize(A &r_a, M &r_m) {
	text(r_a, r_m.reason, kMaxTextBytes, "bad reason text");
}

template <class A, MaybeConst<DebugRequest> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.kind, 8);
	r_a.bits(r_m.id, 32);
	vec3(r_a, r_m.position);
	r_a.bits(r_m.yaw, 16);
	r_a.bits(r_m.index, 32);
	r_a.bits(r_m.ticks, 32);
	r_a.bits(r_m.slot, 8);
	r_a.bits(r_m.pose, 8);
	r_a.check("unknown kind", [&] {
		return uint8_t(r_m.kind) >= uint8_t(DebugKind::Teleport) && uint8_t(r_m.kind) <= uint8_t(DebugKind::Revive);
	});
	r_a.check("slot or pose out of range", [&] { return r_m.slot < kMaxSlots && valid_pose(uint8_t(r_m.pose)); });
	r_a.check("position out of range", [&] { return in_world(r_m.position); });
	r_a.check("tick count above kMaxTraceTicks", [&] { return r_m.ticks <= kMaxTraceTicks; });
}

template <class A, MaybeConst<DebugReport> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.id, 32);
	r_a.bits(r_m.ok, 1);
	text(r_a, r_m.text, kMaxTextBytes, "bad text");

	uint64_t rows = std::min(r_m.sweep.size(), kMaxSweepRows);
	r_a.varint(rows);
	r_a.require("bad sweep row count", [&] { return rows <= kMaxSweepRows && r_a.fits(rows * kSweepRowBits); });
	rows = r_a.resize(r_m.sweep, size_t(rows));
	for (size_t i = 0; i < rows && r_a.ok(); ++i) {
		const bool valid = serialize(r_a, r_m.sweep[i]);
		r_a.check("non-finite sweep value", [&] { return valid; });
	}

	uint64_t states = std::min<size_t>(r_m.trace.size(), kMaxTraceTicks);
	r_a.varint(states);
	r_a.require("bad trace count", [&] { return states <= kMaxTraceTicks && r_a.fits(states * kOwnStateBits); });
	states = r_a.resize(r_m.trace, size_t(states));
	for (size_t i = 0; i < states && r_a.ok(); ++i) {
		const bool valid = serialize(r_a, r_m.trace[i]);
		r_a.check("trace state field out of range", [&] { return valid; });
	}

	uint64_t targets = std::min(r_m.targets.size(), kMaxTargetRows);
	r_a.varint(targets);
	r_a.require("bad target row count", [&] { return targets <= kMaxTargetRows && r_a.fits(targets * kTargetRowMinBits); });
	targets = r_a.resize(r_m.targets, size_t(targets));
	for (size_t i = 0; i < targets && r_a.ok(); ++i) {
		const bool valid = serialize(r_a, r_m.targets[i]);
		r_a.check("target row field out of range", [&] { return valid; });
	}

	uint64_t dropped = r_m.trace_dropped;
	uint64_t ticks = std::min<size_t>(r_m.trace_gunplay.size(), kMaxTraceTicks);
	r_a.varint(ticks);
	r_a.require("bad gunplay trace count", [&] { return ticks <= kMaxTraceTicks && r_a.fits(ticks * kGunplayTickMinBits); });
	ticks = r_a.resize(r_m.trace_gunplay, size_t(ticks));
	for (size_t i = 0; i < ticks && r_a.ok(); ++i) {
		const bool valid = serialize(r_a, r_m.trace_gunplay[i], dropped);
		r_a.check("gunplay trace field out of range", [&] { return valid; });
	}
	uint32_t sent_dropped = uint32_t(std::min<uint64_t>(dropped, 0xFFFFFFFFull));
	r_a.bits(sent_dropped, 32);
	r_a.store(r_m.trace_dropped, [&] { return sent_dropped; });
}

template <class A, MaybeConst<Snapshot> M>
void serialize(A &r_a, M &r_m, size_t &r_others) {
	r_a.bits(r_m.seq, 16);
	r_a.bits(r_m.server_tick, 32);
	r_a.bits(r_m.applied_sequence, 32);
	r_a.bits(r_m.applied_repeated, 1);
	r_a.bits(r_m.command_buffer_ticks, 8);
	r_a.bits(r_m.sequence_echo, 16);
	r_a.bits(r_m.echo_hold_100us, 16);
	r_a.bits(r_m.own_events, 16);
	r_a.optional(r_m.own, [&](auto &r_own) {
		const bool valid = serialize(r_a, r_own);
		r_a.check("own state field out of range", [&] { return valid; });
		r_a.check("own state tick differs from the server tick", [&] { return r_own.tick == r_m.server_tick; });
		r_a.optional(r_m.own_weapons, [&](auto &r_weapons) { serialize(r_a, r_weapons, kOwnWeaponsFaults); });
	});
	r_others = others(r_a, r_m.others, kMaxSlots);
}

template <class A, MaybeConst<SnapshotOthers> M>
void serialize(A &r_a, M &r_m, size_t &r_others) {
	r_a.bits(r_m.seq, 16);
	r_a.bits(r_m.server_tick, 32);
	r_others = others(r_a, r_m.others, kMaxSlots - 1);
}

constexpr unsigned kShotDirBits = 16;
constexpr unsigned kShotAimBits = 12;
constexpr unsigned kShotBackBits = 3;
constexpr unsigned kShotDeltaBits = 4;
constexpr unsigned kPelletBits = 6;
constexpr unsigned kConeBits = 13;
constexpr double kConeScale = 64.0;
constexpr unsigned kPowerBits = 12;
static_assert(sw::kMaxPellets == 64, "pellets travel as pellets - 1 in 6 bits");
static_assert(sw::kMaxAngle < double(1u << kConeBits) / kConeScale);
static_assert(kMaxShotTicksBack < (1u << kShotBackBits));

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

struct ShotIndexChain {
	std::array<bool, kMaxSlots> have{};
	std::array<uint32_t, kMaxSlots> prev{};
};

constexpr uint32_t short_delta(uint32_t p_index, uint32_t p_prev) {
	const uint32_t delta = p_index - p_prev;
	return delta >= 1 && delta < (1u << kShotDeltaBits) ? delta : 0;
}

template <class A, MaybeConst<ShotCodes> C>
constexpr void serialize(A &r_a, C &r_c, ShotIndexChain &r_chain, sim::Tick p_server_tick) {
	r_a.bits(r_c.slot, kWireSlotBits);
	const bool have = r_chain.have[r_c.slot];
	const uint32_t prev = r_chain.prev[r_c.slot];
	if (!have) {
		r_a.bits(r_c.index, 32);
	} else {
		uint32_t delta = short_delta(r_c.index, prev);
		r_a.bits(delta, kShotDeltaBits);
		if (delta == 0) {
			r_a.bits(r_c.index, 32);
		} else {
			r_a.store(r_c.index, [&] { return prev + delta; });
		}
	}
	r_a.bits(r_c.def, 8);
	r_a.bits(r_c.time.back, kShotBackBits);
	r_a.bits(r_c.time.fraction, kTickFractionBits);
	frame(r_a, r_c.origin);
	serialize(r_a, r_c.direction, kShotDirBits);
	r_a.coded(r_c.pellets, kPelletBits, [](uint8_t p_pellets) { return uint32_t(p_pellets - 1); }, [](uint32_t p_code) { return uint8_t(p_code + 1); });
	r_a.bits(r_c.tracer, 1);
	if (r_c.tracer) {
		serialize(r_a, r_c.aim, kShotAimBits);
	}
	if (r_c.pellets > 1) {
		r_a.bits(r_c.cone, kConeBits);
	}
	r_a.bits(r_c.has_power, 1);
	if (r_c.has_power) {
		r_a.bits(r_c.power, kPowerBits);
	}
	r_a.check("def out of range", [&] { return r_c.def < kMaxWireDefs; });
	r_a.check("shot time before tick 1", [&] { return r_c.time.back < p_server_tick; });
	r_a.check("shot index repeated", [&] { return !have || r_c.index != prev; });
	r_chain.have[r_c.slot] = true;
	r_chain.prev[r_c.slot] = r_c.index;
}

constexpr size_t kShotMinBits = count_bits([](auto &r_a) {
	ShotCodes codes{};
	ShotIndexChain chain{};
	chain.have[codes.slot] = true;
	chain.prev[codes.slot] = codes.index - 1;
	serialize(r_a, codes, chain, sim::Tick(0));
});
static_assert(kShotMinBits == 138);

size_t shot_bits(const ShotCodes &p_codes, ShotIndexChain &r_chain) {
	return count_bits([&](auto &r_a) { serialize(r_a, p_codes, r_chain, sim::Tick(0)); });
}

struct ShotEventsWire {
	sim::Tick server_tick = 0;
	std::vector<ShotCodes> shots;
};

bool distinct_shots(const std::vector<ShotCodes> &p_shots) {
	for (size_t i = 0; i < p_shots.size(); ++i) {
		for (size_t j = i + 1; j < p_shots.size(); ++j) {
			if (p_shots[i].slot == p_shots[j].slot && p_shots[i].index == p_shots[j].index) {
				return false;
			}
		}
	}
	return true;
}

template <class A, MaybeConst<ShotEventsWire> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.server_tick, 32);
	size_t count = r_m.shots.size();
	r_a.bits(count, 8);
	r_a.check("shot count out of range", [&] { return count <= kMaxShotsPerDatagram && r_a.fits(count * kShotMinBits); });
	count = r_a.resize(r_m.shots, count);
	ShotIndexChain chain;
	for (size_t i = 0; i < count && r_a.ok(); ++i) {
		serialize(r_a, r_m.shots[i], chain, r_m.server_tick);
	}
	r_a.check("shot named twice", [&] { return distinct_shots(r_m.shots); });
}

std::optional<ShotCodes> shot_codes(const RemoteShot &p_shot, const ShotQuantization &p_frame, sim::Tick p_server_tick) {
	ShotCodes c;
	if (p_shot.slot >= kMaxSlots || p_shot.def >= kMaxWireDefs || p_shot.pellets < 1 || p_shot.pellets > sw::kMaxPellets) {
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
		const double cone = std::isfinite(p_shot.pellet_cone) ? std::clamp(double(p_shot.pellet_cone), 0.0, double(sw::kMaxAngle)) : 0.0;
		c.cone = std::min(uint32_t(cone * kConeScale + 0.5), (1u << kConeBits) - 1u);
	}
	c.has_power = p_shot.power != 1.0f;
	if (c.has_power) {
		const double top = double((1u << kPowerBits) - 1u);
		const double power = std::isfinite(p_shot.power) ? std::clamp(double(p_shot.power), 0.0, double(sw::kMaxPower)) : 0.0;
		c.power = std::min(uint32_t(power / double(sw::kMaxPower) * top + 0.5), (1u << kPowerBits) - 1u);
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
		s.power = float(double(p_c.power) / double((1u << kPowerBits) - 1u) * double(sw::kMaxPower));
	}
	return s;
}

constexpr unsigned kHpBits = 24;
constexpr double kHpScale = 100.0;

constexpr uint32_t kHpMaxCode = 10'000'000;
static_assert(kHpMaxCode < (1u << kHpBits));
constexpr unsigned kNormalBits = 8;

constexpr uint32_t hp_code(float p_hp, size_t &r_clamped) {
	const double t = double(p_hp) * kHpScale + 0.5;
	if (!(t >= 0.0) || !(t < double(kHpMaxCode) + 1.0)) {
		++r_clamped;
	}
	return !(t > 0.0) ? 0u : t >= double(kHpMaxCode) ? kHpMaxCode : uint32_t(t);
}

constexpr float hp_value(uint32_t p_code) {
	return float(double(p_code) / kHpScale);
}

struct CombatFrame {
	const ShotQuantization &frame;
	sim::Tick server_tick = 0;
	size_t clamped = 0;
};

template <class A, MaybeConst<float> F>
constexpr bool hp(A &r_a, F &r_value, CombatFrame &r_f) {
	uint32_t code = r_a.encode([&] { return hp_code(r_value, r_f.clamped); });
	r_a.bits(code, kHpBits);
	r_a.store(r_value, [&] { return hp_value(code); });
	return code <= kHpMaxCode;
}

template <class A, MaybeConst<sim::Vec3> V>
void frame_position(A &r_a, V &r_position, CombatFrame &r_f) {
	std::array<uint32_t, 3> codes = r_a.encode([&] {
		std::array<uint32_t, 3> c{};
		if (!frame_codes(r_position, r_f.frame, c)) {
			++r_f.clamped;
		}
		return c;
	});
	frame(r_a, codes);
	r_a.store(r_position, [&] { return frame_decode(codes, r_f.frame); });
}

template <class A, MaybeConst<sw::SimTime> T>
constexpr void event_time(A &r_a, T &r_time, CombatFrame &r_f, const char *p_why) {
	const WireTime code = r_a.encode([&] {
		WireTime t;
		if (!wire_time(r_time, r_f.server_tick, t)) {
			++r_f.clamped;
		}
		return t;
	});
	uint64_t back = code.back;
	uint32_t fraction = code.fraction;
	r_a.varint(back);
	r_a.bits(fraction, kTickFractionBits);
	r_a.check(p_why, [&] { return back < r_f.server_tick; });
	r_a.store(r_time, [&] { return time_of({ uint32_t(back), fraction }, r_f.server_tick); });
}

template <class A, MaybeConst<HitEvent> H>
void serialize(A &r_a, H &r_h, CombatFrame &r_f) {
	r_a.bits(r_h.victim, kWireSlotBits);
	r_a.bits(r_h.shooter, kWireSlotBits);
	r_a.bits(r_h.shot_index, 32);
	r_a.coded(
			r_h.pellet, kPelletBits,
			[&](uint8_t p_pellet) {
				if (p_pellet >= sw::kMaxPellets) {
					++r_f.clamped;
				}
				return std::min<uint32_t>(p_pellet, sw::kMaxPellets - 1);
			},
			[](uint32_t p_code) { return uint8_t(p_code); });
	r_a.bits(r_h.zone, 2);
	const bool damage = hp(r_a, r_h.damage, r_f);
	const bool health = hp(r_a, r_h.health_after, r_f);
	r_a.check("hit amount out of range", [&] { return damage && health; });
	frame_position(r_a, r_h.position, r_f);
	octa(r_a, r_h.normal, kNormalBits);
	event_time(r_a, r_h.time, r_f, "hit time out of range");
}

template <class A, MaybeConst<DeathEvent> D>
void serialize(A &r_a, D &r_d, CombatFrame &r_f) {
	r_a.bits(r_d.victim, kWireSlotBits);
	r_a.bits(r_d.killer, kWireSlotBits);
	r_a.coded(
			r_d.def, kWireDefBits,
			[&](uint16_t p_def) {
				if (p_def != sw::kNoWeapon && p_def >= kMaxWireDefs) {
					++r_f.clamped;
				}
				return p_def < kMaxWireDefs ? uint32_t(p_def) : kNoWireDef;
			},
			[](uint32_t p_code) { return p_code == kNoWireDef ? sw::kNoWeapon : uint16_t(p_code); });
	r_a.bits(r_d.zone, 2);
	r_a.bits(r_d.shot_index, 32);
	r_a.bits(r_d.headshot, 1);
	event_time(r_a, r_d.time, r_f, "death time out of range");
	uint32_t killer_hp = r_a.encode([&] { return hp_code(r_d.killer_health, r_f.clamped); });
	bool stamped = r_d.respawn_tick != 0 || r_d.revive_until != 0 || killer_hp != 0;
	r_a.bits(stamped, 1);
	if (stamped) {
		r_a.bits(r_d.respawn_tick, 32);
		r_a.bits(r_d.revive_until, 32);
		r_a.bits(killer_hp, kHpBits);
		r_a.check("killer health out of range", [&] { return killer_hp <= kHpMaxCode; });
		r_a.store(r_d.killer_health, [&] { return hp_value(killer_hp); });
	}
}

template <class A, MaybeConst<DetonationWire> D>
void serialize(A &r_a, D &r_d, CombatFrame &r_f) {
	r_a.bits(r_d.shooter, kWireSlotBits);
	r_a.bits(r_d.shot_index, 32);
	r_a.bits(r_d.def, kWireDefBits);
	frame_position(r_a, r_d.position, r_f);
	octa(r_a, r_d.normal, kNormalBits);
	r_a.bits(r_d.dud, 1);
	event_time(r_a, r_d.time, r_f, "detonation time out of range");
	r_a.check("detonation without a weapon def", [&] { return r_d.def < kMaxWireDefs; });
}

template <class A, MaybeConst<ReviveEvent> V>
constexpr void serialize(A &r_a, V &r_v, CombatFrame &r_f) {
	r_a.bits(r_v.reviver, kWireSlotBits);
	r_a.bits(r_v.revived, kWireSlotBits);
	uint64_t back = r_a.encode([&] {
		if (r_v.tick > r_f.server_tick) {
			++r_f.clamped;
		}
		return uint64_t(r_v.tick <= r_f.server_tick ? r_f.server_tick - r_v.tick : 0u);
	});
	r_a.varint(back);
	r_a.check("revive tick out of range", [&] { return back <= r_f.server_tick; });
	r_a.store(r_v.tick, [&] { return sim::Tick(r_f.server_tick - back); });
}

constexpr ShotQuantization kNoFrame{};

template <class T>
size_t combat_min_bits() {
	return count_bits([](auto &r_a) {
		T event{};
		CombatFrame f{ kNoFrame };
		serialize(r_a, event, f);
	});
}

const size_t kHitMinBits = combat_min_bits<HitEvent>();
const size_t kDeathMinBits = combat_min_bits<DeathEvent>();
const size_t kDetonationMinBits = combat_min_bits<DetonationWire>();
const size_t kReviveMinBits = combat_min_bits<ReviveEvent>();

template <class A, class V>
void combat_list(A &r_a, V &r_events, uint64_t p_count, CombatFrame &r_f) {
	const size_t count = r_a.resize(r_events, size_t(p_count));
	for (size_t i = 0; i < count && r_a.ok(); ++i) {
		serialize(r_a, r_events[i], r_f);
	}
}

template <class A, MaybeConst<CombatEvents> M>
void serialize(A &r_a, M &r_m, const ShotQuantization &p_frame, size_t &r_clamped) {
	r_a.bits(r_m.server_tick, 32);
	uint64_t hits = r_m.hits.size();
	uint64_t deaths = r_m.deaths.size();
	uint64_t detonations = r_m.detonations.size();
	uint64_t revives = r_m.revives.size();
	r_a.varint(hits);
	r_a.varint(deaths);
	r_a.varint(detonations);
	r_a.varint(revives);
	r_a.check("event count out of range", [&] {
		return hits <= kMaxCombatEventsPerMessage && deaths <= kMaxCombatEventsPerMessage && detonations <= kMaxCombatEventsPerMessage &&
				revives <= kMaxCombatEventsPerMessage && hits + deaths + detonations + revives <= kMaxCombatEventsPerMessage &&
				r_a.fits(hits * kHitMinBits + deaths * kDeathMinBits + detonations * kDetonationMinBits + revives * kReviveMinBits);
	});
	CombatFrame f{ p_frame, r_m.server_tick };
	combat_list(r_a, r_m.hits, hits, f);
	combat_list(r_a, r_m.deaths, deaths, f);
	combat_list(r_a, r_m.detonations, detonations, f);
	combat_list(r_a, r_m.revives, revives, f);
	r_clamped += f.clamped;
}

template <class A, MaybeConst<KitSelect> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.kit, 8);
	r_a.bits(r_m.team, 8);
	r_a.check("kit or team out of range", [&] { return r_m.kit < kMaxKits && r_m.team <= kMaxTeam; });
}

constexpr unsigned kRowSlotBits = 6;
constexpr unsigned kRowCountBits = 7;
constexpr unsigned kNameBytesBits = 5;
constexpr unsigned kRestrictMaskBits = 13;
static_assert((size_t(1) << kRowSlotBits) == kMaxPlayerRows && (size_t(1) << (kRowCountBits - 1)) == kMaxPlayerRows);
static_assert(kMaxPlayerNameBytes < (size_t(1) << kNameBytesBits));
static_assert(restrict_bits::kAll == (1u << kRestrictMaskBits) - 1u);
static_assert(kMaxKits == 4, "PlayerRow::kit travels in 2 bits");

template <class A, MaybeConst<MatchState> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.phase, 8);
	r_a.bits(r_m.round_start_tick, 32);
	r_a.bits(r_m.countdown_end_tick, 32);
	r_a.bits(r_m.kills[0], 32);
	r_a.bits(r_m.kills[1], 32);
	r_a.bits(r_m.kill_limit, 32);
	r_a.bits(r_m.winner, 2);
	r_a.bits(r_m.killer_camera, 1);
	r_a.bits(r_m.round_end_tick, 32);
	r_a.bits(r_m.unspawn_tick, 32);
	r_a.bits(r_m.restart_tick, 32);
	r_a.bits(r_m.restrict_until_tick, 32);
	r_a.bits(r_m.restrict_mask, kRestrictMaskBits);
	r_a.bits(r_m.players_needed, 16);
	r_a.check("unknown phase", [&] { return uint8_t(r_m.phase) <= uint8_t(MatchPhase::Restarting); });
	r_a.check("winner out of range", [&] { return r_m.winner <= kMaxTeam; });
	r_a.check("negative kill limit", [&] { return r_m.kill_limit >= 0; });
}

MatchState canonical_match_state(const MatchState &p_message) {
	MatchState m = p_message;
	m.phase = MatchPhase(std::min<uint8_t>(uint8_t(m.phase), uint8_t(MatchPhase::Restarting)));
	m.kill_limit = std::max<int32_t>(m.kill_limit, 0);
	m.winner = std::min<uint8_t>(m.winner, kMaxTeam);
	m.restrict_mask &= restrict_bits::kAll;
	return m;
}

template <class A, MaybeConst<PlayerRow> R>
constexpr void serialize(A &r_a, R &r_row, int p_previous_slot) {
	r_a.bits(r_row.slot, kRowSlotBits);
	r_a.bits(r_row.team, 2);
	r_a.bits(r_row.kit, 2);
	r_a.bits(r_row.bot, 1);
	r_a.bits(r_row.alive, 1);
	r_a.bits(r_row.name_bytes, kNameBytesBits);
	r_a.bits(r_row.ping_ms, 16);
	r_a.bits(r_row.kills, 16);
	r_a.bits(r_row.deaths, 16);
	r_a.bits(r_row.score, 32);
	r_a.check("rows not in ascending slot order", [&] { return int(r_row.slot) > p_previous_slot; });
	r_a.check("team out of range", [&] { return r_row.team <= kMaxTeam; });
	r_a.check("bad name length", [&] { return r_row.name_bytes <= kMaxPlayerNameBytes && r_a.fits(size_t(r_row.name_bytes) * 8); });
	for (size_t k = 0; k < r_row.name_bytes && r_a.ok(); ++k) {
		r_a.bits(r_row.name[k], 8);
	}
	r_a.require("bad name", [&] { return valid_utf8(player_name(r_row)); });
}

constexpr size_t kRowMinBits = count_bits([](auto &r_a) {
	PlayerRow row{};
	serialize(r_a, row, -1);
});

template <class A, MaybeConst<PlayerList> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.server_tick, 32);
	r_a.bits(r_m.full, 1);
	size_t rows = r_m.rows.size();
	size_t removed = r_m.removed.size();
	r_a.bits(rows, kRowCountBits);
	r_a.bits(removed, kRowCountBits);
	r_a.check("row count out of range", [&] {
		return rows <= kMaxPlayerRows && removed <= kMaxPlayerRows && !(r_m.full && removed != 0) && r_a.fits(rows * kRowMinBits + removed * kRowSlotBits);
	});
	rows = r_a.resize(r_m.rows, rows);
	std::array<bool, kMaxPlayerRows> present{};
	for (size_t i = 0; i < rows && r_a.ok(); ++i) {
		serialize(r_a, r_m.rows[i], i == 0 ? -1 : int(r_m.rows[i - 1].slot));
		present[r_m.rows[i].slot % kMaxPlayerRows] = true;
	}
	removed = r_a.resize(r_m.removed, removed);
	for (size_t i = 0; i < removed && r_a.ok(); ++i) {
		r_a.bits(r_m.removed[i], kRowSlotBits);
		r_a.check("removed slots not ascending or also a row",
				[&] { return (i == 0 || r_m.removed[i] > r_m.removed[i - 1]) && !present[r_m.removed[i] % kMaxPlayerRows]; });
	}
}

size_t valid_name_bytes(const PlayerRow &p_row) {
	size_t n = std::min<size_t>(p_row.name_bytes, kMaxPlayerNameBytes);
	while (n > 0 && !valid_utf8(std::string_view(p_row.name.data(), n))) {
		--n;
	}
	return n;
}

PlayerList canonical_player_list(const PlayerList &p_message, size_t &r_clamped) {
	std::array<const PlayerRow *, kMaxPlayerRows> by_slot{};
	for (const PlayerRow &row : p_message.rows) {
		if (row.slot >= kMaxPlayerRows || by_slot[row.slot] != nullptr) {
			++r_clamped;
			continue;
		}
		by_slot[row.slot] = &row;
	}
	std::array<bool, kMaxPlayerRows> removed{};
	for (const uint8_t slot : p_message.removed) {
		if (p_message.full || slot >= kMaxPlayerRows || removed[slot] || by_slot[slot] != nullptr) {
			++r_clamped;
			continue;
		}
		removed[slot] = true;
	}
	PlayerList m;
	m.server_tick = p_message.server_tick;
	m.full = p_message.full;
	for (size_t s = 0; s < kMaxPlayerRows; ++s) {
		if (by_slot[s] == nullptr) {
			continue;
		}
		PlayerRow row = *by_slot[s];
		const size_t name_bytes = valid_name_bytes(row);
		if (name_bytes != row.name_bytes) {
			++r_clamped;
		}
		if (row.team > kMaxTeam || row.kit >= kMaxKits) {
			++r_clamped;
		}
		row.team = row.team <= kMaxTeam ? row.team : uint8_t(0);
		row.kit = std::min<uint8_t>(row.kit, kMaxKits - 1);
		row.name_bytes = uint8_t(name_bytes);
		m.rows.push_back(row);
	}
	for (size_t s = 0; s < kMaxPlayerRows; ++s) {
		if (removed[s]) {
			m.removed.push_back(uint8_t(s));
		}
	}
	return m;
}

template <class A, MaybeConst<SpawnRequest> M>
void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.kit, 8);
	r_a.bits(r_m.spawn_choice, 8);
}

template <class A, MaybeConst<SuicideRequest> M> void serialize(A &, M &) {}

template <class A, MaybeConst<LoadoutRequest> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.kit, 8);
	for (auto &item : r_m.items) {
		r_a.bits(item, 32);
	}
	for (auto &accessory : r_m.accessories) {
		r_a.bits(accessory, 32);
	}
	r_a.bits(r_m.specialization, 32);
	r_a.check("kit out of range", [&] { return r_m.kit < kMaxKits; });
}

template <class A, MaybeConst<SoldierGear> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.slot, 8);
	r_a.bits(r_m.def, 16);
	for (auto &accessory : r_m.accessories) {
		r_a.bits(accessory, 32);
	}
	r_a.check("slot out of range", [&] { return r_m.slot < kMaxSlots; });
}

template <class A, MaybeConst<Mismatch> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.components, 8);
	r_a.check("no or unknown mismatch components",
			[&] { return r_m.components != 0 && (r_m.components & ~mismatch_bits::kAll) == 0; });
}

template <class A, MaybeConst<NextLevel> N> void serialize(A &r_a, N &r_n) {
	text(r_a, r_n.level, kMaxIdentBytes, "bad level or mode name");
	text(r_a, r_n.mode, kMaxIdentBytes, "bad level or mode name");
	digest(r_a, r_n.source_digest);
	r_a.bits(r_n.logic_digest, 64);
}

template <class A, MaybeConst<RoundOverInfo> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.winner, 8);
	serialize(r_a, r_m.next);
	r_a.bits(r_m.intermission_s, 16);
	r_a.check("winner out of range", [&] { return r_m.winner <= kMaxTeam; });
}

template <class A, MaybeConst<Prepare> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.round_id, 32);
	digest(r_a, r_m.next_source_digest);
	r_a.bits(r_m.state, 8);
	r_a.bits(r_m.permille, 16);
	r_a.check("unknown prepare state", [&] { return uint8_t(r_m.state) <= uint8_t(PrepareState::MissingXpack); });
	r_a.check("progress above 1000 permille", [&] { return r_m.permille <= 1000; });
}

template <class A, MaybeConst<LevelChange> M> void serialize(A &r_a, M &r_m) {
	text(r_a, r_m.level, kMaxIdentBytes, "bad level or mode name");
	text(r_a, r_m.mode, kMaxIdentBytes, "bad level or mode name");
	r_a.bits(r_m.round_id, 32);
	r_a.bits(r_m.load_deadline_s, 16);
}

template <class A, MaybeConst<sim::logic::LogicNetworkedEvent> E>
constexpr void serialize(A &r_a, E &r_e, sim::Tick p_server_tick) {
	uint64_t back = r_a.encode([&] { return uint64_t(sim::Tick(p_server_tick - r_e.tick)); });
	r_a.varint(back);
	r_a.check("logic event after the message tick", [&] { return back <= p_server_tick; });
	r_a.store(r_e.tick, [&] { return sim::Tick(p_server_tick - back); });
	r_a.bits(r_e.connection, 64);
	r_a.bits(r_e.source, 32);
	r_a.bits(r_e.source_port, 32);
	r_a.coded(
			r_e.context.player, 8, [](int32_t p_player) { return uint32_t(p_player + 1); },
			[](uint32_t p_code) { return int32_t(p_code) - 1; });
	r_a.bits(r_e.context.team, 8);
	r_a.check("logic event context out of range",
			[&] { return r_e.context.player < int32_t(kMaxSlots) && r_e.context.team <= kMaxTeam; });
}

constexpr size_t kLogicEventMinBits = count_bits([](auto &r_a) {
	sim::logic::LogicNetworkedEvent event{};
	serialize(r_a, event, sim::Tick(0));
});

template <class A, MaybeConst<LogicEvents> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.server_tick, 32);
	uint64_t count = r_m.events.size();
	r_a.varint(count);
	r_a.check("logic event count out of range",
			[&] { return count <= kMaxLogicEventsPerMessage && r_a.fits(count * kLogicEventMinBits); });
	count = r_a.resize(r_m.events, size_t(count));
	for (size_t i = 0; i < count && r_a.ok(); ++i) {
		serialize(r_a, r_m.events[i], r_m.server_tick);
	}
}

constexpr size_t kGhostBoolsPerWord = 64;
constexpr size_t kGhostTransformFloats = 12;

constexpr size_t ghost_words(uint64_t p_bools) {
	return size_t((p_bools + kGhostBoolsPerWord - 1) / kGhostBoolsPerWord);
}

template <class A, MaybeConst<FullSync> M> void serialize(A &r_a, M &r_m) {
	auto &g = r_m.ghost;
	r_a.bits(g.tick, 32);
	uint64_t bools = g.bool_count;
	r_a.varint(bools);
	r_a.check(
			"ghost bool count out of range", [&] { return bools <= sim::logic::kMaxGhostEntries && r_a.fits(bools); });
	r_a.store(g.bool_count, [&] { return r_a.ok() ? size_t(bools) : size_t(0); });
	const size_t words = r_a.resize(g.bool_bits, ghost_words(bools));
	for (size_t w = 0; w < words && r_a.ok(); ++w) {
		r_a.bits(g.bool_bits[w], unsigned(std::min<uint64_t>(kGhostBoolsPerWord, bools - w * kGhostBoolsPerWord)));
	}
	uint64_t transforms = g.transforms.size();
	r_a.varint(transforms);
	r_a.check("ghost transform count out of range", [&] {
		return transforms <= sim::logic::kMaxGhostEntries && r_a.fits(transforms * kGhostTransformFloats * 32);
	});
	transforms = r_a.resize(g.transforms, size_t(transforms));
	for (size_t i = 0; i < transforms && r_a.ok(); ++i) {
		for (auto &f : g.transforms[i]) {
			r_a.f32(f);
		}
		r_a.check("non-finite ghost transform", [&] { return finite(std::span<const float>(g.transforms[i])); });
	}
	uint64_t sequences = g.sequence_start.size();
	r_a.varint(sequences);
	r_a.check("ghost sequence count out of range",
			[&] { return sequences <= sim::logic::kMaxGhostEntries && r_a.fits(sequences * 32); });
	sequences = r_a.resize(g.sequence_start, size_t(sequences));
	for (size_t i = 0; i < sequences && r_a.ok(); ++i) {
		r_a.bits(g.sequence_start[i], 32);
	}
}

constexpr unsigned kGhostIndexBits = 12;
static_assert((size_t(1) << kGhostIndexBits) == sim::logic::kMaxGhostEntries);
constexpr size_t kGhostDeltaRowBits = kGhostIndexBits + 1;

template <class A, MaybeConst<LogicDelta> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.tick, 32);
	uint64_t rows = r_m.rows.size();
	r_a.varint(rows);
	r_a.check("ghost delta row count out of range",
			[&] { return rows <= sim::logic::kMaxGhostEntries && r_a.fits(rows * kGhostDeltaRowBits); });
	rows = r_a.resize(r_m.rows, size_t(rows));
	for (size_t i = 0; i < rows && r_a.ok(); ++i) {
		r_a.bits(r_m.rows[i].index, kGhostIndexBits);
		r_a.bits(r_m.rows[i].value, 1);
		r_a.check(
				"ghost delta rows not ascending", [&] { return i == 0 || r_m.rows[i].index > r_m.rows[i - 1].index; });
	}
}

template <class A, MaybeConst<SquadRequest> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.op, 8);
	r_a.bits(r_m.squad, 8);
	r_a.bits(r_m.target, 8);
	r_a.check("unknown squad op", [&] { return uint8_t(r_m.op) <= uint8_t(SquadOp::SwitchTeam); });
	r_a.check("squad or target out of range", [&] { return r_m.squad <= kMaxSquads && r_m.target < kMaxSlots; });
}

template <class A, MaybeConst<SquadOrder> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.team, 8);
	r_a.bits(r_m.squad, 8);
	r_a.bits(r_m.kind, 8);
	r_a.bits(r_m.area, 16);
	vec3(r_a, r_m.position);
	r_a.check("team or squad out of range",
			[&] { return r_m.team >= 1 && r_m.team <= kMaxTeam && r_m.squad >= 1 && r_m.squad <= kMaxSquads; });
	r_a.check("unknown order kind", [&] { return uint8_t(r_m.kind) <= uint8_t(SquadOrderKind::Defend); });
	r_a.check("position out of range", [&] { return in_world(r_m.position); });
}

struct Framing {
	MessageType type;
	const char *what;
	size_t max_bytes = std::numeric_limits<size_t>::max();
};

template <class M, class... Context>
std::vector<uint8_t> encode_as(MessageType p_type, size_t p_cap, const M &p_message, Context &&...p_context) {
	BitWriter w = begin(p_type, p_cap);
	BitWriteArchive archive(w);
	serialize(archive, p_message, std::forward<Context>(p_context)...);
	return finish(w);
}

template <class M, class... Context>
core::Result<M> decode_as(std::span<const uint8_t> p_datagram, const Framing &p_framing, Context &&...p_context) {
	BitReader r(p_datagram);
	if (!open(r, p_framing.type)) {
		return fail(p_framing.what, "wrong type");
	}
	if (p_datagram.size() > p_framing.max_bytes) {
		return fail(p_framing.what, "larger than one datagram");
	}
	BitReadArchive archive(r);
	M message;
	serialize(archive, message, std::forward<Context>(p_context)...);
	if (const char *why = archive.error()) {
		return fail(p_framing.what, why);
	}
	if (!r.at_end()) {
		return fail(p_framing.what, "trailing bytes");
	}
	return message;
}

}

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
		case MessageType::SnapshotOthers:
		case MessageType::ShotEvents:
		case MessageType::CombatEvents:
		case MessageType::KitSelect:
		case MessageType::MatchState:
		case MessageType::PlayerList:
		case MessageType::SpawnRequest:
		case MessageType::SuicideRequest:
		case MessageType::LoadoutRequest:
		case MessageType::SoldierGear:
		case MessageType::Mismatch:
		case MessageType::RoundOverInfo:
		case MessageType::Prepare:
		case MessageType::LevelChange:
		case MessageType::LogicEvents:
		case MessageType::FullSync:
		case MessageType::LogicDelta:
		case MessageType::SquadRequest:
		case MessageType::SquadOrder:
			return MessageType(p_datagram[0]);
	}
	return std::nullopt;
}

std::vector<uint8_t> encode_hello(const Hello &p_message) {
	return encode_as(MessageType::Hello, kMaxReliableBytes, p_message);
}

core::Result<Hello> decode_hello(std::span<const uint8_t> p_datagram) {
	return decode_as<Hello>(p_datagram, { MessageType::Hello, "Hello" });
}

std::vector<uint8_t> encode_match_info(const MatchInfo &p_message) {
	return encode_as(MessageType::MatchInfo, kMaxReliableBytes, p_message);
}

core::Result<MatchInfo> decode_match_info(std::span<const uint8_t> p_datagram) {
	return decode_as<MatchInfo>(p_datagram, { MessageType::MatchInfo, "MatchInfo" });
}

std::vector<uint8_t> encode_ready(const Ready &p_message) {
	return encode_as(MessageType::Ready, kMaxReliableBytes, p_message);
}

core::Result<Ready> decode_ready(std::span<const uint8_t> p_datagram) {
	return decode_as<Ready>(p_datagram, { MessageType::Ready, "Ready" });
}

std::vector<uint8_t> encode_spawned(const Spawned &p_message) {
	return encode_as(MessageType::Spawned, kMaxReliableBytes, p_message);
}

core::Result<Spawned> decode_spawned(std::span<const uint8_t> p_datagram) {
	return decode_as<Spawned>(p_datagram, { MessageType::Spawned, "Spawned" });
}

std::vector<uint8_t> encode_loading(const Loading &p_message) {
	Loading m = p_message;
	m.permille = std::min<uint16_t>(m.permille, 1000);
	return encode_as(MessageType::Loading, kMaxReliableBytes, m);
}

core::Result<Loading> decode_loading(std::span<const uint8_t> p_datagram) {
	return decode_as<Loading>(p_datagram, { MessageType::Loading, "Loading" });
}

std::vector<uint8_t> encode_kick(const Kick &p_message) {
	return encode_as(MessageType::Kick, kMaxReliableBytes, p_message);
}

core::Result<Kick> decode_kick(std::span<const uint8_t> p_datagram) {
	return decode_as<Kick>(p_datagram, { MessageType::Kick, "Kick" });
}

std::vector<uint8_t> encode_bye() {
	BitWriter w = begin(MessageType::Bye, kMaxReliableBytes);
	return finish(w);
}

std::vector<uint8_t> encode_debug(const DebugRequest &p_message) {
	return encode_as(MessageType::Debug, kMaxReliableBytes, p_message);
}

core::Result<DebugRequest> decode_debug(std::span<const uint8_t> p_datagram) {
	return decode_as<DebugRequest>(p_datagram, { MessageType::Debug, "Debug" });
}

std::vector<uint8_t> encode_debug_report(const DebugReport &p_message) {
	return encode_as(MessageType::DebugReport, kMaxDebugReportBytes, p_message);
}

core::Result<DebugReport> decode_debug_report(std::span<const uint8_t> p_datagram) {
	return decode_as<DebugReport>(p_datagram, { MessageType::DebugReport, "DebugReport" });
}

std::vector<uint8_t> encode_command_packet(const CommandPacket &p_packet) {
	size_t count = std::min<size_t>(p_packet.count, kCommandsPerPacket);
	if (count == 0) {
		return {};
	}

	for (size_t i = 1; i < count; ++i) {
		const sim::Command &newer = p_packet.commands[i - 1];
		const sim::Command &older = p_packet.commands[i];
		if (newer.tick == 0 || newer.sequence == 0 || older.tick != newer.tick - 1 || older.sequence != newer.sequence - 1) {
			count = i;
			break;
		}
	}
	CommandPacketWire wire;
	wire.newest_snapshot = p_packet.newest_snapshot;
	wire.snapshot_ack_bits = p_packet.snapshot_ack_bits;
	wire.others_ack_bits = p_packet.others_ack_bits;
	wire.count = uint8_t(count);
	wire.tick = p_packet.commands[0].tick;
	wire.sequence = p_packet.commands[0].sequence;
	for (size_t i = 0; i < count; ++i) {
		wire.commands[i] = wire_of(p_packet.commands[i]);
	}
	return encode_as(MessageType::CommandPacket, kMaxDatagramBytes, wire);
}

core::Result<CommandPacket> decode_command_packet(std::span<const uint8_t> p_datagram) {
	const core::Result<CommandPacketWire> wire = decode_as<CommandPacketWire>(p_datagram, { MessageType::CommandPacket, "CommandPacket" });
	if (!wire) {
		return wire.error();
	}
	CommandPacket p;
	p.newest_snapshot = wire->newest_snapshot;
	p.snapshot_ack_bits = wire->snapshot_ack_bits;
	p.others_ack_bits = wire->others_ack_bits;
	p.count = wire->count;
	for (size_t i = 0; i < p.count; ++i) {
		p.commands[i] = command_of(wire->commands[i], wire->tick - sim::Tick(i), wire->sequence - uint32_t(i));
	}
	return p;
}

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
	size_t carried = 0;
	std::vector<uint8_t> bytes = encode_as(MessageType::Snapshot, kMaxDatagramBytes, p_snapshot, carried);
	if (r_dropped) {
		*r_dropped = p_snapshot.others.size() - carried;
	}
	return bytes;
}

core::Result<Snapshot> decode_snapshot(std::span<const uint8_t> p_datagram) {
	size_t carried = 0;
	return decode_as<Snapshot>(p_datagram, { MessageType::Snapshot, "Snapshot" }, carried);
}

void set_other_weapon(OtherSoldier &r_other, uint16_t p_def, sim::weapons::WeaponPhase p_phase, bool p_firing, bool p_ads, uint32_t p_reload_count,
		uint32_t p_burst_count) {
	r_other.weapon = p_def < kMaxWireDefs ? uint8_t(p_def) : uint8_t(0xFF);
	r_other.weapon_phase = uint8_t(uint8_t(p_phase) & 7u);
	r_other.firing = p_firing;
	r_other.ads = p_ads;
	r_other.reload_seq = uint8_t(p_reload_count & 3u);
	r_other.burst_seq = uint8_t(p_burst_count & 15u);
}

void set_other_team(OtherSoldier &r_other, uint8_t p_team) {
	r_other.team = p_team >= 1 && p_team <= kMaxTeam ? p_team : uint8_t(0);
}

bool snapshot_acked(const CommandPacket &p_packet, uint16_t p_seq) {
	if (p_seq == p_packet.newest_snapshot) {
		return true;
	}
	if (seq_newer(p_seq, p_packet.newest_snapshot)) {
		return false;
	}
	const uint16_t back = uint16_t(p_packet.newest_snapshot - 1u - p_seq);
	return back < 32 && ((p_packet.snapshot_ack_bits >> back) & 1u) != 0;
}

bool others_acked(const CommandPacket &p_packet, uint16_t p_seq) {
	if (seq_newer(p_seq, p_packet.newest_snapshot)) {
		return false;
	}
	const uint16_t back = uint16_t(p_packet.newest_snapshot - p_seq);
	return back < 32 && ((p_packet.others_ack_bits >> back) & 1u) != 0;
}

std::vector<uint8_t> encode_snapshot_others(const SnapshotOthers &p_message, size_t *r_dropped) {
	size_t carried = 0;
	std::vector<uint8_t> bytes = encode_as(MessageType::SnapshotOthers, kMaxDatagramBytes, p_message, carried);
	if (r_dropped) {
		*r_dropped = p_message.others.size() - carried;
	}
	return bytes;
}

core::Result<SnapshotOthers> decode_snapshot_others(std::span<const uint8_t> p_datagram) {
	size_t carried = 0;
	return decode_as<SnapshotOthers>(p_datagram, { MessageType::SnapshotOthers, "SnapshotOthers", kMaxDatagramBytes }, carried);
}

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
		bool repeated = false;
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

	ShotEventsWire wire;
	wire.server_tick = p_message.server_tick;
	ShotIndexChain chain;
	size_t bits = kTypeBits + count_bits([&](auto &r_a) { serialize(r_a, wire); });
	for (const Item &it : items) {
		if (wire.shots.size() >= kMaxShotsPerDatagram) {
			break;
		}
		const size_t b = shot_bits(it.codes, chain);
		if (bits + b > kDatagramBits) {
			break;
		}
		bits += b;
		wire.shots.push_back(it.codes);
	}
	dropped += items.size() - wire.shots.size();
	if (r_dropped) {
		*r_dropped = dropped;
	}
	return encode_as(MessageType::ShotEvents, kMaxDatagramBytes, wire);
}

core::Result<ShotEvents> decode_shot_events(std::span<const uint8_t> p_datagram, const ShotQuantization &p_frame) {
	const core::Result<ShotEventsWire> wire = decode_as<ShotEventsWire>(p_datagram, { MessageType::ShotEvents, "ShotEvents", kMaxDatagramBytes });
	if (!wire) {
		return wire.error();
	}
	ShotEvents m;
	m.server_tick = wire->server_tick;
	m.shots.reserve(wire->shots.size());
	for (const ShotCodes &c : wire->shots) {
		m.shots.push_back(shot_from_codes(c, p_frame, m.server_tick));
	}
	return m;
}

std::vector<uint8_t> encode_combat_events(const CombatEvents &p_message, const ShotQuantization &p_frame, size_t *r_clamped) {
	size_t clamped = 0;

	size_t budget = kMaxCombatEventsPerMessage;
	CombatEvents kept;
	kept.server_tick = p_message.server_tick;
	for (const HitEvent &h : p_message.hits) {
		if (h.victim >= kMaxSlots || h.shooter >= kMaxSlots || budget == 0) {
			++clamped;
			continue;
		}
		kept.hits.push_back(h);
		--budget;
	}
	for (const DeathEvent &d : p_message.deaths) {
		if (d.victim >= kMaxSlots || d.killer >= kMaxSlots || budget == 0) {
			++clamped;
			continue;
		}
		kept.deaths.push_back(d);
		--budget;
	}
	for (const DetonationWire &d : p_message.detonations) {
		if (d.shooter >= kMaxSlots || d.def >= kMaxWireDefs || budget == 0) {
			++clamped;
			continue;
		}
		kept.detonations.push_back(d);
		--budget;
	}
	for (const ReviveEvent &v : p_message.revives) {
		if (v.reviver >= kMaxSlots || v.revived >= kMaxSlots || budget == 0) {
			++clamped;
			continue;
		}
		kept.revives.push_back(v);
		--budget;
	}
	std::vector<uint8_t> bytes = encode_as(MessageType::CombatEvents, kMaxReliableBytes, kept, p_frame, clamped);
	if (r_clamped) {
		*r_clamped = clamped;
	}
	return bytes;
}

core::Result<CombatEvents> decode_combat_events(std::span<const uint8_t> p_datagram, const ShotQuantization &p_frame) {
	size_t clamped = 0;
	return decode_as<CombatEvents>(p_datagram, { MessageType::CombatEvents, "CombatEvents" }, p_frame, clamped);
}

std::vector<uint8_t> encode_kit_select(const KitSelect &p_message) {
	return encode_as(MessageType::KitSelect, kMaxReliableBytes, p_message);
}

core::Result<KitSelect> decode_kit_select(std::span<const uint8_t> p_datagram) {
	return decode_as<KitSelect>(p_datagram, { MessageType::KitSelect, "KitSelect" });
}

std::vector<uint8_t> encode_match_state(const MatchState &p_message) {
	return encode_as(MessageType::MatchState, kMaxReliableBytes, canonical_match_state(p_message));
}

core::Result<MatchState> decode_match_state(std::span<const uint8_t> p_datagram) {
	return decode_as<MatchState>(p_datagram, { MessageType::MatchState, "MatchState" });
}

std::vector<uint8_t> encode_player_list(const PlayerList &p_message, size_t *r_clamped) {
	size_t clamped = 0;
	const PlayerList canonical = canonical_player_list(p_message, clamped);
	if (r_clamped) {
		*r_clamped = clamped;
	}
	return encode_as(MessageType::PlayerList, kMaxReliableBytes, canonical);
}

core::Result<PlayerList> decode_player_list(std::span<const uint8_t> p_datagram) {
	return decode_as<PlayerList>(p_datagram, { MessageType::PlayerList, "PlayerList" });
}

std::vector<uint8_t> encode_spawn_request(const SpawnRequest &p_message) {
	return encode_as(MessageType::SpawnRequest, kMaxReliableBytes, p_message);
}

core::Result<SpawnRequest> decode_spawn_request(std::span<const uint8_t> p_datagram) {
	return decode_as<SpawnRequest>(p_datagram, { MessageType::SpawnRequest, "SpawnRequest" });
}

std::vector<uint8_t> encode_suicide_request() {
	return encode_as(MessageType::SuicideRequest, kMaxReliableBytes, SuicideRequest{});
}

core::Result<SuicideRequest> decode_suicide_request(std::span<const uint8_t> p_datagram) {
	return decode_as<SuicideRequest>(p_datagram, { MessageType::SuicideRequest, "SuicideRequest" });
}

std::vector<uint8_t> encode_loadout_request(const LoadoutRequest &p_message) {
	return encode_as(MessageType::LoadoutRequest, kMaxReliableBytes, p_message);
}

core::Result<LoadoutRequest> decode_loadout_request(std::span<const uint8_t> p_datagram) {
	return decode_as<LoadoutRequest>(p_datagram, { MessageType::LoadoutRequest, "LoadoutRequest" });
}

std::vector<uint8_t> encode_soldier_gear(const SoldierGear &p_message) {
	return encode_as(MessageType::SoldierGear, kMaxReliableBytes, p_message);
}

core::Result<SoldierGear> decode_soldier_gear(std::span<const uint8_t> p_datagram) {
	return decode_as<SoldierGear>(p_datagram, { MessageType::SoldierGear, "SoldierGear" });
}

std::vector<uint8_t> encode_mismatch(const Mismatch &p_message) {
	return encode_as(MessageType::Mismatch, kMaxReliableBytes, p_message);
}

core::Result<Mismatch> decode_mismatch(std::span<const uint8_t> p_datagram) {
	return decode_as<Mismatch>(p_datagram, { MessageType::Mismatch, "Mismatch" });
}

std::vector<uint8_t> encode_round_over_info(const RoundOverInfo &p_message) {
	return encode_as(MessageType::RoundOverInfo, kMaxReliableBytes, p_message);
}

core::Result<RoundOverInfo> decode_round_over_info(std::span<const uint8_t> p_datagram) {
	return decode_as<RoundOverInfo>(p_datagram, { MessageType::RoundOverInfo, "RoundOverInfo" });
}

std::vector<uint8_t> encode_prepare(const Prepare &p_message) {
	Prepare m = p_message;
	m.permille = std::min<uint16_t>(m.permille, 1000);
	return encode_as(MessageType::Prepare, kMaxReliableBytes, m);
}

core::Result<Prepare> decode_prepare(std::span<const uint8_t> p_datagram) {
	return decode_as<Prepare>(p_datagram, { MessageType::Prepare, "Prepare" });
}

std::vector<uint8_t> encode_level_change(const LevelChange &p_message) {
	return encode_as(MessageType::LevelChange, kMaxReliableBytes, p_message);
}

core::Result<LevelChange> decode_level_change(std::span<const uint8_t> p_datagram) {
	return decode_as<LevelChange>(p_datagram, { MessageType::LevelChange, "LevelChange" });
}

std::vector<uint8_t> encode_logic_events(const LogicEvents &p_message, size_t *r_dropped) {
	LogicEvents kept;
	kept.server_tick = p_message.server_tick;
	for (const sim::logic::LogicNetworkedEvent &e : p_message.events) {
		const bool valid = e.tick <= p_message.server_tick && e.context.player >= -1 &&
				e.context.player < int32_t(kMaxSlots) && e.context.team <= kMaxTeam;
		if (valid && kept.events.size() < kMaxLogicEventsPerMessage) {
			kept.events.push_back(e);
		}
	}
	if (r_dropped) {
		*r_dropped = p_message.events.size() - kept.events.size();
	}
	return encode_as(MessageType::LogicEvents, kMaxReliableBytes, kept);
}

core::Result<LogicEvents> decode_logic_events(std::span<const uint8_t> p_datagram) {
	return decode_as<LogicEvents>(p_datagram, { MessageType::LogicEvents, "LogicEvents" });
}

std::vector<uint8_t> encode_full_sync(const FullSync &p_message) {
	FullSync m = p_message;
	sim::logic::LogicGhost &g = m.ghost;
	g.bool_count = std::min(g.bool_count, sim::logic::kMaxGhostEntries);
	g.bool_bits.resize(ghost_words(g.bool_count));
	if (const size_t tail = g.bool_count % kGhostBoolsPerWord; tail != 0) {
		g.bool_bits.back() &= (uint64_t(1) << tail) - 1;
	}
	g.transforms.resize(std::min(g.transforms.size(), sim::logic::kMaxGhostEntries));
	g.sequence_start.resize(std::min(g.sequence_start.size(), sim::logic::kMaxGhostEntries));
	return encode_as(MessageType::FullSync, kMaxReliableBytes, m);
}

core::Result<FullSync> decode_full_sync(std::span<const uint8_t> p_datagram) {
	return decode_as<FullSync>(p_datagram, { MessageType::FullSync, "FullSync" });
}

std::vector<uint8_t> encode_logic_delta(const LogicDelta &p_message, size_t *r_dropped) {
	LogicDelta kept;
	kept.tick = p_message.tick;
	for (const sim::logic::LogicGhost::BoolDelta &row : p_message.rows) {
		if (row.index < sim::logic::kMaxGhostEntries && (kept.rows.empty() || row.index > kept.rows.back().index)) {
			kept.rows.push_back(row);
		}
	}
	if (r_dropped) {
		*r_dropped = p_message.rows.size() - kept.rows.size();
	}
	return encode_as(MessageType::LogicDelta, kMaxReliableBytes, kept);
}

core::Result<LogicDelta> decode_logic_delta(std::span<const uint8_t> p_datagram) {
	return decode_as<LogicDelta>(p_datagram, { MessageType::LogicDelta, "LogicDelta" });
}

std::vector<uint8_t> encode_squad_request(const SquadRequest &p_message) {
	return encode_as(MessageType::SquadRequest, kMaxReliableBytes, p_message);
}

core::Result<SquadRequest> decode_squad_request(std::span<const uint8_t> p_datagram) {
	return decode_as<SquadRequest>(p_datagram, { MessageType::SquadRequest, "SquadRequest" });
}

std::vector<uint8_t> encode_squad_order(const SquadOrder &p_message) {
	return encode_as(MessageType::SquadOrder, kMaxReliableBytes, p_message);
}

core::Result<SquadOrder> decode_squad_order(std::span<const uint8_t> p_datagram) {
	return decode_as<SquadOrder>(p_datagram, { MessageType::SquadOrder, "SquadOrder" });
}

bool set_player_name(PlayerRow &r_row, std::string_view p_name) {
	r_row.name = {};
	size_t n = std::min(p_name.size(), kMaxPlayerNameBytes);
	while (n > 0 && !valid_utf8(p_name.substr(0, n))) {
		--n;
	}
	std::copy_n(p_name.data(), n, r_row.name.data());
	r_row.name_bytes = uint8_t(n);
	return n != p_name.size();
}

std::string_view player_name(const PlayerRow &p_row) {
	return std::string_view(p_row.name.data(), std::min<size_t>(p_row.name_bytes, kMaxPlayerNameBytes));
}

}

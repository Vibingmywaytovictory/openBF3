// M1 T3 (docs/design/m1.md §7, §14): bit packing (net.bits.unit), the protocol's messages (net.proto.unit) and
// the network-conditions simulator (net.netsim.unit). No data. The decoders take hostile bytes, so every message is
// also fed truncated, extended, bit-flipped and random datagrams; the asan preset runs the same cases.
#include <doctest/doctest.h>

#include "bf3/net/bit_stream.h"
#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace net = bf3::net;
namespace sim = bf3::sim;

namespace {

// splitmix64: the tests' own seeded stream (independent of the netsim's).
struct Rng {
	uint64_t s;
	explicit Rng(uint64_t p_seed) :
			s(p_seed) {}
	uint64_t next() {
		uint64_t z = (s += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	uint32_t below(uint32_t p_n) { return p_n == 0 ? 0 : uint32_t(next() % p_n); }
	float uniform(float p_lo, float p_hi) { return p_lo + (p_hi - p_lo) * float(double(next() >> 11) / 9007199254740992.0); }
};

sim::SoldierState sample_state(Rng &r_rng, sim::Tick p_tick) {
	sim::SoldierState s;
	s.tick = p_tick;
	s.life = r_rng.below(1000) + 1;
	s.position = { r_rng.uniform(-300, 300), r_rng.uniform(50, 80), r_rng.uniform(-300, 300) };
	s.velocity = { r_rng.uniform(-7, 7), r_rng.uniform(-20, 4), r_rng.uniform(-7, 7) };
	s.ground_normal = { 0.1f, 0.99f, -0.05f };
	s.yaw = uint16_t(r_rng.next());
	s.pitch = int16_t(int32_t(r_rng.below(2 * 8191 + 1)) - 8191);
	s.pose = sim::Pose(r_rng.below(3));
	s.target_pose = sim::Pose(r_rng.below(3));
	s.transition_total = uint16_t(r_rng.below(64));
	s.transition_ticks = s.transition_total ? uint16_t(r_rng.below(s.transition_total)) : 0;
	s.mode = sim::MoveMode(r_rng.below(uint32_t(sim::kMoveModeCount)));
	s.flags = uint16_t(r_rng.below(64));
	s.events = uint16_t(r_rng.below(256));
	s.jump_penalty_ticks = uint16_t(r_rng.below(61));
	s.air_ticks = uint16_t(r_rng.below(500));
	s.protect_ticks = uint16_t(r_rng.below(121));
	s.health = r_rng.uniform(0, 100);
	s.impact_speed = r_rng.uniform(0, 30);
	s.damage = r_rng.uniform(0, 100);
	return s;
}

sim::Command sample_command(Rng &r_rng, sim::Tick p_tick, uint32_t p_sequence) {
	sim::Command c;
	c.tick = p_tick;
	c.sequence = p_sequence;
	c.buttons = uint32_t(r_rng.next()) & sim::buttons::kWireMask;
	c.move_forward = int8_t(int32_t(r_rng.below(255)) - 127);
	c.move_right = int8_t(int32_t(r_rng.below(255)) - 127);
	c.yaw = uint16_t(r_rng.next());
	c.pitch = int16_t(int32_t(r_rng.below(2 * 8191 + 1)) - 8191);
	c.pose = sim::Pose(r_rng.below(3));
	return c;
}

net::Snapshot sample_snapshot(Rng &r_rng, size_t p_others) {
	net::Snapshot s;
	s.seq = uint16_t(r_rng.next());
	s.server_tick = 100000 + r_rng.below(100000);
	s.applied_sequence = uint32_t(r_rng.next());
	s.applied_repeated = (r_rng.next() & 1) != 0;
	s.command_buffer_ticks = int8_t(int32_t(r_rng.below(13)) - 6);
	s.sequence_echo = uint16_t(r_rng.next());
	s.echo_hold_100us = uint16_t(r_rng.below(400));
	s.own_events = uint16_t(r_rng.below(256));
	s.own = sample_state(r_rng, s.server_tick);
	for (size_t i = 0; i < p_others; ++i) {
		s.others.push_back(net::quantize_other(uint8_t(i + 1), sample_state(r_rng, s.server_tick)));
	}
	return s;
}

bool same_other(const net::OtherSoldier &p_a, const net::OtherSoldier &p_b) {
	return p_a.slot == p_b.slot && p_a.generation == p_b.generation && p_a.position == p_b.position &&
			p_a.velocity == p_b.velocity && p_a.yaw == p_b.yaw && p_a.pitch == p_b.pitch && p_a.pose == p_b.pose &&
			p_a.target_pose == p_b.target_pose && p_a.transition == p_b.transition && p_a.mode == p_b.mode &&
			p_a.flags == p_b.flags;
}

bool same_snapshot(const net::Snapshot &p_a, const net::Snapshot &p_b) {
	if (p_a.seq != p_b.seq || p_a.server_tick != p_b.server_tick || p_a.applied_sequence != p_b.applied_sequence ||
			p_a.applied_repeated != p_b.applied_repeated || p_a.command_buffer_ticks != p_b.command_buffer_ticks ||
			p_a.sequence_echo != p_b.sequence_echo || p_a.echo_hold_100us != p_b.echo_hold_100us ||
			p_a.own_events != p_b.own_events || p_a.own != p_b.own || p_a.others.size() != p_b.others.size()) {
		return false;
	}
	for (size_t i = 0; i < p_a.others.size(); ++i) {
		if (!same_other(p_a.others[i], p_b.others[i])) {
			return false;
		}
	}
	return true;
}

bool same_packet(const net::CommandPacket &p_a, const net::CommandPacket &p_b) {
	if (p_a.newest_snapshot != p_b.newest_snapshot || p_a.snapshot_ack_bits != p_b.snapshot_ack_bits || p_a.count != p_b.count) {
		return false;
	}
	for (size_t i = 0; i < p_a.count; ++i) {
		if (!(p_a.commands[i] == p_b.commands[i])) {
			return false;
		}
	}
	return true;
}

bool same_report(const net::DebugReport &p_a, const net::DebugReport &p_b) {
	if (p_a.id != p_b.id || p_a.ok != p_b.ok || p_a.text != p_b.text || p_a.sweep.size() != p_b.sweep.size() ||
			p_a.trace != p_b.trace) {
		return false;
	}
	for (size_t i = 0; i < p_a.sweep.size(); ++i) {
		const net::SpawnSweepRow &x = p_a.sweep[i];
		const net::SpawnSweepRow &y = p_b.sweep[i];
		if (x.index != y.index || x.ok != y.ok || x.point != y.point || x.settled != y.settled || x.lift != y.lift ||
				x.shift != y.shift || x.gap != y.gap || x.grounded != y.grounded || x.overlapping != y.overlapping) {
			return false;
		}
	}
	return true;
}

// One decoder per message type, re-encoding what it decoded: a datagram that decodes must re-encode to bytes that
// decode to the same message (the fuzz cases' idempotence check). Returns -1 = refused, 1 = decoded and stable,
// 0 = decoded but unstable (a bug).
int decode_reencode(std::span<const uint8_t> p_bytes) {
	const std::optional<net::MessageType> type = net::peek_type(p_bytes);
	if (!type) {
		return -1;
	}
	switch (*type) {
		case net::MessageType::Snapshot: {
			auto a = net::decode_snapshot(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_snapshot(net::encode_snapshot(*a));
			return b && same_snapshot(*a, *b) ? 1 : 0;
		}
		case net::MessageType::CommandPacket: {
			auto a = net::decode_command_packet(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_command_packet(net::encode_command_packet(*a));
			return b && same_packet(*a, *b) ? 1 : 0;
		}
		case net::MessageType::Hello: {
			auto a = net::decode_hello(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_hello(net::encode_hello(*a));
			return b && b->protocol == a->protocol && b->name == a->name ? 1 : 0;
		}
		case net::MessageType::MatchInfo: {
			auto a = net::decode_match_info(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_match_info(net::encode_match_info(*a));
			return b && b->protocol == a->protocol && b->level == a->level && b->mode == a->mode && b->sim_hz == a->sim_hz &&
							b->snapshot_divisor == a->snapshot_divisor && b->slot == a->slot && b->server_tick == a->server_tick &&
							b->phase == a->phase && b->source_digest == a->source_digest && b->motor_hash == a->motor_hash
					? 1
					: 0;
		}
		case net::MessageType::Ready: {
			auto a = net::decode_ready(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_ready(net::encode_ready(*a));
			return b && b->source_digest == a->source_digest ? 1 : 0;
		}
		case net::MessageType::Spawned: {
			auto a = net::decode_spawned(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_spawned(net::encode_spawned(*a));
			return b && b->slot == a->slot && b->spawn_index == a->spawn_index && b->state == a->state ? 1 : 0;
		}
		case net::MessageType::Loading: {
			auto a = net::decode_loading(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_loading(net::encode_loading(*a));
			return b && b->stage == a->stage && b->permille == a->permille ? 1 : 0;
		}
		case net::MessageType::Kick: {
			auto a = net::decode_kick(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_kick(net::encode_kick(*a));
			return b && b->reason == a->reason ? 1 : 0;
		}
		case net::MessageType::Bye:
			return p_bytes.size() == 1 ? 1 : -1;
		case net::MessageType::Debug: {
			auto a = net::decode_debug(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_debug(net::encode_debug(*a));
			return b && b->kind == a->kind && b->id == a->id && b->position == a->position && b->yaw == a->yaw &&
							b->index == a->index && b->ticks == a->ticks
					? 1
					: 0;
		}
		case net::MessageType::DebugReport: {
			auto a = net::decode_debug_report(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_debug_report(net::encode_debug_report(*a));
			return b && same_report(*a, *b) ? 1 : 0;
		}
	}
	return -1;
}

// A sample of every message, encoded (the truncation and mutation cases start from these).
std::vector<std::pair<std::string, std::vector<uint8_t>>> sample_datagrams() {
	Rng rng(42);
	std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
	out.emplace_back("Snapshot", net::encode_snapshot(sample_snapshot(rng, 12)));
	net::Snapshot bare = sample_snapshot(rng, 0);
	bare.own.reset();
	out.emplace_back("Snapshot (no own)", net::encode_snapshot(bare));
	net::CommandPacket p;
	p.newest_snapshot = 777;
	p.snapshot_ack_bits = 0xFFFF00FFu;
	p.count = 4;
	for (uint32_t i = 0; i < 4; ++i) {
		p.commands[i] = sample_command(rng, 5000 - i, 900 - i);
	}
	p.commands[2] = p.commands[1];
	p.commands[2].tick = 4998;
	p.commands[2].sequence = 898;
	out.emplace_back("CommandPacket", net::encode_command_packet(p));
	out.emplace_back("Hello", net::encode_hello({ net::kProtocolVersion, "player\xC3\xA9" }));
	net::MatchInfo mi;
	mi.level = "MP_Subway";
	mi.mode = "TeamDeathMatch0";
	mi.slot = 3;
	mi.server_tick = 123456;
	mi.source_digest = bf3::core::Sha256Digest{};
	mi.source_digest->bytes[5] = 0xAB;
	mi.motor_hash = 0x0123456789ABCDEFull;
	out.emplace_back("MatchInfo", net::encode_match_info(mi));
	out.emplace_back("Ready", net::encode_ready({ mi.source_digest }));
	out.emplace_back("Spawned", net::encode_spawned({ 3, 17, sample_state(rng, 4000) }));
	out.emplace_back("Loading", net::encode_loading({ 4, 500 }));
	out.emplace_back("Kick", net::encode_kick({ "motor hash differs" }));
	out.emplace_back("Bye", net::encode_bye());
	net::DebugRequest dr;
	dr.kind = net::DebugKind::Teleport;
	dr.id = 9;
	dr.position = { 1.5f, 70.25f, -20.0f };
	dr.yaw = 16384;
	out.emplace_back("Debug", net::encode_debug(dr));
	net::DebugReport rep;
	rep.id = 9;
	rep.ok = true;
	rep.text = "sweep 2/2 ok";
	rep.sweep.push_back({ 0, true, { 1, 2, 3 }, { 1, 2.01f, 3 }, 0.01f, 0.0f, 0.02f, true, false });
	rep.sweep.push_back({ 1, false, { -1, 2, 3 }, { -1, 1, 3 }, 0.5f, 0.1f, 0.0f, false, true });
	for (uint32_t i = 0; i < 5; ++i) {
		rep.trace.push_back(sample_state(rng, 7000 + i));
	}
	out.emplace_back("DebugReport", net::encode_debug_report(rep));
	return out;
}

// Bytes built by hand, for hostile inputs the encoders never produce.
template <class F>
std::vector<uint8_t> craft(net::MessageType p_type, F &&p_body) {
	net::BitWriter w(1 << 20);
	w.write_bits(uint8_t(p_type), 8);
	p_body(w);
	return w.take();
}

void write_state_raw(net::BitWriter &r_w, const sim::SoldierState &p_s, uint8_t p_pose, uint8_t p_mode) {
	r_w.write_bits(p_s.tick, 32);
	r_w.write_bits(p_s.life, 32);
	for (const float f : p_s.position) {
		r_w.write_f32(f);
	}
	for (const float f : p_s.velocity) {
		r_w.write_f32(f);
	}
	for (const float f : p_s.ground_normal) {
		r_w.write_f32(f);
	}
	r_w.write_bits(p_s.yaw, 16);
	r_w.write_signed(p_s.pitch, 16);
	r_w.write_bits(p_pose, 8);
	r_w.write_bits(uint8_t(p_s.target_pose), 8);
	r_w.write_bits(p_s.transition_ticks, 16);
	r_w.write_bits(p_s.transition_total, 16);
	r_w.write_bits(p_mode, 8);
	r_w.write_bits(p_s.flags, 16);
	r_w.write_bits(p_s.events, 16);
	r_w.write_bits(p_s.jump_penalty_ticks, 16);
	r_w.write_bits(p_s.air_ticks, 16);
	r_w.write_bits(p_s.protect_ticks, 16);
	r_w.write_f32(p_s.health);
	r_w.write_f32(p_s.impact_speed);
	r_w.write_f32(p_s.damage);
}

} // namespace

// =================================================================================================================
TEST_SUITE("net-bits") {
	TEST_CASE("bits: least significant bit first into little-endian bytes") {
		net::BitWriter w(16);
		w.write_bits(1, 1);
		w.write_bits(2, 2); // 0b10 -> bits 1..2
		w.write_bits(0x1F, 5);
		CHECK(w.byte_count() == 1);
		CHECK(w.bytes()[0] == uint8_t(0b11111101));
		w.write_bits(0xABCD, 16);
		REQUIRE(w.byte_count() == 3);
		CHECK(w.bytes()[1] == 0xCD);
		CHECK(w.bytes()[2] == 0xAB);
		w.write_bits(0x3, 3); // a partial byte, zero padded
		CHECK(w.bit_count() == 27);
		CHECK(w.byte_count() == 4);
		CHECK(w.bytes()[3] == 0x03);
		w.write_bits(0xFFFF, 0); // nothing
		CHECK(w.bit_count() == 27);
		w.write_bits(0xF0, 4); // high bits of the value are ignored
		CHECK(w.bytes()[3] == 0x03);
	}

	TEST_CASE("bits: seeded round trip of every width, signed values, varints and floats") {
		Rng rng(7);
		struct Item {
			int kind;
			unsigned bits;
			uint64_t u;
			int64_t s;
			float f;
		};
		std::vector<Item> items;
		net::BitWriter w(1 << 20);
		for (int i = 0; i < 20000; ++i) {
			Item it{ int(rng.below(4)), 0, 0, 0, 0.0f };
			switch (it.kind) {
				case 0:
					it.bits = rng.below(65);
					it.u = it.bits == 0 ? 0 : (rng.next() & (it.bits == 64 ? ~0ull : ((1ull << it.bits) - 1)));
					w.write_bits(it.u, it.bits);
					break;
				case 1: {
					it.bits = 1 + rng.below(63);
					const int64_t lo = -(int64_t(1) << (it.bits - 1));
					const int64_t hi = (int64_t(1) << (it.bits - 1)) - 1;
					const uint32_t pick = rng.below(3);
					it.s = pick == 0 ? lo : pick == 1 ? hi : lo + int64_t(rng.next() % uint64_t(hi - lo + 1));
					w.write_signed(it.s, it.bits);
					break;
				}
				case 2:
					it.u = rng.next() >> rng.below(64);
					w.write_varint(it.u);
					break;
				default:
					it.f = std::bit_cast<float>(uint32_t(rng.next()));
					w.write_f32(it.f);
					break;
			}
			items.push_back(it);
		}
		REQUIRE_FALSE(w.overflowed());
		const std::vector<uint8_t> bytes = w.take();
		net::BitReader r(bytes);
		for (const Item &it : items) {
			switch (it.kind) {
				case 0:
					REQUIRE(r.read_bits(it.bits) == it.u);
					break;
				case 1:
					REQUIRE(r.read_signed(it.bits) == it.s);
					break;
				case 2:
					REQUIRE(r.read_varint() == it.u);
					break;
				default:
					REQUIRE(std::bit_cast<uint32_t>(r.read_f32()) == std::bit_cast<uint32_t>(it.f));
					break;
			}
		}
		CHECK_FALSE(r.failed());
		CHECK(r.at_end());
	}

	TEST_CASE("bits: varint edge values and bounds") {
		const uint64_t values[] = { 0, 1, 127, 128, 16383, 16384, (1ull << 35) - 1, 1ull << 63, ~0ull };
		for (const uint64_t v : values) {
			net::BitWriter w(32);
			w.write_varint(v);
			const std::vector<uint8_t> b = w.take();
			net::BitReader r(b);
			CHECK(r.read_varint() == v);
			CHECK(r.at_end());
		}
		{
			net::BitWriter w(32);
			w.write_varint(127);
			CHECK(w.bit_count() == 8);
			w.write_varint(128);
			CHECK(w.bit_count() == 24);
			w.write_varint(~0ull);
			CHECK(w.bit_count() == 24 + 80); // 10 groups at most
		}
		// An eleventh group, or a tenth group with more than bit 63, does not fit 64 bits.
		{
			net::BitWriter w(32);
			for (int i = 0; i < 10; ++i) {
				w.write_bool(true);
				w.write_bits(0x7F, 7);
			}
			w.write_bool(false);
			w.write_bits(0, 7);
			const std::vector<uint8_t> b = w.take();
			net::BitReader r(b);
			CHECK(r.read_varint() == 0);
			CHECK(r.failed());
		}
		{
			net::BitWriter w(32);
			for (int i = 0; i < 9; ++i) {
				w.write_bool(true);
				w.write_bits(0x7F, 7);
			}
			w.write_bool(false);
			w.write_bits(2, 7);
			const std::vector<uint8_t> b = w.take();
			net::BitReader r(b);
			CHECK(r.read_varint() == 0);
			CHECK(r.failed());
		}
		{ // truncated in the middle of a group sequence
			net::BitWriter w(32);
			w.write_varint(1ull << 40);
			std::vector<uint8_t> b = w.take();
			b.pop_back();
			net::BitReader r(b);
			CHECK(r.read_varint() == 0);
			CHECK(r.failed());
		}
	}

	TEST_CASE("bits: the writer's byte cap sets overflowed() and drops every later write") {
		net::BitWriter w(2);
		w.write_bits(0xFFFF, 16);
		CHECK_FALSE(w.overflowed());
		w.write_bool(true);
		CHECK(w.overflowed());
		CHECK(w.bit_count() == 16);
		CHECK(w.byte_count() == 2);
		w.write_bits(0, 0);
		CHECK(w.bit_count() == 16);
		net::BitWriter w2(8);
		w2.write_bits(1, 65); // more than 64 bits is a caller bug: refused
		CHECK(w2.overflowed());
		CHECK(w2.bit_count() == 0);
		net::BitWriter w3(1);
		w3.write_bits(1, 3);
		w3.write_bits(1, 6); // 9 bits > 1 byte: dropped whole, not split
		CHECK(w3.overflowed());
		CHECK(w3.bit_count() == 3);
		net::BitWriter w4(0);
		w4.write_bool(false);
		CHECK(w4.overflowed());
		CHECK(w4.byte_count() == 0);
		// take() hands the bytes over and resets the writer.
		std::vector<uint8_t> b = w.take();
		CHECK(b.size() == 2);
		CHECK_FALSE(w.overflowed());
		CHECK(w.bit_count() == 0);
	}

	TEST_CASE("bits: the reader yields 0 past its end and the fail flag sticks") {
		const uint8_t bytes[] = { 0xA5, 0x0F };
		net::BitReader r(bytes);
		CHECK(r.bits_left() == 16);
		CHECK(r.read_bits(4) == 0x5);
		CHECK(r.read_bits(12) == 0x0FA);
		CHECK(r.bits_left() == 0);
		CHECK(r.at_end());
		CHECK(r.read_bits(1) == 0);
		CHECK(r.failed());
		CHECK_FALSE(r.at_end());
		net::BitReader r2(bytes);
		CHECK(r2.read_bits(17) == 0); // more than is left: nothing is read
		CHECK(r2.failed());
		CHECK(r2.read_bits(1) == 0); // sticky
		net::BitReader r3(bytes);
		CHECK(r3.read_bits(65) == 0);
		CHECK(r3.failed());
		net::BitReader r4(std::span<const uint8_t>{});
		CHECK(r4.at_end());
		CHECK(r4.read_bool() == false);
		CHECK(r4.failed());
		net::BitReader r5(bytes);
		CHECK(std::bit_cast<uint32_t>(r5.read_f32()) == 0);
		CHECK(r5.failed());
	}

	TEST_CASE("bits: at_end allows the pad bits of the last byte only") {
		net::BitWriter w(8);
		w.write_bits(0x5, 3);
		std::vector<uint8_t> b = w.take();
		{
			net::BitReader r(b);
			CHECK(r.read_bits(3) == 5);
			CHECK(r.at_end());
			CHECK(r.bits_left() == 5);
		}
		b.push_back(0);
		{
			net::BitReader r(b);
			CHECK(r.read_bits(3) == 5);
			CHECK_FALSE(r.at_end());
		}
	}

	TEST_CASE("bits: align_byte on both sides") {
		net::BitWriter w(8);
		w.write_bits(1, 1);
		w.align_byte();
		CHECK(w.bit_count() == 8);
		w.align_byte(); // already aligned
		CHECK(w.bit_count() == 8);
		w.write_bits(0x7F, 8);
		const std::vector<uint8_t> b = w.take();
		REQUIRE(b.size() == 2);
		net::BitReader r(b);
		CHECK(r.read_bits(1) == 1);
		r.align_byte();
		CHECK(r.read_bits(8) == 0x7F);
		CHECK(r.at_end());
		r.align_byte();
		CHECK_FALSE(r.failed());
	}

	TEST_CASE("bits: quantisers stay within half a step, clamp outside their range and re-encode exactly") {
		const net::Quantizer qs[] = { net::quant::kPositionXZ, net::quant::kPositionY, net::quant::kSoldierVelocity };
		Rng rng(11);
		for (const net::Quantizer &q : qs) {
			const float top = q.decode(q.max_code());
			for (int i = 0; i < 200000; ++i) {
				const float v = rng.uniform(q.min, top);
				const float e = std::fabs(q.decode(q.encode(v)) - v);
				REQUIRE(e <= q.step * 0.5f + std::fabs(v) * 6e-8f); // half a step, plus the float rounding of v itself
			}
			CHECK(q.encode(q.min - 100.0f) == 0);
			CHECK(q.encode(top + 100.0f) == q.max_code());
			CHECK(q.encode(std::numeric_limits<float>::quiet_NaN()) == 0);
			CHECK(q.encode(std::numeric_limits<float>::infinity()) == q.max_code());
			CHECK(q.encode(-std::numeric_limits<float>::infinity()) == 0);
			CHECK(q.decode(q.max_code() + 5) == top); // a code above the range decodes as the top
			// Every code's decoded value encodes back to the code: quantize_other is idempotent on the wire.
			uint32_t bad = 0;
			for (uint32_t c = 0; c <= q.max_code(); ++c) {
				bad += q.encode(q.decode(c)) != c ? 1 : 0;
			}
			CHECK(bad == 0);
		}
	}
}

// =================================================================================================================
TEST_SUITE("net-proto") {
	TEST_CASE("proto: peek_type knows every message and nothing else") {
		CHECK_FALSE(net::peek_type({}).has_value());
		int known = 0;
		for (int t = 0; t < 256; ++t) {
			const uint8_t b = uint8_t(t);
			known += net::peek_type(std::span<const uint8_t>(&b, 1)).has_value() ? 1 : 0;
		}
		CHECK(known == 11);
		const uint8_t s = 1;
		CHECK(net::peek_type(std::span<const uint8_t>(&s, 1)) == net::MessageType::Snapshot);
	}

	TEST_CASE("proto: every control message round trips") {
		Rng rng(3);
		{
			const net::Hello h{ 7, "Soldier \xE2\x82\xAC" };
			auto d = net::decode_hello(net::encode_hello(h));
			REQUIRE(d);
			CHECK(d->protocol == 7);
			CHECK(d->name == h.name);
		}
		{
			net::MatchInfo m;
			m.level = "MP_Subway";
			m.mode = "TeamDeathMatch0";
			m.sim_hz = 60;
			m.snapshot_divisor = 2;
			m.slot = 127;
			m.server_tick = 0xFFFFFFF0u;
			m.phase = net::MatchPhase::RoundOver;
			m.motor_hash = ~0ull;
			for (int with_digest = 0; with_digest < 2; ++with_digest) {
				if (with_digest) {
					bf3::core::Sha256Digest dg;
					for (size_t i = 0; i < dg.bytes.size(); ++i) {
						dg.bytes[i] = uint8_t(i * 7 + 1);
					}
					m.source_digest = dg;
				}
				auto d = net::decode_match_info(net::encode_match_info(m));
				REQUIRE(d);
				CHECK(d->protocol == net::kProtocolVersion);
				CHECK(d->level == m.level);
				CHECK(d->mode == m.mode);
				CHECK(d->sim_hz == 60);
				CHECK(d->snapshot_divisor == 2);
				CHECK(d->slot == 127);
				CHECK(d->server_tick == m.server_tick);
				CHECK(d->phase == net::MatchPhase::RoundOver);
				CHECK(d->source_digest == m.source_digest);
				CHECK(d->motor_hash == m.motor_hash);
			}
		}
		{
			auto d = net::decode_ready(net::encode_ready({}));
			REQUIRE(d);
			CHECK_FALSE(d->source_digest.has_value());
		}
		{
			const net::Spawned s{ 5, 101, sample_state(rng, 4242) };
			const std::vector<uint8_t> b = net::encode_spawned(s);
			CHECK(b.size() == 1 + 1 + 4 + 77); // the own-state block is 77 bytes (docs §7.3)
			auto d = net::decode_spawned(b);
			REQUIRE(d);
			CHECK(d->slot == 5);
			CHECK(d->spawn_index == 101);
			CHECK(d->state == s.state);
		}
		{
			auto d = net::decode_loading(net::encode_loading({ 9, 1000 }));
			REQUIRE(d);
			CHECK(d->stage == 9);
			CHECK(d->permille == 1000);
			auto clamped = net::decode_loading(net::encode_loading({ 1, 5000 }));
			REQUIRE(clamped);
			CHECK(clamped->permille == 1000);
		}
		{
			auto d = net::decode_kick(net::encode_kick({ "bye now" }));
			REQUIRE(d);
			CHECK(d->reason == "bye now");
			const std::vector<uint8_t> bye = net::encode_bye();
			CHECK(bye.size() == 1);
			CHECK(net::peek_type(bye) == net::MessageType::Bye);
		}
		for (uint8_t kind = 1; kind <= 5; ++kind) {
			net::DebugRequest r;
			r.kind = net::DebugKind(kind);
			r.id = 1000u + kind;
			r.position = { -12.5f, 66.0f, 1e5f };
			r.yaw = 0xBEEF;
			r.index = 101;
			r.ticks = net::kMaxTraceTicks;
			auto d = net::decode_debug(net::encode_debug(r));
			REQUIRE(d);
			CHECK(d->kind == r.kind);
			CHECK(d->id == r.id);
			CHECK(d->position == r.position);
			CHECK(d->yaw == r.yaw);
			CHECK(d->index == r.index);
			CHECK(d->ticks == r.ticks);
		}
	}

	TEST_CASE("proto: a DebugReport carries a full 1,200-tick trace and the sweep rows") {
		Rng rng(5);
		net::DebugReport rep;
		rep.id = 77;
		rep.ok = true;
		rep.text = "trace 1200 ticks";
		for (uint32_t i = 0; i < net::kMaxTraceTicks; ++i) {
			rep.trace.push_back(sample_state(rng, 50000 + i));
		}
		for (uint32_t i = 0; i < 102; ++i) {
			rep.sweep.push_back({ i, (i % 7) != 0, { float(i), 60.0f, -float(i) }, { float(i), 60.1f, -float(i) }, 0.1f, 0.01f,
					0.02f, true, false });
		}
		const std::vector<uint8_t> b = net::encode_debug_report(rep);
		MESSAGE("DebugReport with 1,200 trace states and 102 sweep rows: ", b.size(), " bytes");
		CHECK(b.size() < 100 * 1024);
		auto d = net::decode_debug_report(b);
		REQUIRE(d);
		CHECK(same_report(*d, rep));
		// Over the limits: the encoder clamps, a crafted count above them is refused.
		rep.trace.push_back(sample_state(rng, 1));
		auto clamped = net::decode_debug_report(net::encode_debug_report(rep));
		REQUIRE(clamped);
		CHECK(clamped->trace.size() == net::kMaxTraceTicks);
		const auto crafted = craft(net::MessageType::DebugReport, [&](net::BitWriter &w) {
			w.write_bits(1, 32);
			w.write_bool(true);
			w.write_varint(0);
			w.write_varint(0);
			w.write_varint(net::kMaxTraceTicks + 1);
			for (uint32_t i = 0; i <= net::kMaxTraceTicks; ++i) {
				write_state_raw(w, rep.trace[0], 0, 0);
			}
		});
		CHECK_FALSE(net::decode_debug_report(crafted));
		// A count far beyond the bytes is refused before anything is allocated.
		const auto huge = craft(net::MessageType::DebugReport, [&](net::BitWriter &w) {
			w.write_bits(1, 32);
			w.write_bool(true);
			w.write_varint(0);
			w.write_varint(~0ull);
		});
		CHECK_FALSE(net::decode_debug_report(huge));
		const auto rows_beyond = craft(net::MessageType::DebugReport, [&](net::BitWriter &w) {
			w.write_bits(1, 32);
			w.write_bool(true);
			w.write_varint(0);
			w.write_varint(1000); // under the row limit, but the bytes are not there
		});
		CHECK_FALSE(net::decode_debug_report(rows_beyond));
	}

	TEST_CASE("proto: command packet sizes are 26 and 53 bytes, with 4 commands of redundancy") {
		Rng rng(9);
		net::CommandPacket p;
		p.newest_snapshot = 65535;
		p.snapshot_ack_bits = 0x80000001u;
		p.count = 4;
		p.commands[0] = sample_command(rng, 10000, 20000);
		for (uint32_t i = 1; i < 4; ++i) {
			p.commands[i] = p.commands[0];
			p.commands[i].tick = 10000 - i;
			p.commands[i].sequence = 20000 - i;
		}
		const std::vector<uint8_t> same = net::encode_command_packet(p);
		CHECK(same.size() == 26);
		auto ds = net::decode_command_packet(same);
		REQUIRE(ds);
		CHECK(same_packet(*ds, p));
		for (uint32_t i = 1; i < 4; ++i) {
			p.commands[i] = sample_command(rng, 10000 - i, 20000 - i);
			p.commands[i].yaw = uint16_t(p.commands[i - 1].yaw + 1); // never equal to the newer one
		}
		const std::vector<uint8_t> differ = net::encode_command_packet(p);
		CHECK(differ.size() == 53);
		auto dd = net::decode_command_packet(differ);
		REQUIRE(dd);
		CHECK(same_packet(*dd, p));
		// A single command: header 8 B + 17 B.
		p.count = 1;
		CHECK(net::encode_command_packet(p).size() == 25);
		// Server-side flags never travel; buttons above the 24 wire bits are dropped.
		p.count = 2;
		p.commands[0].flags = sim::command_flags::kRepeated;
		p.commands[1].flags = sim::command_flags::kLocalSource;
		p.commands[0].buttons = 0xFF000000u | sim::buttons::kJump;
		auto df = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(df);
		CHECK(df->commands[0].flags == 0);
		CHECK(df->commands[1].flags == 0);
		CHECK(df->commands[0].buttons == sim::buttons::kJump);
	}

	TEST_CASE("proto: the encoder cuts count at the first command that is not one tick and one sequence older") {
		Rng rng(10);
		net::CommandPacket p;
		p.count = 4;
		for (uint32_t i = 0; i < 4; ++i) {
			p.commands[i] = sample_command(rng, 500 - i, 70 - i);
		}
		p.commands[2].tick = 400; // after a clock reset
		auto d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		CHECK(d->count == 2);
		CHECK(d->commands[0] == p.commands[0]);
		CHECK(d->commands[1] == p.commands[1]);
		p.commands[2].tick = 498;
		p.commands[3].sequence = 1; // a sequence gap
		d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		CHECK(d->count == 3);
		p.commands[0].tick = 0; // the implied tick of an older command would wrap below 0
		d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		CHECK(d->count == 1);
		p.count = 0;
		CHECK(net::encode_command_packet(p).empty());
		p.count = 9; // clamped to kCommandsPerPacket
		p.commands[0].tick = 500;
		p.commands[3].sequence = 67;
		d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		CHECK(d->count == 4);
	}

	TEST_CASE("proto: hostile command packets are refused") {
		const auto packet = [](uint8_t p_count, uint32_t p_tick, uint32_t p_seq, uint32_t p_pose, int32_t p_pitch, int32_t p_fwd) {
			return craft(net::MessageType::CommandPacket, [&](net::BitWriter &w) {
				w.write_bits(1, 16);
				w.write_bits(0, 32);
				w.write_bits(p_count, 8);
				w.write_bits(p_tick, 32);
				w.write_bits(p_seq, 32);
				w.write_bits(0, 24);
				w.write_signed(p_fwd, 8);
				w.write_signed(0, 8);
				w.write_bits(0, 16);
				w.write_signed(p_pitch, 14);
				w.write_bits(p_pose, 2);
				for (int i = 1; i < p_count; ++i) {
					w.write_bool(true);
				}
			});
		};
		CHECK(net::decode_command_packet(packet(1, 10, 10, 2, 8191, -127)));
		CHECK(net::decode_command_packet(packet(4, 10, 10, 0, -8191, 127)));
		CHECK_FALSE(net::decode_command_packet(packet(0, 10, 10, 0, 0, 0))); // count 0
		CHECK_FALSE(net::decode_command_packet(packet(5, 10, 10, 0, 0, 0))); // count above 4
		CHECK_FALSE(net::decode_command_packet(packet(1, 10, 10, 3, 0, 0))); // pose 3
		CHECK_FALSE(net::decode_command_packet(packet(1, 10, 10, 0, 8192, 0))); // pitch beyond kPitchLimit
		CHECK_FALSE(net::decode_command_packet(packet(1, 10, 10, 0, -8192, 0)));
		CHECK_FALSE(net::decode_command_packet(packet(1, 10, 10, 0, 0, -128))); // axis -128 is never produced
		CHECK_FALSE(net::decode_command_packet(packet(2, 0, 10, 0, 0, 0))); // implied tick below 0
		CHECK_FALSE(net::decode_command_packet(packet(2, 10, 0, 0, 0, 0))); // implied sequence below 0
		// The encoder clamps to what the decoder accepts.
		net::CommandPacket p;
		p.count = 1;
		p.commands[0].pitch = 20000;
		p.commands[0].move_forward = -128;
		auto d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		CHECK(d->commands[0].pitch == sim::kPitchLimit);
		CHECK(d->commands[0].move_forward == -127);
	}

	TEST_CASE("proto: a snapshot with 55 others fits 1,200 bytes; more are dropped, never overflowed") {
		Rng rng(21);
		net::Snapshot s = sample_snapshot(rng, 0);
		const size_t header = net::encode_snapshot(s).size();
		MESSAGE("snapshot header + own state: ", header, " bytes");
		CHECK(header == 97); // 770 bits (docs §7.3: about 96 B)
		s = sample_snapshot(rng, 55);
		size_t dropped = 99;
		const std::vector<uint8_t> b = net::encode_snapshot(s, &dropped);
		MESSAGE("snapshot with own state and 55 others: ", b.size(), " bytes");
		CHECK(b.size() <= net::kMaxDatagramBytes);
		CHECK(dropped == 0);
		auto d = net::decode_snapshot(b);
		REQUIRE(d);
		CHECK(same_snapshot(*d, s));
		s = sample_snapshot(rng, 127);
		const std::vector<uint8_t> big = net::encode_snapshot(s, &dropped);
		CHECK(big.size() <= net::kMaxDatagramBytes);
		CHECK(dropped == 127 - 55);
		auto db = net::decode_snapshot(big);
		REQUIRE(db);
		REQUIRE(db->others.size() == 55);
		for (size_t i = 0; i < 55; ++i) {
			CHECK(same_other(db->others[i], s.others[i])); // the caller's order (nearest first) is kept
		}
		// Without the own state there is room for one more.
		s.own.reset();
		auto dn = net::decode_snapshot(net::encode_snapshot(s, &dropped));
		REQUIRE(dn);
		CHECK_FALSE(dn->own.has_value());
		CHECK(dn->others.size() == 59);
		CHECK(dropped == 127 - 59);
	}

	TEST_CASE("proto: snapshot invariants and hostile snapshots") {
		Rng rng(22);
		net::Snapshot s = sample_snapshot(rng, 3);
		s.own->tick = s.server_tick - 1; // the one-tick invariant (docs §7.3)
		auto d = net::decode_snapshot(net::encode_snapshot(s));
		CHECK_FALSE(d);
		s.own->tick = s.server_tick;
		s.others[2].slot = s.others[0].slot; // a slot named twice
		CHECK_FALSE(net::decode_snapshot(net::encode_snapshot(s)));
		s.others[2].slot = 200; // beyond kMaxSlots
		CHECK_FALSE(net::decode_snapshot(net::encode_snapshot(s)));
		s.others[2].slot = 127;
		CHECK(net::decode_snapshot(net::encode_snapshot(s)));
		// Own-state enums and floats.
		const auto with_own = [&](uint8_t p_pose, uint8_t p_mode, float p_x) {
			sim::SoldierState own = *s.own;
			own.position[0] = p_x;
			return craft(net::MessageType::Snapshot, [&](net::BitWriter &w) {
				w.write_bits(1, 16);
				w.write_bits(own.tick, 32);
				w.write_bits(0, 32);
				w.write_bool(false);
				w.write_signed(0, 8);
				w.write_bits(0, 16);
				w.write_bits(0, 16);
				w.write_bits(0, 16);
				w.write_bool(true);
				write_state_raw(w, own, p_pose, p_mode);
				w.write_bits(0, 8);
			});
		};
		CHECK(net::decode_snapshot(with_own(2, 8, 1.0f)));
		CHECK_FALSE(net::decode_snapshot(with_own(3, 0, 1.0f)));
		CHECK_FALSE(net::decode_snapshot(with_own(0, 9, 1.0f)));
		CHECK_FALSE(net::decode_snapshot(with_own(0, 0, std::numeric_limits<float>::quiet_NaN())));
		CHECK_FALSE(net::decode_snapshot(with_own(0, 0, std::numeric_limits<float>::infinity())));
		// An others count beyond the bytes, or beyond kMaxSlots.
		const auto with_count = [&](uint32_t p_count) {
			return craft(net::MessageType::Snapshot, [&](net::BitWriter &w) {
				w.write_bits(1, 16);
				w.write_bits(5, 32);
				w.write_bits(0, 32);
				w.write_bool(false);
				w.write_signed(0, 8);
				w.write_bits(0, 48);
				w.write_bool(false);
				w.write_bits(p_count, 8);
			});
		};
		CHECK(net::decode_snapshot(with_count(0)));
		CHECK_FALSE(net::decode_snapshot(with_count(1)));
		CHECK_FALSE(net::decode_snapshot(with_count(129)));
		CHECK_FALSE(net::decode_snapshot(with_count(255)));
	}

	TEST_CASE("proto: quantize_other is the receiver's exact view, within its quantisation") {
		Rng rng(23);
		for (int i = 0; i < 20000; ++i) {
			const sim::SoldierState st = sample_state(rng, 1);
			const net::OtherSoldier q = net::quantize_other(uint8_t(i % 128), st);
			REQUIRE(std::fabs(q.position[0] - st.position[0]) <= net::quant::kPositionXZ.step * 0.5f + 1e-6f);
			REQUIRE(std::fabs(q.position[1] - st.position[1]) <= net::quant::kPositionY.step * 0.5f + 1e-6f);
			REQUIRE(std::fabs(q.position[2] - st.position[2]) <= net::quant::kPositionXZ.step * 0.5f + 1e-6f);
			for (int k = 0; k < 3; ++k) {
				REQUIRE(std::fabs(q.velocity[k] - st.velocity[k]) <= net::quant::kSoldierVelocity.step * 0.5f + 1e-6f);
			}
			const int yaw_error = std::abs(int16_t(uint16_t(q.yaw - st.yaw)));
			REQUIRE(yaw_error <= 2);
			REQUIRE((q.yaw & 3) == 0);
			REQUIRE(std::abs(int(q.pitch) - int(st.pitch)) <= 3); // 2 by rounding, 3 at the clamp (+-8188)
			REQUIRE((q.pitch & 3) == 0);
			REQUIRE(q.generation == uint8_t(st.life & 0xFF));
			REQUIRE(q.flags == uint8_t(st.flags & 0xFF));
			// What the receiver decodes equals quantize_other bit for bit, and it is a fixed point.
			net::Snapshot s;
			s.server_tick = 1;
			s.others.push_back(q);
			auto d = net::decode_snapshot(net::encode_snapshot(s));
			REQUIRE(d);
			REQUIRE(d->others.size() == 1);
			REQUIRE(same_other(d->others[0], q));
		}
		// Transition progress: 0 = start, 255 = done; yaw wraps at a turn.
		sim::SoldierState st;
		st.transition_total = 42;
		st.transition_ticks = 41;
		CHECK(net::quantize_other(0, st).transition == 6); // round(1/42 * 255)
		st.transition_ticks = 21;
		CHECK(net::quantize_other(0, st).transition == 128);
		st.transition_ticks = 0;
		CHECK(net::quantize_other(0, st).transition == 255);
		st.transition_total = 0;
		CHECK(net::quantize_other(0, st).transition == 255);
		st.transition_total = 10;
		st.transition_ticks = 50; // inconsistent input clamps to the start
		CHECK(net::quantize_other(0, st).transition == 0);
		st.yaw = 0xFFFF;
		CHECK(net::quantize_other(0, st).yaw == 0);
		st.pitch = sim::kPitchLimit;
		CHECK(net::quantize_other(0, st).pitch == 8188);
		st.pitch = -sim::kPitchLimit;
		CHECK(net::quantize_other(0, st).pitch == -8188);
		st.life = 0x1234;
		CHECK(net::quantize_other(0, st).generation == 0x34);
	}

	TEST_CASE("proto: seq_newer across the 16-bit wrap") {
		for (uint32_t a = 0; a < 65536; ++a) {
			const uint16_t x = uint16_t(a);
			REQUIRE(net::seq_newer(uint16_t(x + 1), x));
			REQUIRE_FALSE(net::seq_newer(x, uint16_t(x + 1)));
			REQUIRE(net::seq_newer(uint16_t(x + 0x7FFF), x));
			REQUIRE_FALSE(net::seq_newer(uint16_t(x + 0x8000), x));
			REQUIRE_FALSE(net::seq_newer(x, x));
		}
	}

	TEST_CASE("proto: strings are bounded UTF-8") {
		// The encoder clamps a long name at a code point boundary.
		std::string name;
		for (int i = 0; i < 20; ++i) {
			name += "\xC3\xA9"; // 2 bytes each
		}
		name = "x" + name; // the 32-byte cut falls inside a sequence
		auto d = net::decode_hello(net::encode_hello({ 1, name }));
		REQUIRE(d);
		CHECK(d->name.size() == 31);
		CHECK(d->name == name.substr(0, 31));
		const auto hello = [](std::string_view p_bytes, uint64_t p_len) {
			return craft(net::MessageType::Hello, [&](net::BitWriter &w) {
				w.write_bits(1, 16);
				w.write_varint(p_len);
				for (const char c : p_bytes) {
					w.write_bits(uint8_t(c), 8);
				}
			});
		};
		CHECK(net::decode_hello(hello("abc", 3)));
		CHECK_FALSE(net::decode_hello(hello("abc", 4))); // longer than the bytes
		CHECK_FALSE(net::decode_hello(hello(std::string(33, 'a'), 33))); // above kMaxNameBytes
		CHECK_FALSE(net::decode_hello(hello("abc", ~0ull))); // never allocated
		CHECK_FALSE(net::decode_hello(hello("\xC3", 1))); // cut sequence
		CHECK_FALSE(net::decode_hello(hello("\xC0\xAF", 2))); // overlong
		CHECK_FALSE(net::decode_hello(hello("\xED\xA0\x80", 3))); // surrogate
		CHECK_FALSE(net::decode_hello(hello("\xF4\x90\x80\x80", 4))); // above U+10FFFF
		CHECK_FALSE(net::decode_hello(hello("\xFF", 1)));
		CHECK(net::decode_hello(hello("\xF0\x9F\x98\x80", 4))); // U+1F600
		std::string reason(5000, 'r');
		auto k = net::decode_kick(net::encode_kick({ reason }));
		REQUIRE(k);
		CHECK(k->reason.size() == 2048);
	}

	TEST_CASE("proto: hostile control messages are refused") {
		Rng rng(31);
		const sim::SoldierState st = sample_state(rng, 10);
		const auto spawned = [&](uint32_t p_slot, uint8_t p_pose, uint8_t p_mode, float p_health) {
			sim::SoldierState s = st;
			s.health = p_health;
			return craft(net::MessageType::Spawned, [&](net::BitWriter &w) {
				w.write_bits(p_slot, 8);
				w.write_bits(1, 32);
				write_state_raw(w, s, p_pose, p_mode);
			});
		};
		CHECK(net::decode_spawned(spawned(127, 2, 8, 50.0f)));
		CHECK_FALSE(net::decode_spawned(spawned(128, 0, 0, 50.0f)));
		CHECK_FALSE(net::decode_spawned(spawned(0, 3, 0, 50.0f)));
		CHECK_FALSE(net::decode_spawned(spawned(0, 0, 9, 50.0f)));
		CHECK_FALSE(net::decode_spawned(spawned(0, 0, 0, std::numeric_limits<float>::quiet_NaN())));
		sim::SoldierState bad_pitch = st;
		bad_pitch.pitch = 9000;
		CHECK_FALSE(net::decode_spawned(net::encode_spawned({ 0, 0, bad_pitch })));

		const auto debug = [](uint32_t p_kind, float p_x, uint32_t p_ticks) {
			return craft(net::MessageType::Debug, [&](net::BitWriter &w) {
				w.write_bits(p_kind, 8);
				w.write_bits(1, 32);
				w.write_f32(p_x);
				w.write_f32(0.0f);
				w.write_f32(0.0f);
				w.write_bits(0, 16);
				w.write_bits(0, 32);
				w.write_bits(p_ticks, 32);
			});
		};
		CHECK(net::decode_debug(debug(1, 10.0f, 60)));
		CHECK_FALSE(net::decode_debug(debug(0, 10.0f, 60)));
		CHECK_FALSE(net::decode_debug(debug(6, 10.0f, 60)));
		CHECK_FALSE(net::decode_debug(debug(1, std::numeric_limits<float>::infinity(), 60)));
		CHECK_FALSE(net::decode_debug(debug(1, 2.0e6f, 60)));
		CHECK_FALSE(net::decode_debug(debug(5, 0.0f, net::kMaxTraceTicks + 1)));

		const auto match = [](uint32_t p_hz, uint32_t p_div, uint32_t p_slot, uint32_t p_phase) {
			return craft(net::MessageType::MatchInfo, [&](net::BitWriter &w) {
				w.write_bits(1, 16);
				w.write_varint(0);
				w.write_varint(0);
				w.write_bits(p_hz, 16);
				w.write_bits(p_div, 8);
				w.write_bits(p_slot, 8);
				w.write_bits(0, 32);
				w.write_bits(p_phase, 8);
				w.write_bool(false);
				w.write_bits(0, 64);
			});
		};
		CHECK(net::decode_match_info(match(60, 2, 0, 2)));
		CHECK_FALSE(net::decode_match_info(match(0, 2, 0, 2)));
		CHECK_FALSE(net::decode_match_info(match(60, 0, 0, 2)));
		CHECK_FALSE(net::decode_match_info(match(60, 2, 128, 2)));
		CHECK_FALSE(net::decode_match_info(match(60, 2, 0, 4)));
		CHECK_FALSE(net::decode_loading(craft(net::MessageType::Loading, [](net::BitWriter &w) {
			w.write_bits(1, 8);
			w.write_bits(1001, 16);
		})));
		// Every decoder refuses another message's type byte.
		const std::vector<uint8_t> hello = net::encode_hello({ 1, "x" });
		CHECK_FALSE(net::decode_match_info(hello));
		CHECK_FALSE(net::decode_snapshot(hello));
		CHECK_FALSE(net::decode_command_packet(hello));
		CHECK_FALSE(net::decode_ready(hello));
		CHECK_FALSE(net::decode_spawned(hello));
		CHECK_FALSE(net::decode_loading(hello));
		CHECK_FALSE(net::decode_kick(hello));
		CHECK_FALSE(net::decode_debug(hello));
		CHECK_FALSE(net::decode_debug_report(hello));
		CHECK_FALSE(net::decode_hello(net::encode_bye()));
	}

	TEST_CASE("proto: every truncation and every extension of every message is refused") {
		for (const auto &[name, bytes] : sample_datagrams()) {
			INFO(name);
			REQUIRE(decode_reencode(bytes) == 1);
			for (size_t n = 0; n < bytes.size(); ++n) {
				const std::span<const uint8_t> prefix(bytes.data(), n);
				REQUIRE(decode_reencode(prefix) == -1);
			}
			for (const uint8_t extra : { uint8_t(0), uint8_t(0xFF) }) {
				std::vector<uint8_t> longer = bytes;
				longer.push_back(extra);
				REQUIRE(decode_reencode(longer) == -1);
			}
		}
	}

	TEST_CASE("proto: fuzzed datagrams never crash; whatever decodes re-encodes to the same message") {
		Rng rng(0xF022);
		const auto samples = sample_datagrams();
		size_t accepted = 0, refused = 0, unstable = 0;
		// Bit flips, byte overwrites, truncations and insertions of valid datagrams.
		for (int i = 0; i < 60000; ++i) {
			std::vector<uint8_t> b = samples[rng.below(uint32_t(samples.size()))].second;
			const uint32_t edits = 1 + rng.below(6);
			for (uint32_t e = 0; e < edits && !b.empty(); ++e) {
				switch (rng.below(4)) {
					case 0:
						b[rng.below(uint32_t(b.size()))] ^= uint8_t(1u << rng.below(8));
						break;
					case 1:
						b[rng.below(uint32_t(b.size()))] = uint8_t(rng.next());
						break;
					case 2:
						b.resize(rng.below(uint32_t(b.size()) + 1));
						break;
					default:
						b.insert(b.begin() + rng.below(uint32_t(b.size()) + 1), uint8_t(rng.next()));
						break;
				}
			}
			const int r = decode_reencode(b);
			(r == 1 ? accepted : r == 0 ? unstable : refused)++;
		}
		// Random bytes behind every type byte (and unknown ones), every length up to beyond a datagram.
		for (int i = 0; i < 40000; ++i) {
			std::vector<uint8_t> b(rng.below(1400));
			for (uint8_t &x : b) {
				x = uint8_t(rng.next());
			}
			if (!b.empty() && (rng.next() & 3) != 0) {
				static constexpr uint8_t kTypes[] = { 1, 2, 16, 17, 18, 19, 20, 21, 22, 30, 31 };
				b[0] = kTypes[rng.below(11)];
			}
			const int r = decode_reencode(b);
			(r == 1 ? accepted : r == 0 ? unstable : refused)++;
		}
		MESSAGE("fuzz: ", accepted, " accepted, ", refused, " refused, ", unstable, " unstable");
		CHECK(unstable == 0);
		CHECK(accepted > 0);
		CHECK(refused > 0);
	}

	TEST_CASE("proto: unsequenced datagrams over ENet loopback, both ways") {
		auto server = net::Host::create_server("127.0.0.1", 0, 4, net::kChannelCount);
		auto client = net::Host::create_client(net::kChannelCount);
		REQUIRE(server);
		REQUIRE(client);
		const int peer = client->connect("127.0.0.1", server->bound_port());
		REQUIRE(peer >= 0);
		int server_peer = -1;
		bool connected = false;
		const auto pump = [](net::Host &h, const std::function<void(net::Host::Event &)> &f) {
			net::Host::Event ev;
			while (h.service(0, ev) > 0) {
				f(ev);
			}
		};
		const uint64_t t0 = net::monotonic_ns();
		while ((!connected || server_peer < 0) && net::monotonic_ns() - t0 < 3'000'000'000ull) {
			pump(*server, [&](net::Host::Event &ev) {
				if (ev.type == net::Host::EventType::Connect) {
					server_peer = int(ev.peer);
				}
			});
			pump(*client, [&](net::Host::Event &ev) { connected |= ev.type == net::Host::EventType::Connect; });
		}
		REQUIRE(connected);
		REQUIRE(server_peer >= 0);
		const size_t limit = client->max_unfragmented_bytes(uint32_t(peer));
		MESSAGE("largest unfragmented ENet packet: ", limit, " bytes");
		CHECK(limit >= net::kMaxDatagramBytes);
		CHECK(client->max_unfragmented_bytes(99) == 0);

		Rng rng(41);
		net::CommandPacket cp;
		cp.count = 4;
		for (uint32_t i = 0; i < 4; ++i) {
			cp.commands[i] = sample_command(rng, 300 - i, 30 - i);
		}
		const net::Snapshot snap = sample_snapshot(rng, 55);
		const std::vector<uint8_t> cmd_bytes = net::encode_command_packet(cp);
		const std::vector<uint8_t> snap_bytes = net::encode_snapshot(snap);
		REQUIRE(snap_bytes.size() > 1100);
		constexpr int kCount = 20;
		for (int i = 0; i < kCount; ++i) {
			CHECK(client->send(uint32_t(peer), net::kChannelUnreliable, cmd_bytes.data(), cmd_bytes.size(),
					net::Host::SendMode::Unsequenced));
			CHECK(server->send(uint32_t(server_peer), net::kChannelUnreliable, snap_bytes.data(), snap_bytes.size(),
					net::Host::SendMode::Unsequenced));
		}
		// An unsequenced packet above one fragment is refused rather than sent as reliable fragments.
		const std::vector<uint8_t> oversized(limit + 1, 0x55);
		CHECK_FALSE(client->send(uint32_t(peer), net::kChannelUnreliable, oversized.data(), oversized.size(),
				net::Host::SendMode::Unsequenced));
		CHECK(client->send(uint32_t(peer), net::kChannelReliable, oversized.data(), oversized.size(), net::Host::SendMode::Reliable));
		client->flush();
		server->flush();
		int got_cmd = 0, got_snap = 0, got_big = 0;
		const uint64_t t1 = net::monotonic_ns();
		while ((got_cmd < kCount || got_snap < kCount || got_big < 1) && net::monotonic_ns() - t1 < 3'000'000'000ull) {
			pump(*server, [&](net::Host::Event &ev) {
				if (ev.type != net::Host::EventType::Receive) {
					return;
				}
				if (ev.channel == net::kChannelReliable && ev.data.size() == oversized.size()) {
					got_big++;
					return;
				}
				auto d = net::decode_command_packet(ev.data);
				if (d && same_packet(*d, cp) && ev.channel == net::kChannelUnreliable) {
					got_cmd++;
				}
			});
			pump(*client, [&](net::Host::Event &ev) {
				if (ev.type != net::Host::EventType::Receive) {
					return;
				}
				auto d = net::decode_snapshot(ev.data);
				if (d && same_snapshot(*d, snap) && ev.channel == net::kChannelUnreliable) {
					got_snap++;
				}
			});
		}
		CHECK(got_cmd == kCount);
		CHECK(got_snap == kCount);
		CHECK(got_big == 1);
		client->disconnect_all();
	}
}

// =================================================================================================================
namespace {

struct Delivery {
	uint32_t id = 0;
	uint32_t peer = 0;
	uint8_t channel = 0;
	bool reliable = false;
	uint64_t at_ns = 0;
};

std::vector<uint8_t> id_bytes(uint32_t p_id) {
	std::vector<uint8_t> b(4);
	std::memcpy(b.data(), &p_id, 4);
	return b;
}

uint32_t id_of(const std::vector<uint8_t> &p_bytes) {
	uint32_t id = 0;
	std::memcpy(&id, p_bytes.data(), 4);
	return id;
}

// Drives a NetSim with a submit schedule, polling at every release time, so every delivery is seen at its exact
// release instant. p_submit(i) fills the i-th datagram; returns the deliveries and the submit times by id.
struct Run {
	std::vector<Delivery> out;
	std::vector<uint64_t> submitted_at;
	net::NetSimStats stats;
};

Run drive(const net::NetSimConfig &p_config, uint32_t p_count, uint64_t p_interval_ns,
		const std::function<void(uint32_t, net::NetSim::Datagram &)> &p_fill) {
	net::NetSim sim(p_config);
	Run run;
	run.submitted_at.resize(p_count);
	uint64_t now = 1'000'000'000ull;
	uint32_t next = 0;
	uint64_t next_submit = now;
	for (;;) {
		const uint64_t release = sim.next_release_ns();
		if (next >= p_count && release == 0) {
			break;
		}
		if (next < p_count && (release == 0 || next_submit <= release)) {
			now = next_submit;
			net::NetSim::Datagram d;
			p_fill(next, d);
			d.bytes = id_bytes(next);
			run.submitted_at[next] = now;
			sim.submit(std::move(d), now);
			next++;
			next_submit += p_interval_ns;
		} else {
			now = std::max(now, release);
		}
		net::NetSim::Datagram got;
		while (sim.poll(now, got)) {
			run.out.push_back({ id_of(got.bytes), got.peer, got.channel, got.reliable, now });
		}
	}
	run.stats = sim.stats();
	return run;
}

constexpr uint64_t kTick60 = 16'666'667; // ns

} // namespace

TEST_SUITE("net-netsim") {
	TEST_CASE("netsim: profiles parse to the design's numbers") {
		auto p = net::parse_netsim("150/2");
		REQUIRE(p);
		CHECK(p->latency_min_ms == doctest::Approx(67.5f));
		CHECK(p->latency_max_ms == doctest::Approx(82.5f));
		CHECK(p->loss == doctest::Approx(0.02f));
		CHECK(p->spike_chance == 0.0f);
		CHECK(p->reorder == 0.0f);
		CHECK(p->duplicate == 0.0f);
		CHECK(p->enabled());
		auto stress = net::parse_netsim("lat=150-165,loss=2");
		REQUIRE(stress);
		CHECK(stress->latency_min_ms == 150.0f);
		CHECK(stress->latency_max_ms == 165.0f);
		CHECK(stress->loss == doctest::Approx(0.02f));
		for (const char *off : { "", "off", "  off  " }) {
			auto o = net::parse_netsim(off);
			REQUIRE(o);
			CHECK_FALSE(o->enabled());
		}
		auto bf3 = net::parse_netsim("bf3");
		REQUIRE(bf3);
		CHECK(bf3->latency_min_ms == 100.0f);
		CHECK(bf3->latency_max_ms == 200.0f);
		CHECK(bf3->loss == doctest::Approx(0.01f));
		CHECK(bf3->spike_chance == doctest::Approx(0.03f));
		CHECK(bf3->spike_min_ms == 300.0f);
		CHECK(bf3->spike_max_ms == 1000.0f);
		CHECK(bf3->reorder == doctest::Approx(0.01f));
		CHECK(bf3->duplicate == doctest::Approx(0.02f));
		auto keys = net::parse_netsim("lat=60-90,loss=2,spike=0,spike_ms=300-1000,reorder=1,dup=1,seed=7");
		REQUIRE(keys);
		CHECK(keys->latency_min_ms == 60.0f);
		CHECK(keys->latency_max_ms == 90.0f);
		CHECK(keys->reorder == doctest::Approx(0.01f));
		CHECK(keys->duplicate == doctest::Approx(0.01f));
		CHECK(keys->seed == 7);
		auto seeded = net::parse_netsim("150/2, seed=99");
		REQUIRE(seeded);
		CHECK(seeded->seed == 99);
		CHECK(seeded->latency_max_ms == doctest::Approx(82.5f));
		auto fixed = net::parse_netsim("lat=40");
		REQUIRE(fixed);
		CHECK(fixed->latency_min_ms == 40.0f);
		CHECK(fixed->latency_max_ms == 40.0f);
		for (const char *bad : { "lat=90-60", "loss=101", "foo=1", "150/", "/2", "abc", "lat=-5", "lat=5-", "seed=x",
					 "seed=-1", ",", "150/2,", "lat=nan", "lat=inf", "150/2,bf3", "lat=20000", "spike_ms=1-70000",
					 "loss=2,150/2", "lat==5", "50000/1" }) {
			INFO(bad);
			CHECK_FALSE(net::parse_netsim(bad));
		}
	}

	TEST_CASE("netsim: format_netsim round trips through parse_netsim") {
		for (const char *profile : { "150/2", "lat=150-165,loss=2", "bf3", "lat=60-90,loss=2,spike=5,spike_ms=300-1000,reorder=1,dup=1,seed=7" }) {
			auto a = net::parse_netsim(profile);
			REQUIRE(a);
			const std::string text = net::format_netsim(*a);
			INFO(profile, " -> ", text);
			auto b = net::parse_netsim(text);
			REQUIRE(b);
			CHECK(b->latency_min_ms == a->latency_min_ms);
			CHECK(b->latency_max_ms == a->latency_max_ms);
			CHECK(b->loss == doctest::Approx(a->loss));
			CHECK(b->spike_chance == doctest::Approx(a->spike_chance));
			CHECK(b->spike_min_ms == a->spike_min_ms);
			CHECK(b->spike_max_ms == a->spike_max_ms);
			CHECK(b->reorder == doctest::Approx(a->reorder));
			CHECK(b->duplicate == doctest::Approx(a->duplicate));
			CHECK(b->seed == a->seed);
		}
		CHECK(net::format_netsim({}) == "off");
		CHECK(net::format_netsim(*net::parse_netsim("150/2")) == "lat=67.5-82.5,loss=2,spike=0,spike_ms=300-1000,reorder=0,dup=0,seed=1");
	}

	TEST_CASE("netsim: a disabled simulator passes everything through at once, in order") {
		net::NetSim sim;
		CHECK(sim.next_release_ns() == 0);
		for (uint32_t i = 0; i < 100; ++i) {
			sim.submit({ i % 3, uint8_t(i % 2), (i % 2) != 0, id_bytes(i) }, 5000);
		}
		CHECK(sim.pending() == 100);
		CHECK(sim.next_release_ns() == 5000);
		net::NetSim::Datagram d;
		CHECK_FALSE(sim.poll(4999, d));
		for (uint32_t i = 0; i < 100; ++i) {
			REQUIRE(sim.poll(5000, d));
			CHECK(id_of(d.bytes) == i);
			CHECK(d.peer == i % 3);
			CHECK(d.channel == i % 2);
		}
		CHECK_FALSE(sim.poll(5000, d));
		CHECK(sim.stats().submitted == 100);
		CHECK(sim.stats().delivered == 100);
		CHECK(sim.stats().dropped == 0);
		CHECK(sim.next_release_ns() == 0);
		net::NetSim zero;
		zero.submit({ 0, 0, false, id_bytes(1) }, 0);
		CHECK(zero.next_release_ns() == 1); // pending at time 0 still reads as pending
	}

	TEST_CASE("netsim: \"150/2\" gives 2 % loss and a uniform 67.5-82.5 ms delay") {
		auto config = net::parse_netsim("150/2");
		REQUIRE(config);
		constexpr uint32_t kCount = 200000; // 55 minutes of 60 Hz commands
		const Run run = drive(*config, kCount, kTick60, [](uint32_t, net::NetSim::Datagram &d) { d.reliable = false; });
		const double loss = double(run.stats.dropped) / double(kCount);
		std::vector<double> delays;
		delays.reserve(run.out.size());
		for (const Delivery &d : run.out) {
			delays.push_back(double(d.at_ns - run.submitted_at[d.id]) * 1e-6);
		}
		std::sort(delays.begin(), delays.end());
		double sum = 0;
		for (const double x : delays) {
			sum += x;
		}
		const double mean = sum / double(delays.size());
		const auto q = [&](double p_f) { return delays[size_t(p_f * double(delays.size() - 1))]; };
		MESSAGE("150/2: loss ", loss * 100.0, " %, delay min ", delays.front(), " q25 ", q(0.25), " median ", q(0.5), " q75 ",
				q(0.75), " max ", delays.back(), " mean ", mean, " ms (stats mean ", run.stats.mean_delay_ms, ")");
		CHECK(run.stats.submitted == kCount);
		CHECK(run.stats.delivered + run.stats.dropped == kCount);
		CHECK(run.out.size() == run.stats.delivered);
		CHECK(loss == doctest::Approx(0.02).epsilon(0.1)); // 2 % +- 0.2 % (the binomial sigma is 0.03 %)
		CHECK(delays.front() >= 67.5 - 1e-6);
		CHECK(delays.back() <= 82.5 + 1e-6);
		CHECK(mean == doctest::Approx(75.0).epsilon(0.002));
		CHECK(q(0.25) == doctest::Approx(71.25).epsilon(0.003));
		CHECK(q(0.5) == doctest::Approx(75.0).epsilon(0.003));
		CHECK(q(0.75) == doctest::Approx(78.75).epsilon(0.003));
		CHECK(run.stats.mean_delay_ms == doctest::Approx(mean).epsilon(0.001));
		CHECK(run.stats.duplicated == 0);
		CHECK(run.stats.reordered == 0);
		CHECK(run.stats.spike_dropped == 0);
	}

	TEST_CASE("netsim: the same seed gives the same deliveries, another seed others") {
		auto config = net::parse_netsim("bf3");
		REQUIRE(config);
		const auto fill = [](uint32_t i, net::NetSim::Datagram &d) {
			d.reliable = i % 5 == 0;
			d.channel = d.reliable ? 1 : 0;
		};
		const Run a = drive(*config, 20000, kTick60, fill);
		const Run b = drive(*config, 20000, kTick60, fill);
		REQUIRE(a.out.size() == b.out.size());
		bool same = true;
		for (size_t i = 0; i < a.out.size(); ++i) {
			same = same && a.out[i].id == b.out[i].id && a.out[i].at_ns == b.out[i].at_ns;
		}
		CHECK(same);
		config->seed = 2;
		const Run c = drive(*config, 20000, kTick60, fill);
		bool differs = c.out.size() != a.out.size();
		for (size_t i = 0; !differs && i < a.out.size(); ++i) {
			differs = a.out[i].id != c.out[i].id || a.out[i].at_ns != c.out[i].at_ns;
		}
		CHECK(differs);
	}

	TEST_CASE("netsim: reliable datagrams are never dropped, duplicated or reordered") {
		auto config = net::parse_netsim("lat=50-250,loss=20,spike=30,spike_ms=200-800,reorder=20,dup=20,seed=5");
		REQUIRE(config);
		constexpr uint32_t kCount = 60000;
		const Run run = drive(*config, kCount, kTick60 / 2, [](uint32_t i, net::NetSim::Datagram &d) {
			d.peer = i % 3;
			d.reliable = (i % 4) != 0;
			d.channel = d.reliable ? uint8_t(1 + (i % 2)) : 0;
		});
		std::map<std::pair<uint32_t, uint8_t>, uint32_t> last; // (peer, channel) -> last reliable id
		std::vector<int> seen(kCount, 0);
		uint32_t reliable_in = 0, reliable_out = 0, inversions = 0;
		for (uint32_t i = 0; i < kCount; ++i) {
			reliable_in += (i % 4) != 0 ? 1 : 0;
		}
		uint32_t newest_unreliable = 0;
		for (const Delivery &d : run.out) {
			seen[d.id]++;
			if (d.reliable) {
				reliable_out++;
				const auto key = std::make_pair(d.peer, d.channel);
				auto it = last.find(key);
				REQUIRE((it == last.end() || it->second < d.id)); // in submit order per peer and channel
				last[key] = d.id;
				REQUIRE(seen[d.id] == 1);
				const double delay_ms = double(d.at_ns - run.submitted_at[d.id]) * 1e-6;
				REQUIRE(delay_ms >= 50.0 - 1e-6);
			} else {
				inversions += d.id < newest_unreliable ? 1 : 0;
				newest_unreliable = std::max(newest_unreliable, d.id);
			}
		}
		MESSAGE("lossy run: ", run.stats.dropped, " dropped (", run.stats.spike_dropped, " in spikes), ", run.stats.duplicated,
				" duplicated, ", run.stats.reordered, " held back, ", inversions, " unreliable deliveries out of order");
		CHECK(reliable_out == reliable_in);
		CHECK(run.stats.delivered == run.stats.submitted - run.stats.dropped + run.stats.duplicated);
		CHECK(run.stats.dropped > 0);
		CHECK(run.stats.spike_dropped > 0);
		CHECK(run.stats.duplicated > 0);
		CHECK(inversions > 0);
	}

	TEST_CASE("netsim: reorder holds a datagram back behind the next one") {
		auto config = net::parse_netsim("lat=20,reorder=10,seed=3");
		REQUIRE(config);
		constexpr uint32_t kCount = 50000;
		const Run run = drive(*config, kCount, kTick60, [](uint32_t, net::NetSim::Datagram &d) { d.reliable = false; });
		REQUIRE(run.out.size() == kCount);
		// With a fixed latency the only disorder is the hold: a held datagram comes right after the next datagram
		// that is not held itself, so it is overtaken only by the few datagrams right behind it.
		uint32_t swapped = 0;
		uint32_t worst_overtake = 0;
		uint32_t newest = 0;
		for (size_t i = 0; i < run.out.size(); ++i) {
			if (i + 1 < run.out.size() && run.out[i].id > run.out[i + 1].id) {
				swapped++;
			}
			newest = std::max(newest, run.out[i].id);
			worst_overtake = std::max(worst_overtake, newest - run.out[i].id);
		}
		const double rate = double(run.stats.reordered) / double(kCount);
		MESSAGE("reorder=10: ", run.stats.reordered, " held (", rate * 100.0, " %), ", swapped, " swapped pairs, overtaken by at most ",
				worst_overtake);
		CHECK(rate == doctest::Approx(0.10).epsilon(0.05));
		CHECK(worst_overtake >= 1);
		CHECK(worst_overtake <= 8);
		// A run of held datagrams goes behind one successor, so swaps can be fewer than holds, never more.
		CHECK(swapped <= run.stats.reordered);
		CHECK(swapped >= run.stats.reordered * 8 / 10);
		// A held datagram with no successor is released after its hold time instead of waiting forever.
		net::NetSim lone(*net::parse_netsim("lat=20,reorder=100"));
		lone.submit({ 0, 0, false, id_bytes(1) }, 1000);
		net::NetSim::Datagram d;
		CHECK_FALSE(lone.poll(1000 + 20'000'000, d));
		CHECK(lone.next_release_ns() == 1000 + 120'000'000);
		CHECK(lone.poll(1000 + 120'000'000, d));
	}

	TEST_CASE("netsim: duplication and drop spikes follow their knobs") {
		auto dup = net::parse_netsim("lat=10-30,dup=5,seed=8");
		REQUIRE(dup);
		const Run d = drive(*dup, 100000, kTick60, [](uint32_t, net::NetSim::Datagram &x) { x.reliable = false; });
		const double dup_rate = double(d.stats.duplicated) / 100000.0;
		CHECK(dup_rate == doctest::Approx(0.05).epsilon(0.06));
		CHECK(d.out.size() == 100000 + d.stats.duplicated);

		// 50 % chance per second to start a spike of exactly 300 ms (18 datagrams at 60 Hz).
		auto spike = net::parse_netsim("lat=10,spike=50,spike_ms=300,seed=4");
		REQUIRE(spike);
		constexpr uint32_t kCount = 60 * 1000; // 1,000 s
		const Run s = drive(*spike, kCount, kTick60, [](uint32_t i, net::NetSim::Datagram &x) {
			x.reliable = i % 10 == 0;
			x.channel = x.reliable ? 1 : 0;
		});
		std::vector<bool> got(kCount, false);
		for (const Delivery &x : s.out) {
			got[x.id] = true;
		}
		std::vector<uint32_t> runs;
		uint32_t current = 0, reliable_lost = 0;
		for (uint32_t i = 0; i < kCount; ++i) {
			if (i % 10 == 0) {
				reliable_lost += got[i] ? 0 : 1;
				continue; // reliable ones pass through spikes and do not break a run
			}
			if (!got[i]) {
				current++;
			} else if (current > 0) {
				runs.push_back(current);
				current = 0;
			}
		}
		std::sort(runs.begin(), runs.end());
		REQUIRE(runs.size() > 20);
		size_t up_to_one_spike = 0;
		for (const uint32_t r : runs) {
			up_to_one_spike += r <= 18 ? 1 : 0;
		}
		// Rate lambda = -ln(0.5) per second while no spike runs; 300 ms spikes -> about 17 % of the time. A spike
		// drops 18 datagrams at 60 Hz (16 or 17 unreliable ones when reliable ones fall inside); a new spike can
		// start on the tick one ends, so a few runs are two spikes back to back.
		const double fraction = double(s.stats.spike_dropped) / double(kCount * 9 / 10);
		MESSAGE("spike=50: ", runs.size(), " runs of losses, shortest ", runs.front(), ", median ", runs[runs.size() / 2],
				", longest ", runs.back(), " unreliable datagrams, ", fraction * 100.0, " % lost in spikes");
		CHECK(reliable_lost == 0);
		CHECK(s.stats.dropped == s.stats.spike_dropped);
		CHECK(fraction > 0.12);
		CHECK(fraction < 0.22);
		CHECK(runs.front() >= 16);
		CHECK(runs[runs.size() / 2] >= 16);
		CHECK(runs[runs.size() / 2] <= 18);
		CHECK(double(up_to_one_spike) >= 0.95 * double(runs.size()));
		CHECK(runs.size() > 300);
		CHECK(runs.size() < 800);
	}
}

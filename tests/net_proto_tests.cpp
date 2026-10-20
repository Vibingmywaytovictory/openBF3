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

int decode_reencode_v2(net::MessageType p_type, std::span<const uint8_t> p_bytes);
bool same_report_v2(const net::DebugReport &p_a, const net::DebugReport &p_b);
void append_v2_samples(std::vector<std::pair<std::string, std::vector<uint8_t>>> &r_out);

int decode_reencode_v3(net::MessageType p_type, std::span<const uint8_t> p_bytes);
void append_v3_samples(std::vector<std::pair<std::string, std::vector<uint8_t>>> &r_out);

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
			p_a.flags == p_b.flags && p_a == p_b;
}

bool same_snapshot(const net::Snapshot &p_a, const net::Snapshot &p_b) {
	if (p_a.seq != p_b.seq || p_a.server_tick != p_b.server_tick || p_a.applied_sequence != p_b.applied_sequence ||
			p_a.applied_repeated != p_b.applied_repeated || p_a.command_buffer_ticks != p_b.command_buffer_ticks ||
			p_a.sequence_echo != p_b.sequence_echo || p_a.echo_hold_100us != p_b.echo_hold_100us ||
			p_a.own_events != p_b.own_events || p_a.own != p_b.own || p_a.others.size() != p_b.others.size() ||
			p_a.own_weapons != p_b.own_weapons) {
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
	if (p_a.newest_snapshot != p_b.newest_snapshot || p_a.snapshot_ack_bits != p_b.snapshot_ack_bits || p_a.count != p_b.count ||
			p_a.others_ack_bits != p_b.others_ack_bits) {
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
			p_a.trace != p_b.trace || !same_report_v2(p_a, p_b)) {
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
							b->phase == a->phase && b->source_digest == a->source_digest && b->motor_hash == a->motor_hash &&
							b->match_seed == a->match_seed && b->weapon_hash == a->weapon_hash && b->weapon_flags == a->weapon_flags &&
							b->kit_hash == a->kit_hash && b->max_rewind_ticks == a->max_rewind_ticks
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
			return b && b->slot == a->slot && b->spawn_index == a->spawn_index && b->state == a->state && b->kit == a->kit &&
							b->team == a->team
					? 1
					: 0;
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
							b->index == a->index && b->ticks == a->ticks && b->slot == a->slot && b->pose == a->pose
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
		case net::MessageType::SnapshotOthers:
		case net::MessageType::ShotEvents:
		case net::MessageType::CombatEvents:
		case net::MessageType::KitSelect:
		case net::MessageType::MatchState:
		case net::MessageType::PlayerList:
		case net::MessageType::SpawnRequest:
			return decode_reencode_v2(*type, p_bytes);
	}
	return -1;
}

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
	append_v2_samples(out);
	append_v3_samples(out);
	return out;
}

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

}

TEST_SUITE("net-bits") {
	TEST_CASE("bits: least significant bit first into little-endian bytes") {
		net::BitWriter w(16);
		w.write_bits(1, 1);
		w.write_bits(2, 2);
		w.write_bits(0x1F, 5);
		CHECK(w.byte_count() == 1);
		CHECK(w.bytes()[0] == uint8_t(0b11111101));
		w.write_bits(0xABCD, 16);
		REQUIRE(w.byte_count() == 3);
		CHECK(w.bytes()[1] == 0xCD);
		CHECK(w.bytes()[2] == 0xAB);
		w.write_bits(0x3, 3);
		CHECK(w.bit_count() == 27);
		CHECK(w.byte_count() == 4);
		CHECK(w.bytes()[3] == 0x03);
		w.write_bits(0xFFFF, 0);
		CHECK(w.bit_count() == 27);
		w.write_bits(0xF0, 4);
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
			CHECK(w.bit_count() == 24 + 80);
		}

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
		{
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
		w2.write_bits(1, 65);
		CHECK(w2.overflowed());
		CHECK(w2.bit_count() == 0);
		net::BitWriter w3(1);
		w3.write_bits(1, 3);
		w3.write_bits(1, 6);
		CHECK(w3.overflowed());
		CHECK(w3.bit_count() == 3);
		net::BitWriter w4(0);
		w4.write_bool(false);
		CHECK(w4.overflowed());
		CHECK(w4.byte_count() == 0);

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
		CHECK(r2.read_bits(17) == 0);
		CHECK(r2.failed());
		CHECK(r2.read_bits(1) == 0);
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
		w.align_byte();
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
				REQUIRE(e <= q.step * 0.5f + std::fabs(v) * 6e-8f);
			}
			CHECK(q.encode(q.min - 100.0f) == 0);
			CHECK(q.encode(top + 100.0f) == q.max_code());
			CHECK(q.encode(std::numeric_limits<float>::quiet_NaN()) == 0);
			CHECK(q.encode(std::numeric_limits<float>::infinity()) == q.max_code());
			CHECK(q.encode(-std::numeric_limits<float>::infinity()) == 0);
			CHECK(q.decode(q.max_code() + 5) == top);

			uint32_t bad = 0;
			for (uint32_t c = 0; c <= q.max_code(); ++c) {
				bad += q.encode(q.decode(c)) != c ? 1 : 0;
			}
			CHECK(bad == 0);
		}
	}
}

TEST_SUITE("net-proto") {
	TEST_CASE("proto: peek_type knows every message and nothing else") {
		CHECK_FALSE(net::peek_type({}).has_value());
		int known = 0;
		for (int t = 0; t < 256; ++t) {
			const uint8_t b = uint8_t(t);
			known += net::peek_type(std::span<const uint8_t>(&b, 1)).has_value() ? 1 : 0;
		}
		CHECK(known == 18);
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
			CHECK(b.size() == 1 + 1 + 4 + 77 + 2 + 1);
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
			w.write_varint(1000);
		});
		CHECK_FALSE(net::decode_debug_report(rows_beyond));
	}

	TEST_CASE("proto: command packet sizes are 33 and 70 bytes (v2: 26 and 53 plus the others ack and view sample), with 4 commands of redundancy") {
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
		CHECK(same.size() == 33);
		auto ds = net::decode_command_packet(same);
		REQUIRE(ds);
		CHECK(same_packet(*ds, p));
		for (uint32_t i = 1; i < 4; ++i) {
			p.commands[i] = sample_command(rng, 10000 - i, 20000 - i);
			p.commands[i].yaw = uint16_t(p.commands[i - 1].yaw + 1);
		}
		const std::vector<uint8_t> differ = net::encode_command_packet(p);
		CHECK(differ.size() == 70);
		auto dd = net::decode_command_packet(differ);
		REQUIRE(dd);
		CHECK(same_packet(*dd, p));

		p.count = 1;
		CHECK(net::encode_command_packet(p).size() == 33);

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
		p.commands[2].tick = 400;
		auto d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		CHECK(d->count == 2);
		CHECK(d->commands[0] == p.commands[0]);
		CHECK(d->commands[1] == p.commands[1]);
		p.commands[2].tick = 498;
		p.commands[3].sequence = 1;
		d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		CHECK(d->count == 3);
		p.commands[0].tick = 0;
		d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		CHECK(d->count == 1);
		p.count = 0;
		CHECK(net::encode_command_packet(p).empty());
		p.count = 9;
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
				w.write_bits(0, 16 + 2 + 8);
				for (int i = 1; i < p_count; ++i) {
					w.write_bool(true);
				}
			});
		};
		CHECK(net::decode_command_packet(packet(1, 10, 10, 2, 8191, -127)));
		CHECK(net::decode_command_packet(packet(4, 10, 10, 0, -8191, 127)));
		CHECK_FALSE(net::decode_command_packet(packet(0, 10, 10, 0, 0, 0)));
		CHECK_FALSE(net::decode_command_packet(packet(5, 10, 10, 0, 0, 0)));
		CHECK_FALSE(net::decode_command_packet(packet(1, 10, 10, 3, 0, 0)));
		CHECK_FALSE(net::decode_command_packet(packet(1, 10, 10, 0, 8192, 0)));
		CHECK_FALSE(net::decode_command_packet(packet(1, 10, 10, 0, -8192, 0)));
		CHECK_FALSE(net::decode_command_packet(packet(1, 10, 10, 0, 0, -128)));
		CHECK_FALSE(net::decode_command_packet(packet(2, 0, 10, 0, 0, 0)));
		CHECK_FALSE(net::decode_command_packet(packet(2, 10, 0, 0, 0, 0)));

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
		CHECK(header == 97);

		s = sample_snapshot(rng, 49);
		size_t dropped = 99;
		const std::vector<uint8_t> b = net::encode_snapshot(s, &dropped);
		MESSAGE("snapshot with own state and 49 others: ", b.size(), " bytes");
		CHECK(b.size() <= net::kMaxDatagramBytes);
		CHECK(dropped == 0);
		auto d = net::decode_snapshot(b);
		REQUIRE(d);
		CHECK(same_snapshot(*d, s));
		s = sample_snapshot(rng, 127);
		const std::vector<uint8_t> big = net::encode_snapshot(s, &dropped);
		CHECK(big.size() <= net::kMaxDatagramBytes);
		CHECK(dropped == 127 - 49);
		auto db = net::decode_snapshot(big);
		REQUIRE(db);
		REQUIRE(db->others.size() == 49);
		for (size_t i = 0; i < 49; ++i) {
			CHECK(same_other(db->others[i], s.others[i]));
		}

		s.own.reset();
		auto dn = net::decode_snapshot(net::encode_snapshot(s, &dropped));
		REQUIRE(dn);
		CHECK_FALSE(dn->own.has_value());
		CHECK(dn->others.size() == 52);
		CHECK(dropped == 127 - 52);
	}

	TEST_CASE("proto: snapshot invariants and hostile snapshots") {
		Rng rng(22);
		net::Snapshot s = sample_snapshot(rng, 3);
		s.own->tick = s.server_tick - 1;
		auto d = net::decode_snapshot(net::encode_snapshot(s));
		CHECK_FALSE(d);
		s.own->tick = s.server_tick;
		s.others[2].slot = s.others[0].slot;
		CHECK_FALSE(net::decode_snapshot(net::encode_snapshot(s)));
		s.others[2].slot = 200;
		CHECK_FALSE(net::decode_snapshot(net::encode_snapshot(s)));
		s.others[2].slot = 127;
		CHECK(net::decode_snapshot(net::encode_snapshot(s)));

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
			REQUIRE(std::abs(int(q.pitch) - int(st.pitch)) <= 3);
			REQUIRE((q.pitch & 3) == 0);
			REQUIRE(q.generation == uint8_t(st.life & 0xFF));
			REQUIRE(q.flags == uint8_t(st.flags & 0xFF));

			net::Snapshot s;
			s.server_tick = 1;
			s.others.push_back(q);
			auto d = net::decode_snapshot(net::encode_snapshot(s));
			REQUIRE(d);
			REQUIRE(d->others.size() == 1);
			REQUIRE(same_other(d->others[0], q));
		}

		sim::SoldierState st;
		st.transition_total = 42;
		st.transition_ticks = 41;
		CHECK(net::quantize_other(0, st).transition == 6);
		st.transition_ticks = 21;
		CHECK(net::quantize_other(0, st).transition == 128);
		st.transition_ticks = 0;
		CHECK(net::quantize_other(0, st).transition == 255);
		st.transition_total = 0;
		CHECK(net::quantize_other(0, st).transition == 255);
		st.transition_total = 10;
		st.transition_ticks = 50;
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

		std::string name;
		for (int i = 0; i < 20; ++i) {
			name += "\xC3\xA9";
		}
		name = "x" + name;
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
		CHECK_FALSE(net::decode_hello(hello("abc", 4)));
		CHECK_FALSE(net::decode_hello(hello(std::string(33, 'a'), 33)));
		CHECK_FALSE(net::decode_hello(hello("abc", ~0ull)));
		CHECK_FALSE(net::decode_hello(hello("\xC3", 1)));
		CHECK_FALSE(net::decode_hello(hello("\xC0\xAF", 2)));
		CHECK_FALSE(net::decode_hello(hello("\xED\xA0\x80", 3)));
		CHECK_FALSE(net::decode_hello(hello("\xF4\x90\x80\x80", 4)));
		CHECK_FALSE(net::decode_hello(hello("\xFF", 1)));
		CHECK(net::decode_hello(hello("\xF0\x9F\x98\x80", 4)));
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
				w.write_bits(0, 8);
				w.write_bits(1, 8);
				w.write_bool(false);
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
				w.write_bits(0, 8);
				w.write_bits(0, 8);
			});
		};
		CHECK(net::decode_debug(debug(1, 10.0f, 60)));
		CHECK_FALSE(net::decode_debug(debug(0, 10.0f, 60)));
		CHECK_FALSE(net::decode_debug(debug(8, 10.0f, 60)));
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
				for (const unsigned bits : { 64u, 64u, 8u, 64u, 8u, 64u, 8u }) {
					w.write_bits(0, bits);
				}
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
		const net::Snapshot snap = sample_snapshot(rng, 49);
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

constexpr uint64_t kTick60 = 16'666'667;

}

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
		CHECK(zero.next_release_ns() == 1);
	}

	TEST_CASE("netsim: \"150/2\" gives 2 % loss and a uniform 67.5-82.5 ms delay") {
		auto config = net::parse_netsim("150/2");
		REQUIRE(config);
		constexpr uint32_t kCount = 200000;
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
		CHECK(loss == doctest::Approx(0.02).epsilon(0.1));
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
		std::map<std::pair<uint32_t, uint8_t>, uint32_t> last;
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
				REQUIRE((it == last.end() || it->second < d.id));
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

		CHECK(swapped <= run.stats.reordered);
		CHECK(swapped >= run.stats.reordered * 8 / 10);

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

		auto spike = net::parse_netsim("lat=10,spike=50,spike_ms=300,seed=4");
		REQUIRE(spike);
		constexpr uint32_t kCount = 60 * 1000;
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
				continue;
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

namespace {

namespace sw = bf3::sim::weapons;

const net::ShotQuantization kFrame{ { -2048.0f, -512.0f, -2048.0f } };

sim::Vec3 unit_vector(Rng &r_rng) {
	for (;;) {
		const float x = r_rng.uniform(-1, 1), y = r_rng.uniform(-1, 1), z = r_rng.uniform(-1, 1);
		const float n = std::sqrt(x * x + y * y + z * z);
		if (n > 0.1f && n <= 1.0f) {
			return { x / n, y / n, z / n };
		}
	}
}

double angle_between(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const double d = double(p_a[0]) * p_b[0] + double(p_a[1]) * p_b[1] + double(p_a[2]) * p_b[2];
	const double na = std::sqrt(double(p_a[0]) * p_a[0] + double(p_a[1]) * p_a[1] + double(p_a[2]) * p_a[2]);
	const double nb = std::sqrt(double(p_b[0]) * p_b[0] + double(p_b[1]) * p_b[1] + double(p_b[2]) * p_b[2]);
	return std::acos(std::clamp(d / (na * nb), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
}

double max_axis_error(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return std::max({ std::fabs(double(p_a[0]) - p_b[0]), std::fabs(double(p_a[1]) - p_b[1]), std::fabs(double(p_a[2]) - p_b[2]) });
}

net::OtherSoldier sample_other_v2(Rng &r_rng, uint8_t p_slot, sim::Tick p_tick) {
	net::OtherSoldier o = net::quantize_other(p_slot, sample_state(r_rng, p_tick));
	net::set_other_weapon(o, uint16_t(r_rng.below(300)), sw::WeaponPhase(r_rng.below(8)), (r_rng.next() & 1) != 0, (r_rng.next() & 1) != 0,
			uint32_t(r_rng.next()), uint32_t(r_rng.next()));
	return o;
}

int32_t sample_wire_time(Rng &r_rng) {
	if (r_rng.below(5) == 0) {
		return net::kWeaponsWireTimeZero;
	}
	return int32_t(int64_t(r_rng.next() % uint64_t(2 * int64_t(net::kWeaponsWireMaxOffset) + 1)) - int64_t(net::kWeaponsWireMaxOffset));
}

net::WeaponsWire sample_weapons_wire(Rng &r_rng) {
	net::WeaponsWire w;
	w.next_shot_index = uint32_t(r_rng.next());
	w.active = uint8_t(r_rng.below(sw::kMaxWeaponSlots));
	w.pending = (r_rng.next() & 1) != 0 ? uint8_t(r_rng.below(sw::kMaxWeaponSlots)) : uint8_t(0xFF);
	if (w.pending == w.active) {
		w.pending = 0xFF;
	}
	w.events = uint32_t(r_rng.below(1u << 16));
	w.sprint_recover_until = sample_wire_time(r_rng);
	for (uint8_t i = 0; i < sw::kMaxWeaponSlots; ++i) {
		net::WeaponSlotWire &s = w.slots[i];
		s.mode_counter = uint8_t(r_rng.below(4));
		if (i != w.active && i != w.pending && r_rng.below(3) == 0) {
			continue;
		}
		s.def = uint16_t(r_rng.below(300));
		s.mode = uint8_t(r_rng.below(4));
		s.magazine = int32_t(r_rng.below(200)) - 5;
		s.reserve = int32_t(uint32_t(r_rng.next()));
		net::WeaponTimersWire &t = i == w.active ? w.timers[0] : i == w.pending ? w.timers[1] : w.idle[i];
		t.slot = i;
		t.phase = sw::WeaponPhase(r_rng.below(8));
		t.burst_left = uint8_t(r_rng.below(64));
		t.flags = uint8_t(r_rng.below(128));
		t.phase_start = sample_wire_time(r_rng);
		t.phase_end = sample_wire_time(r_rng);
		t.next_shot_time = sample_wire_time(r_rng);
		t.spread_time = sample_wire_time(r_rng);
		t.hold_start = sample_wire_time(r_rng);
		t.spread = r_rng.uniform(0, 8);
		if (r_rng.below(3) != 0) {
			t.recoil = { r_rng.uniform(-3, 3), r_rng.uniform(-3, 3) };
		}
	}
	return w;
}

net::RemoteShot sample_shot(Rng &r_rng, uint8_t p_slot, uint32_t p_index, sim::Tick p_tick) {
	net::RemoteShot s;
	s.slot = p_slot;
	s.shot_index = p_index;
	s.def = uint16_t(r_rng.below(net::kMaxWireDefs));
	s.time = sw::tick_start(p_tick - 1) + int64_t(r_rng.next() % uint64_t(2 * sw::kTimeUnitsPerTick));
	s.origin = { r_rng.uniform(-300, 300), r_rng.uniform(-50, 120), r_rng.uniform(-300, 300) };
	s.direction = unit_vector(r_rng);
	s.pellets = r_rng.below(4) == 0 ? uint8_t(1 + r_rng.below(uint32_t(sw::kMaxPellets))) : uint8_t(1);
	s.tracer = r_rng.below(3) == 0;
	if (s.tracer) {
		s.aim = unit_vector(r_rng);
	}
	if (s.pellets > 1) {
		s.pellet_cone = r_rng.uniform(0, sw::kMaxAngle);
	}
	if (r_rng.below(4) == 0) {
		s.power = r_rng.uniform(0, sw::kMaxPower);
	}
	return s;
}

bool same_shot(const net::RemoteShot &p_a, const net::RemoteShot &p_b) {
	return p_a.slot == p_b.slot && p_a.shot_index == p_b.shot_index && p_a.def == p_b.def && p_a.time == p_b.time && p_a.origin == p_b.origin &&
			p_a.direction == p_b.direction && p_a.pellets == p_b.pellets && p_a.tracer == p_b.tracer && p_a.aim == p_b.aim &&
			p_a.pellet_cone == p_b.pellet_cone && p_a.power == p_b.power;
}

net::CombatEvents sample_combat(Rng &r_rng, sim::Tick p_tick, size_t p_hits, size_t p_deaths, size_t p_detonations) {
	net::CombatEvents m;
	m.server_tick = p_tick;
	const auto time = [&] { return sw::tick_start(p_tick - r_rng.below(40)) + int64_t(r_rng.next() % uint64_t(sw::kTimeUnitsPerTick)); };
	for (size_t i = 0; i < p_hits; ++i) {
		net::HitEvent h;
		h.victim = uint8_t(r_rng.below(128));
		h.shooter = uint8_t(r_rng.below(128));
		h.shot_index = uint32_t(r_rng.next());
		h.pellet = uint8_t(r_rng.below(64));
		h.zone = sw::HitZone(r_rng.below(4));
		h.damage = r_rng.uniform(0, 300);
		h.health_after = r_rng.uniform(0, 100);
		h.position = { r_rng.uniform(-300, 300), r_rng.uniform(-50, 120), r_rng.uniform(-300, 300) };
		h.normal = unit_vector(r_rng);
		h.time = time();
		m.hits.push_back(h);
	}
	for (size_t i = 0; i < p_deaths; ++i) {
		net::DeathEvent d;
		d.victim = uint8_t(r_rng.below(128));
		d.killer = uint8_t(r_rng.below(128));
		d.def = r_rng.below(5) == 0 ? sw::kNoWeapon : uint16_t(r_rng.below(net::kMaxWireDefs));
		d.zone = sw::HitZone(r_rng.below(4));
		d.shot_index = uint32_t(r_rng.next());
		d.headshot = (r_rng.next() & 1) != 0;
		d.time = time();
		m.deaths.push_back(d);
	}
	for (size_t i = 0; i < p_detonations; ++i) {
		net::DetonationWire d;
		d.shooter = uint8_t(r_rng.below(128));
		d.shot_index = uint32_t(r_rng.next());
		d.def = uint16_t(r_rng.below(net::kMaxWireDefs));
		d.time = time();
		d.position = { r_rng.uniform(-300, 300), r_rng.uniform(-50, 120), r_rng.uniform(-300, 300) };
		d.normal = unit_vector(r_rng);
		d.dud = (r_rng.next() & 1) != 0;
		m.detonations.push_back(d);
	}
	return m;
}

template <class M, class Enc>
bool same_encoding(const M &p_a, const M &p_b, Enc &&p_encode) {
	return p_encode(p_a) == p_encode(p_b);
}

net::DebugReport sample_gunplay_report(Rng &r_rng) {
	net::DebugReport rep;
	rep.id = 41;
	rep.ok = true;
	rep.text = "target info";
	net::TargetInfoRow row;
	row.slot = 9;
	row.pose = sim::Pose::Crouch;
	row.feet = { 1.0f, 60.0f, -3.0f };
	row.yaw = 12345;
	row.target.id = 9;
	row.target.team = 2;
	row.target.bound_center = { 1.0f, 61.0f, -3.0f };
	row.target.bound_radius = 1.25f;
	row.target.capsule_count = 3;
	for (uint8_t i = 0; i < 3; ++i) {
		row.target.capsules[i] = { { 1.0f, 60.2f + i, -3.0f }, { 1.0f, 60.5f + i, -3.0f }, 0.15f, sw::HitZone(i), i, uint8_t(10 + i), 1 };
	}
	rep.targets.push_back(row);
	for (sim::Tick t = 0; t < 3; ++t) {
		rep.trace.push_back(sample_state(r_rng, 9000 + t));
		net::TraceGunplayTick g;
		g.tick = 9000 + t;
		sw::ShotEvent s;
		s.time = sw::tick_start(g.tick) + 1234;
		s.spawn_time = s.time + 5;
		s.seed = r_rng.next();
		s.player_slot = 4;
		s.shot_index = 70 + t;
		s.def = 3;
		s.weapon_slot = 1;
		s.pellets = 1;
		s.origin = { 1, 2, 3 };
		s.aim = { 0, 0, 1 };
		s.direction = { 0, 0.01f, 0.99995f };
		s.spread = 0.3f;
		s.recoil = { 0.1f, -0.05f };
		g.shots.push_back(s);
		g.projectiles.push_back({ 1, 2, 30 });
		g.projectile_shots.push_back(70 + t);
		sw::DamageEvent d;
		d.time = s.time + 99;
		d.attacker_slot = 4;
		d.target = 9;
		d.shot_index = 70 + t;
		d.def = 3;
		d.amount = 25;
		g.damage.push_back(d);
		if (t == 2) {
			sw::DetonationEvent det;
			det.time = s.time;
			det.position = { 5, 6, 7 };
			det.normal = { 0, 1, 0 };
			det.player_slot = 4;
			det.def = 3;
			g.detonations.push_back(det);
			g.deaths.push_back({ 9, 4, 72 });
		}
		rep.trace_gunplay.push_back(g);
	}
	rep.trace_dropped = 2;
	return rep;
}

}

namespace {

bool same_report_v2(const net::DebugReport &p_a, const net::DebugReport &p_b) {

	net::DebugReport a, b;
	a.targets = p_a.targets;
	a.trace_gunplay = p_a.trace_gunplay;
	a.trace_dropped = p_a.trace_dropped;
	b.targets = p_b.targets;
	b.trace_gunplay = p_b.trace_gunplay;
	b.trace_dropped = p_b.trace_dropped;
	return net::encode_debug_report(a) == net::encode_debug_report(b);
}

int decode_reencode_v2(net::MessageType p_type, std::span<const uint8_t> p_bytes) {
	switch (p_type) {
		case net::MessageType::SnapshotOthers: {
			auto a = net::decode_snapshot_others(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_snapshot_others(net::encode_snapshot_others(*a));
			return b && b->seq == a->seq && b->server_tick == a->server_tick && b->others == a->others ? 1 : 0;
		}
		case net::MessageType::ShotEvents: {
			auto a = net::decode_shot_events(p_bytes, kFrame);
			if (!a) {
				return -1;
			}
			const sim::Vec3 eye{};
			size_t dropped = 0;
			auto b = net::decode_shot_events(net::encode_shot_events(*a, kFrame, eye, &dropped), kFrame);
			const auto enc = [&](const net::ShotEvents &p_m) { return net::encode_shot_events(p_m, kFrame, eye); };
			return b && dropped == 0 && b->shots.size() == a->shots.size() && same_encoding(*a, *b, enc) ? 1 : 0;
		}
		case net::MessageType::CombatEvents: {
			auto a = net::decode_combat_events(p_bytes, kFrame);
			if (!a) {
				return -1;
			}
			size_t clamped = 0;
			auto b = net::decode_combat_events(net::encode_combat_events(*a, kFrame, &clamped), kFrame);
			const auto enc = [&](const net::CombatEvents &p_m) { return net::encode_combat_events(p_m, kFrame); };
			return b && clamped == 0 && same_encoding(*a, *b, enc) ? 1 : 0;
		}
		case net::MessageType::KitSelect: {
			auto a = net::decode_kit_select(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_kit_select(net::encode_kit_select(*a));
			return b && b->kit == a->kit && b->team == a->team ? 1 : 0;
		}
		default:
			return decode_reencode_v3(p_type, p_bytes);
	}
}

void append_v2_samples(std::vector<std::pair<std::string, std::vector<uint8_t>>> &r_out) {
	Rng rng(0x2002);
	net::Snapshot s = sample_snapshot(rng, 6);
	s.own_weapons = sample_weapons_wire(rng);
	for (net::OtherSoldier &o : s.others) {
		net::set_other_weapon(o, uint16_t(rng.below(254)), sw::WeaponPhase(rng.below(8)), true, false, uint32_t(rng.next()), uint32_t(rng.next()));
	}
	r_out.emplace_back("Snapshot (v2 own weapons)", net::encode_snapshot(s));
	net::SnapshotOthers so;
	so.seq = 4242;
	so.server_tick = 77777;
	for (uint8_t i = 0; i < 5; ++i) {
		so.others.push_back(sample_other_v2(rng, uint8_t(60 + i), so.server_tick));
	}
	r_out.emplace_back("SnapshotOthers", net::encode_snapshot_others(so));
	net::ShotEvents se;
	se.server_tick = 77777;
	for (uint32_t i = 0; i < 9; ++i) {
		se.shots.push_back(sample_shot(rng, uint8_t(1 + i % 3), 500 + i * (i == 7 ? 40 : 1), se.server_tick));
	}
	se.shots[0].pellets = 12;
	se.shots[0].pellet_cone = 2.5f;
	se.shots[1].tracer = true;
	se.shots[1].aim = { 0, 0, 1 };
	se.shots[2].power = 0.5f;
	r_out.emplace_back("ShotEvents", net::encode_shot_events(se, kFrame, {}));
	r_out.emplace_back("CombatEvents", net::encode_combat_events(sample_combat(rng, 77777, 3, 2, 1), kFrame));
	r_out.emplace_back("KitSelect", net::encode_kit_select({ 3, 2 }));
	net::DebugRequest dr;
	dr.kind = net::DebugKind::SpawnTarget;
	dr.id = 5;
	dr.slot = 17;
	dr.pose = sim::Pose::Prone;
	r_out.emplace_back("Debug (SpawnTarget)", net::encode_debug(dr));
	r_out.emplace_back("DebugReport (v2)", net::encode_debug_report(sample_gunplay_report(rng)));
	net::MatchInfo mi;
	mi.level = "MP_Subway";
	mi.mode = "TeamDeathMatch0";
	mi.match_seed = 0x1234;
	mi.weapon_hash = 0x5678;
	mi.weapon_flags = net::weapon_flags::kExactAim;
	mi.kit_hash = 0x9ABC;
	mi.max_rewind_ticks = 21;
	r_out.emplace_back("MatchInfo (v2)", net::encode_match_info(mi));
	net::Spawned sp{ 3, 17, sample_state(rng, 4000) };
	sp.kit = 2;
	sp.team = 2;
	r_out.emplace_back("Spawned (v2)", net::encode_spawned(sp));
}

}

TEST_SUITE("net-proto") {
	TEST_CASE("proto v2: version, message types and the bounds") {
		CHECK(net::kProtocolVersion == 3);
		for (const uint8_t t : { uint8_t(3), uint8_t(4), uint8_t(23), uint8_t(24) }) {
			CHECK(net::peek_type(std::span<const uint8_t>(&t, 1)).has_value());
		}
		CHECK(net::Hello{}.protocol == 3);
		CHECK(net::MatchInfo{}.protocol == 3);
	}

	TEST_CASE("proto v2: MatchInfo, Spawned, KitSelect and the new Debug kinds round trip; out-of-range values are refused") {
		Rng rng(21);
		net::MatchInfo m;
		m.level = "MP_Subway";
		m.mode = "TeamDeathMatch0";
		m.match_seed = 0xFEDCBA9876543210ull;
		m.weapon_hash = 0x0123456789ABCDEFull;
		m.weapon_flags = net::weapon_flags::kExactAim | net::weapon_flags::kInfiniteMagazine;
		m.kit_hash = 0xA5A5A5A55A5A5A5Aull;
		m.max_rewind_ticks = 21;
		auto dm = net::decode_match_info(net::encode_match_info(m));
		REQUIRE(dm);
		CHECK(dm->match_seed == m.match_seed);
		CHECK(dm->weapon_hash == m.weapon_hash);
		CHECK(dm->weapon_flags == m.weapon_flags);
		CHECK(dm->kit_hash == m.kit_hash);
		CHECK(dm->max_rewind_ticks == 21);
		for (int bit = 2; bit < 8; ++bit) {
			m.weapon_flags = uint8_t(1u << bit);
			CHECK_FALSE(net::decode_match_info(net::encode_match_info(m)));
		}
		net::Spawned s{ 5, 101, sample_state(rng, 4242) };
		for (uint8_t kit = 0; kit < 6; ++kit) {
			for (uint8_t team = 0; team < 4; ++team) {
				s.kit = kit;
				s.team = team;
				const std::vector<uint8_t> b = net::encode_spawned(s);
				CHECK(b.size() == 1 + 1 + 4 + 77 + 2 + 1);
				auto d = net::decode_spawned(b);
				CHECK(d.ok() == (kit < net::kMaxKits && team <= net::kMaxTeam));
				if (d) {
					CHECK(d->kit == kit);
					CHECK(d->team == team);
					CHECK(d->state == s.state);
				}
				const std::vector<uint8_t> k = net::encode_kit_select({ kit, team });
				CHECK(k.size() == 3);
				auto dk = net::decode_kit_select(k);
				CHECK(dk.ok() == (kit < net::kMaxKits && team <= net::kMaxTeam));
				if (dk) {
					CHECK(dk->kit == kit);
					CHECK(dk->team == team);
				}
			}
		}
		CHECK_FALSE(net::decode_kit_select(std::vector<uint8_t>{ uint8_t(net::MessageType::KitSelect), 1 }));
		CHECK_FALSE(net::decode_kit_select(std::vector<uint8_t>{ uint8_t(net::MessageType::KitSelect), 1, 1, 0 }));
		for (const net::DebugKind kind : { net::DebugKind::SpawnTarget, net::DebugKind::TargetInfo }) {
			for (const sim::Pose pose : { sim::Pose::Stand, sim::Pose::Crouch, sim::Pose::Prone }) {
				net::DebugRequest r;
				r.kind = kind;
				r.id = 3;
				r.slot = 127;
				r.pose = pose;
				r.position = { 4, 5, 6 };
				auto d = net::decode_debug(net::encode_debug(r));
				REQUIRE(d);
				CHECK(d->kind == kind);
				CHECK(d->slot == 127);
				CHECK(d->pose == pose);
			}
		}
		net::DebugRequest bad;
		bad.kind = net::DebugKind::TargetInfo;
		bad.slot = 128;
		CHECK_FALSE(net::decode_debug(net::encode_debug(bad)));
		bad.slot = 0;
		bad.pose = sim::Pose(3);
		CHECK_FALSE(net::decode_debug(net::encode_debug(bad)));
		bad.pose = sim::Pose::Stand;
		bad.kind = net::DebugKind(8);
		CHECK_FALSE(net::decode_debug(net::encode_debug(bad)));
	}

	TEST_CASE("proto v2: a DebugReport carries TargetInfo rows and the gunplay trace; the per-tick bounds cut and count") {
		Rng rng(22);
		net::DebugReport rep = sample_gunplay_report(rng);
		const std::vector<uint8_t> b = net::encode_debug_report(rep);
		MESSAGE("DebugReport with 1 target row and 3 gunplay ticks: ", b.size(), " bytes");
		auto d = net::decode_debug_report(b);
		REQUIRE(d);
		CHECK(same_report(*d, rep));
		REQUIRE(d->targets.size() == 1);
		CHECK(d->targets[0].target.capsule_count == 3);
		CHECK(d->targets[0].target.capsules[2].property == 12);
		REQUIRE(d->trace_gunplay.size() == 3);
		CHECK(d->trace_gunplay[2].deaths.size() == 1);
		CHECK(d->trace_gunplay[2].detonations.size() == 1);
		CHECK(d->trace_gunplay[1].shots[0].shot_index == 71);
		CHECK(d->trace_gunplay[1].shots[0].seed == rep.trace_gunplay[1].shots[0].seed);
		CHECK(d->trace_dropped == 2);

		net::TraceGunplayTick &g = rep.trace_gunplay[0];
		const sw::ShotEvent first_shot = g.shots[0];
		while (g.shots.size() < net::kMaxTraceShots + 3) {
			g.shots.push_back(first_shot);
		}
		while (g.deaths.size() < net::kMaxTraceDeaths + 1) {
			g.deaths.push_back({ 1, 2, 3 });
		}
		auto cut = net::decode_debug_report(net::encode_debug_report(rep));
		REQUIRE(cut);
		CHECK(cut->trace_gunplay[0].shots.size() == net::kMaxTraceShots);
		CHECK(cut->trace_gunplay[0].deaths.size() == net::kMaxTraceDeaths);
		CHECK(cut->trace_dropped == 2 + 3 + 1);

		net::DebugReport full;
		const net::TraceGunplayTick &one = rep.trace_gunplay[2];
		net::TraceGunplayTick max_tick;
		max_tick.tick = one.tick;
		max_tick.shots.assign(net::kMaxTraceShots, one.shots[0]);
		max_tick.projectiles.assign(net::kMaxTraceProjectiles, one.projectiles[0]);
		max_tick.projectile_shots.assign(net::kMaxTraceProjectiles, 1);
		max_tick.damage.assign(net::kMaxTraceDamage, one.damage[0]);
		max_tick.detonations.assign(net::kMaxTraceDetonations, one.detonations[0]);
		max_tick.deaths.assign(net::kMaxTraceDeaths, one.deaths[0]);
		for (uint32_t i = 0; i < net::kMaxTraceTicks; ++i) {
			full.trace.push_back(sample_state(rng, 100 + i));
			full.trace_gunplay.push_back(max_tick);
		}
		for (uint32_t i = 0; i < net::kMaxTargetRows; ++i) {
			full.targets.push_back(rep.targets[0]);
		}
		const std::vector<uint8_t> fb = net::encode_debug_report(full);
		MESSAGE("DebugReport with every bound full: ", fb.size(), " bytes (cap ", net::kMaxDebugReportBytes, ")");
		REQUIRE_FALSE(fb.empty());
		CHECK(fb.size() <= net::kMaxDebugReportBytes);
		auto df = net::decode_debug_report(fb);
		REQUIRE(df);
		CHECK(df->trace_gunplay.size() == net::kMaxTraceTicks);
		CHECK(df->targets.size() == net::kMaxTargetRows);

		net::DebugReport bad_slot = rep;
		bad_slot.targets[0].slot = 200;
		CHECK_FALSE(net::decode_debug_report(net::encode_debug_report(bad_slot)));
		const auto huge = craft(net::MessageType::DebugReport, [&](net::BitWriter &w) {
			w.write_bits(1, 32);
			w.write_bool(true);
			w.write_varint(0);
			w.write_varint(0);
			w.write_varint(0);
			w.write_varint(0);
			w.write_varint(net::kMaxTraceTicks);
		});
		CHECK_FALSE(net::decode_debug_report(huge));
	}

	TEST_CASE("proto v2: the command packet carries the view sample and others_ack_bits (33 / 70 bytes)") {
		Rng rng(23);
		net::CommandPacket p;
		p.newest_snapshot = 65535;
		p.snapshot_ack_bits = 0x80000001u;
		p.others_ack_bits = 0xDEADBEEFu;
		p.count = 4;
		for (uint32_t i = 0; i < 4; ++i) {
			p.commands[i] = sample_command(rng, 1000 - i, 2000 - i);
			p.commands[i].view_back_q8 = uint16_t(rng.next());
			p.commands[i].view_quality = uint8_t(rng.below(4));
			p.commands[i].view_seq8 = uint8_t(rng.next());
		}
		const std::vector<uint8_t> differ = net::encode_command_packet(p);
		MESSAGE("v2 command packet, 4 different commands: ", differ.size(), " bytes");
		CHECK(differ.size() == 70);
		auto d = net::decode_command_packet(differ);
		REQUIRE(d);
		CHECK(same_packet(*d, p));
		CHECK(d->others_ack_bits == 0xDEADBEEFu);
		CHECK(d->commands[3].view_back_q8 == p.commands[3].view_back_q8);
		for (uint32_t i = 1; i < 4; ++i) {
			const sim::Command c = p.commands[0];
			p.commands[i] = c;
			p.commands[i].tick = c.tick - i;
			p.commands[i].sequence = c.sequence - i;
		}
		const std::vector<uint8_t> same = net::encode_command_packet(p);
		MESSAGE("v2 command packet, 4 equal commands: ", same.size(), " bytes");
		CHECK(same.size() == 33);

		p.commands[2].view_seq8 = uint8_t(p.commands[2].view_seq8 + 1);
		CHECK(net::encode_command_packet(p).size() == 58);
		auto d2 = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d2);
		CHECK(same_packet(*d2, p));
		p.count = 1;
		CHECK(net::encode_command_packet(p).size() == 33);
	}

	TEST_CASE("proto v2: the ack predicates follow §7.2 (snapshot bit i = newest - 1 - i, others bit i = newest - i)") {
		net::CommandPacket p;
		p.newest_snapshot = 100;
		p.snapshot_ack_bits = 0b101u;
		p.others_ack_bits = 0b110u;
		CHECK(net::snapshot_acked(p, 100));
		CHECK(net::snapshot_acked(p, 99));
		CHECK_FALSE(net::snapshot_acked(p, 98));
		CHECK(net::snapshot_acked(p, 97));
		CHECK_FALSE(net::others_acked(p, 100));
		CHECK(net::others_acked(p, 99));
		CHECK(net::others_acked(p, 98));
		CHECK_FALSE(net::others_acked(p, 97));
		CHECK_FALSE(net::snapshot_acked(p, 101));
		CHECK_FALSE(net::others_acked(p, 101));
		p.snapshot_ack_bits = ~0u;
		p.others_ack_bits = ~0u;
		CHECK(net::snapshot_acked(p, uint16_t(100 - 32)));
		CHECK_FALSE(net::snapshot_acked(p, uint16_t(100 - 33)));
		CHECK(net::others_acked(p, uint16_t(100 - 31)));
		CHECK_FALSE(net::others_acked(p, uint16_t(100 - 32)));

		p.newest_snapshot = 1;
		p.snapshot_ack_bits = 0b11u;
		p.others_ack_bits = 0b1001u;
		CHECK(net::snapshot_acked(p, 0));
		CHECK(net::snapshot_acked(p, 65535));
		CHECK_FALSE(net::snapshot_acked(p, 65534));
		CHECK(net::others_acked(p, 1));
		CHECK_FALSE(net::others_acked(p, 0));
		CHECK(net::others_acked(p, 65534));
		CHECK_FALSE(net::others_acked(p, 2));

		p.count = 1;
		p.commands[0].tick = 9;
		auto d = net::decode_command_packet(net::encode_command_packet(p));
		REQUIRE(d);
		for (uint32_t s = 0; s < 65536; s += 1) {
			REQUIRE(net::snapshot_acked(*d, uint16_t(s)) == net::snapshot_acked(p, uint16_t(s)));
			REQUIRE(net::others_acked(*d, uint16_t(s)) == net::others_acked(p, uint16_t(s)));
		}
	}

	TEST_CASE("proto v2: snapshots carry the own weapons and the others' weapon fields; 64 and 128 players split into Snapshot + SnapshotOthers") {
		Rng rng(24);
		net::Snapshot s = sample_snapshot(rng, 0);
		size_t dropped = 0;
		const size_t header = net::encode_snapshot(s).size();
		s.own_weapons = sample_weapons_wire(rng);
		const size_t with_weapons = net::encode_snapshot(s).size();
		MESSAGE("snapshot header + own state: ", header, " bytes; + a random 8-slot weapons wire: ", with_weapons, " bytes");
		CHECK(header == 97);

		net::WeaponsWire kit;
		kit.next_shot_index = 1234;
		kit.active = 0;
		for (uint8_t i : { uint8_t(0), uint8_t(1), uint8_t(6), uint8_t(7) }) {
			kit.slots[i].def = uint16_t(10 + i);
			kit.slots[i].magazine = 30;
			kit.slots[i].reserve = 120;
			net::WeaponTimersWire &t = i == 0 ? kit.timers[0] : kit.idle[i];
			t.slot = i;
			t.phase = i == 0 ? sw::WeaponPhase::Ready : sw::WeaponPhase::None;
			t.phase_start = i == 0 ? -250000 : net::kWeaponsWireTimeZero;
			t.phase_end = net::kWeaponsWireTimeZero;
			t.next_shot_time = i == 0 ? 400000 : net::kWeaponsWireTimeZero;
			t.spread_time = i == 0 ? -1000 : net::kWeaponsWireTimeZero;
			t.hold_start = net::kWeaponsWireTimeZero;
			t.spread = i == 0 ? 1.5f : 0.0f;
			if (i == 0) {
				t.recoil = { 0.4f, -0.1f };
			}
		}
		kit.sprint_recover_until = net::kWeaponsWireTimeZero;
		s.own_weapons = kit;
		const size_t with_kit = net::encode_snapshot(s).size();
		MESSAGE("snapshot header + own state + a 4-slot kit: ", with_kit, " bytes");
		auto dk = net::decode_snapshot(net::encode_snapshot(s));
		REQUIRE(dk);
		CHECK(dk->own_weapons == kit);

		for (const size_t players : { size_t(64), size_t(128) }) {
			net::Snapshot full = s;
			std::vector<net::OtherSoldier> all;
			for (size_t i = 1; i < players; ++i) {
				all.push_back(sample_other_v2(rng, uint8_t(i), full.server_tick));
			}
			full.others = all;
			const std::vector<uint8_t> main = net::encode_snapshot(full, &dropped);
			auto dm = net::decode_snapshot(main);
			REQUIRE(dm);
			const size_t in_main = dm->others.size();
			CHECK(in_main + dropped == all.size());
			net::SnapshotOthers rest;
			rest.seq = full.seq;
			rest.server_tick = full.server_tick;
			rest.others.assign(all.begin() + std::ptrdiff_t(in_main), all.end());
			size_t rest_dropped = 0;
			const std::vector<uint8_t> extra = net::encode_snapshot_others(rest, &rest_dropped);
			auto de = net::decode_snapshot_others(extra);
			REQUIRE(de);
			MESSAGE(players, " players: Snapshot ", main.size(), " B with ", in_main, " others; SnapshotOthers ", extra.size(), " B with ", de->others.size(),
					" others; dropped ", rest_dropped);
			CHECK(main.size() <= net::kMaxDatagramBytes);
			CHECK(extra.size() <= net::kMaxDatagramBytes);

			CHECK(rest_dropped == (players == 64 ? 0u : players - 1 - in_main - de->others.size()));
			CHECK(in_main + de->others.size() + rest_dropped == players - 1);
			for (size_t i = 0; i < in_main; ++i) {
				CHECK(dm->others[i] == all[i]);
			}
			for (size_t i = 0; i < de->others.size(); ++i) {
				CHECK(de->others[i] == all[in_main + i]);
			}
			CHECK(de->seq == full.seq);
			CHECK(de->server_tick == full.server_tick);
		}

		net::Snapshot bare = sample_snapshot(rng, 0);
		for (size_t i = 1; i < 128; ++i) {
			bare.others.push_back(sample_other_v2(rng, uint8_t(i), bare.server_tick));
		}
		const size_t without = net::decode_snapshot(net::encode_snapshot(bare))->others.size();
		bare.own.reset();
		const size_t no_own = net::decode_snapshot(net::encode_snapshot(bare))->others.size();
		net::SnapshotOthers many;
		many.others = bare.others;
		const size_t so_cap = net::decode_snapshot_others(net::encode_snapshot_others(many, &dropped))->others.size();
		MESSAGE("others per datagram: Snapshot with own state ", without, ", without own state ", no_own, ", SnapshotOthers ", so_cap);
		CHECK(without == 49);
		CHECK(so_cap == 52);
		CHECK(dropped == 127 - 52);
	}

	TEST_CASE("proto v2: set_other_weapon masks to the wire widths; others' weapon fields round trip") {
		net::OtherSoldier o;
		net::set_other_weapon(o, 300, sw::WeaponPhase::Reloading, true, true, 7, 17);
		CHECK(o.weapon == 0xFF);
		CHECK(o.weapon_phase == uint8_t(sw::WeaponPhase::Reloading));
		CHECK(o.reload_seq == 3);
		CHECK(o.burst_seq == 1);
		net::set_other_weapon(o, 254, sw::WeaponPhase::Releasing, false, true, 4, 16);
		CHECK(o.weapon == 254);
		CHECK(o.reload_seq == 0);
		CHECK(o.burst_seq == 0);
		CHECK_FALSE(o.firing);
		CHECK(o.ads);
		net::SnapshotOthers m;
		m.seq = 1;
		m.server_tick = 5;
		m.others.push_back(o);
		auto d = net::decode_snapshot_others(net::encode_snapshot_others(m));
		REQUIRE(d);
		CHECK(d->others[0] == o);

		m.others.push_back(o);
		CHECK_FALSE(net::decode_snapshot_others(net::encode_snapshot_others(m)));
		const auto count_128 = craft(net::MessageType::SnapshotOthers, [&](net::BitWriter &w) {
			w.write_bits(1, 16);
			w.write_bits(5, 32);
			w.write_bits(128, 8);
		});
		CHECK_FALSE(net::decode_snapshot_others(count_128));
		std::vector<uint8_t> big = net::encode_snapshot_others({ 1, 5, {} });
		big.resize(net::kMaxDatagramBytes + 1, 0);
		CHECK_FALSE(net::decode_snapshot_others(big));
	}

	TEST_CASE("proto v2: ShotEvents round trip at the documented precision, nearest shooters first, the rest dropped and counted") {
		Rng rng(25);
		const sim::Tick tick = 50000;
		net::ShotEvents m;
		m.server_tick = tick;
		std::vector<uint32_t> next(16, 0);
		for (size_t i = 0; i < 48; ++i) {
			const uint8_t slot = uint8_t(1 + rng.below(12));
			next[slot] += (i % 11 == 0) ? 1000 : 1 + rng.below(3);
			m.shots.push_back(sample_shot(rng, slot, next[slot], tick));
		}
		m.shots.push_back(sample_shot(rng, 13, 0xFFFFFFFFu, tick));
		m.shots.push_back(sample_shot(rng, 13, 4, tick));
		const sim::Vec3 eye{ 10.0f, 60.0f, -20.0f };
		size_t dropped = 99;
		const std::vector<uint8_t> b = net::encode_shot_events(m, kFrame, eye, &dropped);
		CHECK(dropped == 0);
		CHECK(b.size() <= net::kMaxDatagramBytes);
		auto d = net::decode_shot_events(b, kFrame);
		REQUIRE(d);
		REQUIRE(d->shots.size() == m.shots.size());
		MESSAGE("ShotEvents with ", m.shots.size(), " mixed shots: ", b.size(), " bytes");
		double t_err = 0, o_err = 0, dir_err = 0, aim_err = 0, cone_err = 0, power_err = 0;
		size_t matched = 0;
		for (const net::RemoteShot &orig : m.shots) {
			const std::optional<net::RemoteShot> q = net::quantize_remote_shot(orig, kFrame, tick);
			REQUIRE(q.has_value());
			for (const net::RemoteShot &got : d->shots) {
				if (got.slot == orig.slot && got.shot_index == orig.shot_index) {
					++matched;
					CHECK(same_shot(got, *q));
					t_err = std::max(t_err, std::fabs(double(got.time - orig.time)));
					o_err = std::max(o_err, max_axis_error(got.origin, orig.origin));
					dir_err = std::max(dir_err, angle_between(got.direction, orig.direction));
					if (orig.tracer) {
						aim_err = std::max(aim_err, angle_between(got.aim, orig.aim));
					}
					if (orig.pellets > 1) {
						cone_err = std::max(cone_err, std::fabs(double(got.pellet_cone) - orig.pellet_cone));
					}
					power_err = std::max(power_err, std::fabs(double(got.power) - orig.power));
					CHECK(got.pellets == orig.pellets);
					CHECK(got.def == orig.def);
					CHECK(got.tracer == orig.tracer);
				}
			}
		}
		CHECK(matched == m.shots.size());
		MESSAGE("shot precision: time ", t_err, " units, origin ", o_err, " m, direction ", dir_err, " deg, aim ", aim_err, " deg, cone ", cone_err,
				" deg, power ", power_err);
		CHECK(t_err <= 8.0);
		CHECK(o_err <= 1.0 / 512.0 + 1e-4);
		CHECK(dir_err < 0.01);
		CHECK(aim_err < 0.1);
		CHECK(cone_err <= 1.0 / 128.0 + 1e-6);
		CHECK(power_err <= double(sw::kMaxPower) / 4095.0 / 2.0 + 1e-5);

		std::vector<double> nearest(128, 1e30);
		for (const net::RemoteShot &s : m.shots) {
			const double dx = s.origin[0] - eye[0], dy = s.origin[1] - eye[1], dz = s.origin[2] - eye[2];
			nearest[s.slot] = std::min(nearest[s.slot], dx * dx + dy * dy + dz * dz);
		}
		for (size_t i = 1; i < d->shots.size(); ++i) {
			CHECK(nearest[d->shots[i - 1].slot] <= nearest[d->shots[i].slot]);
		}

		net::ShotEvents ex;
		ex.server_tick = tick;
		ex.shots.push_back(sample_shot(rng, 1, 1, tick));
		ex.shots[0].pellets = 64;
		ex.shots[0].pellet_cone = sw::kMaxAngle;
		ex.shots[0].power = 0;
		ex.shots.push_back(sample_shot(rng, 1, 2, tick));
		ex.shots[1].power = sw::kMaxPower;
		ex.shots[1].pellets = 2;
		ex.shots[1].pellet_cone = 200.0f;
		auto dex = net::decode_shot_events(net::encode_shot_events(ex, kFrame, eye), kFrame);
		REQUIRE(dex);
		CHECK(dex->shots[0].pellets == 64);
		CHECK(dex->shots[0].pellet_cone == sw::kMaxAngle);
		CHECK(dex->shots[0].power == 0.0f);
		CHECK(dex->shots[1].power == sw::kMaxPower);
		CHECK(dex->shots[1].pellet_cone == sw::kMaxAngle);
	}

	TEST_CASE("proto v2: ShotEvents fill one datagram, and drop shots that cannot travel") {
		Rng rng(26);
		const sim::Tick tick = 777;
		const sim::Vec3 eye{};

		for (const bool tracer : { false, true }) {
			net::ShotEvents m;
			m.server_tick = tick;
			for (uint32_t i = 0; i < 200; ++i) {
				net::RemoteShot s = sample_shot(rng, 5, 100 + i, tick);
				s.tracer = tracer;
				s.aim = s.direction;
				s.pellets = 1;
				s.pellet_cone = 0;
				s.power = 1;
				m.shots.push_back(s);
			}
			size_t dropped = 0;
			const std::vector<uint8_t> b = net::encode_shot_events(m, kFrame, eye, &dropped);
			auto d = net::decode_shot_events(b, kFrame);
			REQUIRE(d);
			MESSAGE(std::string(tracer ? "traced" : "untraced"), " single shots per datagram: ", d->shots.size(), " (", b.size(), " B), dropped ", dropped);
			CHECK(b.size() <= net::kMaxDatagramBytes);
			CHECK(d->shots.size() + dropped == 200);
			CHECK(d->shots.size() == (tracer ? 58u : 69u));
		}

		net::ShotEvents m;
		m.server_tick = tick;
		for (uint32_t i = 0; i < 300; ++i) {
			m.shots.push_back(sample_shot(rng, uint8_t(rng.below(128)), i, tick));
		}
		size_t dropped = 0;
		auto d = net::decode_shot_events(net::encode_shot_events(m, kFrame, eye, &dropped), kFrame);
		REQUIRE(d);
		CHECK(d->shots.size() + dropped == 300);
		CHECK(d->shots.size() <= net::kMaxShotsPerDatagram);
		std::vector<double> nearest(128, 1e30);
		for (const net::RemoteShot &s : m.shots) {
			nearest[s.slot] = std::min(nearest[s.slot], double(s.origin[0]) * s.origin[0] + double(s.origin[1]) * s.origin[1] + double(s.origin[2]) * s.origin[2]);
		}
		double kept_max = 0;
		for (const net::RemoteShot &s : d->shots) {
			kept_max = std::max(kept_max, nearest[s.slot]);
		}
		for (const net::RemoteShot &s : m.shots) {
			bool kept = false;
			for (const net::RemoteShot &k : d->shots) {
				kept = kept || (k.slot == s.slot && k.shot_index == s.shot_index);
			}
			if (!kept) {
				CHECK(nearest[s.slot] >= kept_max);
			}
		}

		net::ShotEvents bad;
		bad.server_tick = tick;
		bad.shots.push_back(sample_shot(rng, 1, 1, tick));
		net::RemoteShot s = sample_shot(rng, 2, 1, tick);
		s.slot = 128;
		bad.shots.push_back(s);
		s = sample_shot(rng, 3, 1, tick);
		s.def = 255;
		bad.shots.push_back(s);
		s = sample_shot(rng, 4, 1, tick);
		s.time = sw::tick_start(tick - net::kMaxShotTicksBack) - 1;
		bad.shots.push_back(s);
		s = sample_shot(rng, 5, 1, tick);
		s.time = sw::tick_end(tick);
		bad.shots.push_back(s);
		s = sample_shot(rng, 6, 1, tick);
		s.origin[0] = kFrame.origin_min[0] - 1.0f;
		bad.shots.push_back(s);
		s = sample_shot(rng, 7, 1, tick);
		s.origin[2] = kFrame.origin_min[2] + 4096.5f;
		bad.shots.push_back(s);
		s = sample_shot(rng, 8, 1, tick);
		s.pellets = 0;
		bad.shots.push_back(s);
		bad.shots.push_back(bad.shots[0]);
		s = sample_shot(rng, 9, 1, tick);
		s.time = sw::tick_start(tick - net::kMaxShotTicksBack);
		bad.shots.push_back(s);
		auto db = net::decode_shot_events(net::encode_shot_events(bad, kFrame, eye, &dropped), kFrame);
		REQUIRE(db);
		CHECK(dropped == 8);
		CHECK(db->shots.size() == 2);
		for (const net::RemoteShot &x : bad.shots) {
			const bool travels = net::quantize_remote_shot(x, kFrame, tick).has_value();
			CHECK(travels == (x.slot == 1 || x.slot == 9));
		}
		net::ShotEvents zero;
		zero.server_tick = 0;
		zero.shots.push_back(sample_shot(rng, 1, 1, 1));
		auto dz = net::decode_shot_events(net::encode_shot_events(zero, kFrame, eye, &dropped), kFrame);
		REQUIRE(dz);
		CHECK(dz->shots.empty());
		CHECK(dropped == 1);

		sw::ShotEvent ev;
		ev.time = 12345;
		ev.player_slot = 7;
		ev.shot_index = 99;
		ev.def = 3;
		ev.pellets = 9;
		ev.origin = { 1, 2, 3 };
		ev.aim = { 0, 1, 0 };
		ev.direction = { 0, 0, 1 };
		ev.pellet_cone = 1.5f;
		ev.tracer = true;
		ev.power = 0.25f;
		const net::RemoteShot r = net::remote_shot_of(ev);
		CHECK(r.slot == 7);
		CHECK(r.shot_index == 99);
		CHECK(r.def == 3);
		CHECK(r.time == 12345);
		CHECK(r.pellets == 9);
		CHECK(r.origin == ev.origin);
		CHECK(r.aim == ev.aim);
		CHECK(r.direction == ev.direction);
		CHECK(r.pellet_cone == 1.5f);
		CHECK(r.tracer);
		CHECK(r.power == 0.25f);
	}

	TEST_CASE("proto v2: hostile ShotEvents are refused") {
		const sim::Tick tick = 100;

		const auto shot = [](net::BitWriter &w, uint8_t p_slot, bool p_first, uint32_t p_index_or_delta, uint32_t p_def, uint32_t p_back) {
			w.write_bits(p_slot, 7);
			if (p_first) {
				w.write_bits(p_index_or_delta, 32);
			} else {
				w.write_bits(p_index_or_delta, 4);
				if (p_index_or_delta == 0) {
					w.write_bits(5, 32);
				}
			}
			w.write_bits(p_def, 8);
			w.write_bits(p_back, 3);
			w.write_bits(0, 16);
			for (int i = 0; i < 3; ++i) {
				w.write_bits(1000, 20);
			}
			w.write_bits(0x8000, 16);
			w.write_bits(0x8000, 16);
			w.write_bits(0, 6);
			w.write_bool(false);
			w.write_bool(false);
		};
		const auto message = [&](sim::Tick p_tick, auto &&p_body) {
			return craft(net::MessageType::ShotEvents, [&](net::BitWriter &w) {
				w.write_bits(p_tick, 32);
				p_body(w);
			});
		};
		CHECK(net::decode_shot_events(message(tick, [&](net::BitWriter &w) {
			w.write_bits(2, 8);
			shot(w, 1, true, 5, 3, 0);
			shot(w, 1, false, 1, 3, 1);
		}), kFrame));
		CHECK_FALSE(net::decode_shot_events(message(tick, [&](net::BitWriter &w) {
			w.write_bits(2, 8);
			shot(w, 1, true, 5, 3, 0);
			shot(w, 1, false, 0, 3, 0);
		}), kFrame));
		CHECK_FALSE(net::decode_shot_events(message(tick, [&](net::BitWriter &w) {
			w.write_bits(3, 8);
			shot(w, 1, true, 5, 3, 0);
			shot(w, 1, false, 1, 3, 0);
			shot(w, 1, false, 0, 3, 0);
		}), kFrame));
		CHECK_FALSE(net::decode_shot_events(message(tick, [&](net::BitWriter &w) {
			w.write_bits(1, 8);
			shot(w, 1, true, 5, 255, 0);
		}), kFrame));
		CHECK_FALSE(net::decode_shot_events(message(3, [&](net::BitWriter &w) {
			w.write_bits(1, 8);
			shot(w, 1, true, 5, 3, 3);
		}), kFrame));
		CHECK_FALSE(net::decode_shot_events(message(tick, [&](net::BitWriter &w) {
			w.write_bits(129, 8);
		}), kFrame));
		CHECK_FALSE(net::decode_shot_events(message(tick, [&](net::BitWriter &w) {
			w.write_bits(60, 8);
			shot(w, 1, true, 5, 3, 0);
		}), kFrame));
	}

	TEST_CASE("proto v2: CombatEvents round trip; out-of-range values clamp and count; hostile bytes are refused") {
		Rng rng(27);
		const sim::Tick tick = 4000;
		const net::CombatEvents m = sample_combat(rng, tick, 20, 6, 4);
		size_t clamped = 99;
		const std::vector<uint8_t> b = net::encode_combat_events(m, kFrame, &clamped);
		CHECK(clamped == 0);
		auto d = net::decode_combat_events(b, kFrame);
		REQUIRE(d);
		REQUIRE(d->hits.size() == 20);
		REQUIRE(d->deaths.size() == 6);
		REQUIRE(d->detonations.size() == 4);
		const size_t one_hit = net::encode_combat_events(sample_combat(rng, tick, 1, 0, 0), kFrame).size();
		const size_t one_death = net::encode_combat_events(sample_combat(rng, tick, 0, 1, 0), kFrame).size();
		const size_t one_det = net::encode_combat_events(sample_combat(rng, tick, 0, 0, 1), kFrame).size();
		const size_t empty = net::encode_combat_events(sample_combat(rng, tick, 0, 0, 0), kFrame).size();
		MESSAGE("CombatEvents: empty ", empty, " B; one hit +", one_hit - empty, " B, one death +", one_death - empty, " B, one detonation +",
				one_det - empty, " B; 20 / 6 / 4: ", b.size(), " B");
		double hp_err = 0, pos_err = 0, normal_err = 0, t_err = 0;
		for (size_t i = 0; i < 20; ++i) {
			const net::HitEvent &x = m.hits[i], &y = d->hits[i];
			CHECK(x.victim == y.victim);
			CHECK(x.shooter == y.shooter);
			CHECK(x.shot_index == y.shot_index);
			CHECK(x.pellet == y.pellet);
			CHECK(x.zone == y.zone);
			hp_err = std::max({ hp_err, std::fabs(double(x.damage) - y.damage), std::fabs(double(x.health_after) - y.health_after) });
			pos_err = std::max(pos_err, max_axis_error(x.position, y.position));
			normal_err = std::max(normal_err, angle_between(x.normal, y.normal));
			t_err = std::max(t_err, std::fabs(double(x.time - y.time)));
		}
		for (size_t i = 0; i < 6; ++i) {
			const net::DeathEvent &x = m.deaths[i], &y = d->deaths[i];
			CHECK(x.victim == y.victim);
			CHECK(x.killer == y.killer);
			CHECK(x.def == y.def);
			CHECK(x.zone == y.zone);
			CHECK(x.shot_index == y.shot_index);
			CHECK(x.headshot == y.headshot);
			t_err = std::max(t_err, std::fabs(double(x.time - y.time)));
		}
		for (size_t i = 0; i < 4; ++i) {
			const net::DetonationWire &x = m.detonations[i], &y = d->detonations[i];
			CHECK(x.shooter == y.shooter);
			CHECK(x.shot_index == y.shot_index);
			CHECK(x.def == y.def);
			CHECK(x.dud == y.dud);
			pos_err = std::max(pos_err, max_axis_error(x.position, y.position));
			normal_err = std::max(normal_err, angle_between(x.normal, y.normal));
			t_err = std::max(t_err, std::fabs(double(x.time - y.time)));
		}
		MESSAGE("combat precision: hp ", hp_err, ", position ", pos_err, " m, normal ", normal_err, " deg, time ", t_err, " units");
		CHECK(hp_err <= 0.005 + 1e-4);
		CHECK(pos_err <= 1.0 / 512.0 + 1e-4);
		CHECK(normal_err < 1.5);
		CHECK(t_err <= 8.0);

		net::CombatEvents c = sample_combat(rng, tick, 2, 2, 2);
		c.hits[0].damage = -5;
		c.hits[0].health_after = 1e9f;
		c.hits[1].victim = 200;
		c.deaths[0].def = 300;
		c.detonations[0].position[1] = -10000;
		c.detonations[1].def = sw::kNoWeapon;
		auto dc = net::decode_combat_events(net::encode_combat_events(c, kFrame, &clamped), kFrame);
		REQUIRE(dc);
		CHECK(clamped == 6);
		CHECK(dc->hits.size() == 1);
		CHECK(dc->hits[0].damage == 0.0f);
		CHECK(dc->hits[0].health_after == 100000.0f);
		CHECK(dc->deaths[0].def == sw::kNoWeapon);
		CHECK(dc->detonations.size() == 1);
		CHECK(dc->detonations[0].position[1] == kFrame.origin_min[1]);
		net::CombatEvents many = sample_combat(rng, tick, 200, 100, 10);
		auto dmany = net::decode_combat_events(net::encode_combat_events(many, kFrame, &clamped), kFrame);
		REQUIRE(dmany);
		CHECK(dmany->hits.size() + dmany->deaths.size() + dmany->detonations.size() == net::kMaxCombatEventsPerMessage);
		CHECK(clamped == 310 - net::kMaxCombatEventsPerMessage);
		CHECK(dmany->detonations.empty());

		const auto message = [&](sim::Tick p_tick, auto &&p_body) {
			return craft(net::MessageType::CombatEvents, [&](net::BitWriter &w) {
				w.write_bits(p_tick, 32);
				p_body(w);
			});
		};
		CHECK_FALSE(net::decode_combat_events(message(tick, [](net::BitWriter &w) {
			w.write_varint(200);
			w.write_varint(57);
			w.write_varint(0);
			w.write_varint(0);
		}), kFrame));
		CHECK_FALSE(net::decode_combat_events(message(tick, [](net::BitWriter &w) {
			w.write_varint(0);
			w.write_varint(~0ull);
			w.write_varint(0);
			w.write_varint(0);
		}), kFrame));
		CHECK_FALSE(net::decode_combat_events(message(tick, [](net::BitWriter &w) {
			w.write_varint(0);
			w.write_varint(10);
			w.write_varint(0);
			w.write_varint(0);
		}), kFrame));
		const auto death = [](net::BitWriter &w, uint32_t p_back) {
			w.write_bits(1, 7);
			w.write_bits(2, 7);
			w.write_bits(3, 8);
			w.write_bits(1, 2);
			w.write_bits(77, 32);
			w.write_bool(false);
			w.write_varint(p_back);
			w.write_bits(0, 16);
			w.write_bool(false);
		};
		CHECK(net::decode_combat_events(message(tick, [&](net::BitWriter &w) {
			w.write_varint(0);
			w.write_varint(1);
			w.write_varint(0);
			w.write_varint(0);
			death(w, tick - 1);
		}), kFrame));
		CHECK_FALSE(net::decode_combat_events(message(tick, [&](net::BitWriter &w) {
			w.write_varint(0);
			w.write_varint(1);
			w.write_varint(0);
			w.write_varint(0);
			death(w, tick);
		}), kFrame));
		CHECK_FALSE(net::decode_combat_events(message(0, [&](net::BitWriter &w) {
			w.write_varint(0);
			w.write_varint(1);
			w.write_varint(0);
			w.write_varint(0);
			death(w, 0);
		}), kFrame));
		const auto detonation = [](net::BitWriter &w, uint32_t p_def) {
			w.write_bits(1, 7);
			w.write_bits(77, 32);
			w.write_bits(p_def, 8);
			for (int i = 0; i < 3; ++i) {
				w.write_bits(1000, 20);
			}
			w.write_bits(128, 8);
			w.write_bits(255, 8);
			w.write_bool(false);
			w.write_varint(0);
			w.write_bits(0, 16);
		};
		CHECK(net::decode_combat_events(message(tick, [&](net::BitWriter &w) {
			w.write_varint(0);
			w.write_varint(0);
			w.write_varint(1);
			w.write_varint(0);
			detonation(w, 254);
		}), kFrame));
		CHECK_FALSE(net::decode_combat_events(message(tick, [&](net::BitWriter &w) {
			w.write_varint(0);
			w.write_varint(0);
			w.write_varint(1);
			w.write_varint(0);
			detonation(w, 255);
		}), kFrame));
		const auto hit = [](net::BitWriter &w, uint32_t p_damage) {
			w.write_bits(1, 7);
			w.write_bits(2, 7);
			w.write_bits(77, 32);
			w.write_bits(0, 6);
			w.write_bits(1, 2);
			w.write_bits(p_damage, 24);
			w.write_bits(0, 24);
			for (int i = 0; i < 3; ++i) {
				w.write_bits(1000, 20);
			}
			w.write_bits(128, 8);
			w.write_bits(255, 8);
			w.write_varint(0);
			w.write_bits(0, 16);
		};
		CHECK(net::decode_combat_events(message(tick, [&](net::BitWriter &w) {
			w.write_varint(1);
			w.write_varint(0);
			w.write_varint(0);
			w.write_varint(0);
			hit(w, 10'000'000);
		}), kFrame));
		CHECK_FALSE(net::decode_combat_events(message(tick, [&](net::BitWriter &w) {
			w.write_varint(1);
			w.write_varint(0);
			w.write_varint(0);
			w.write_varint(0);
			hit(w, 10'000'001);
		}), kFrame));
	}

	TEST_CASE("proto v2: the weapons wire round trips every field; offsets saturate at 35 s; hostile bytes are refused") {
		Rng rng(28);
		size_t max_bytes = 0;
		for (int i = 0; i < 2000; ++i) {
			const net::WeaponsWire w = sample_weapons_wire(rng);
			net::BitWriter bw(4096);
			net::write_weapons_wire(bw, w);
			REQUIRE_FALSE(bw.overflowed());
			const std::vector<uint8_t> bytes = bw.take();
			max_bytes = std::max(max_bytes, bytes.size());
			net::BitReader br(bytes);
			net::WeaponsWire r;
			REQUIRE(net::read_weapons_wire(br, r).ok());
			REQUIRE(r == w);

			if (i < 50) {
				for (size_t n = 0; n + 1 < bytes.size(); ++n) {
					net::BitReader tr(std::span<const uint8_t>(bytes.data(), n));
					net::WeaponsWire t;
					REQUIRE_FALSE(net::read_weapons_wire(tr, t).ok());
				}
			}
		}
		MESSAGE("weapons wire: largest random 8-slot view ", max_bytes, " bytes");

		sw::SoldierWeaponsState st;
		st.tick = 100000;
		st.slots[0].def = 1;
		st.slots[0].phase = sw::WeaponPhase::Ready;
		st.slots[0].spread_time = sw::tick_start(st.tick) - 40 * sw::kTimeUnitsPerSecond;
		st.slots[0].next_shot_time = sw::tick_start(st.tick) + 5;
		st.slots[3].mode_counter = 3;
		const net::WeaponsWire v = net::weapons_wire_view(st, st.tick);
		CHECK(v.timers[0].spread_time == -net::kWeaponsWireMaxOffset);
		CHECK(v.timers[0].next_shot_time == 5);
		CHECK(v.timers[0].phase_start == net::kWeaponsWireTimeZero);
		CHECK(v.slots[3].mode_counter == 3);
		auto back = net::weapons_from_wire(v, st.tick, 0, 2);
		REQUIRE(back.ok());
		CHECK(back->slots[0].next_shot_time == st.slots[0].next_shot_time);
		CHECK(back->slots[0].phase_start == 0);
		CHECK(back->slots[0].spread_time == sw::tick_start(st.tick) - sw::SimTime(net::kWeaponsWireMaxOffset));
		CHECK(back->slots[3].mode_counter == 3);
		CHECK_FALSE(net::weapons_from_wire(v, st.tick, 0, 1).ok());
		net::WeaponsWire bad = v;
		bad.active = 8;
		CHECK_FALSE(net::weapons_from_wire(bad, st.tick, 0, 2).ok());
		bad = v;
		bad.timers[0].slot = 0xFF;
		CHECK_FALSE(net::weapons_from_wire(bad, st.tick, 0, 2).ok());

		const auto wire_bytes = [&](uint32_t p_active, uint64_t p_def, int64_t p_offset, float p_spread) {
			net::BitWriter w(256);
			w.write_bits(1, 32);
			w.write_bits(p_active, 8);
			w.write_bits(0xFF, 8);
			w.write_bits(0, 16);
			w.write_bool(true);
			w.write_bool(true);
			w.write_varint(p_def);
			w.write_bits(0, 2);
			w.write_bits(0, 2);
			w.write_varint(0);
			w.write_varint(0);
			w.write_bits(1, 3);
			w.write_bits(0, 6);
			w.write_bits(0, 7);
			w.write_bool(false);
			w.write_signed(p_offset, 32);
			w.write_bool(true);
			w.write_bool(true);
			w.write_f32(p_spread);
			w.write_bool(true);
			w.write_bool(true);
			w.write_bool(true);
			for (int i = 1; i < 8; ++i) {
				w.write_bool(false);
				w.write_bits(0, 2);
			}
			return w.take();
		};
		const auto reads = [](const std::vector<uint8_t> &p_bytes) {
			net::BitReader r(p_bytes);
			net::WeaponsWire w;
			return net::read_weapons_wire(r, w).ok();
		};
		CHECK(reads(wire_bytes(0, 3, -1000, 1.0f)));
		CHECK_FALSE(reads(wire_bytes(8, 3, -1000, 1.0f)));
		CHECK_FALSE(reads(wire_bytes(0, sw::kNoWeapon, -1000, 1.0f)));
		CHECK_FALSE(reads(wire_bytes(0, ~0ull >> 1, -1000, 1.0f)));
		CHECK_FALSE(reads(wire_bytes(0, 3, int64_t(net::kWeaponsWireMaxOffset) + 1, 1.0f)));
		CHECK_FALSE(reads(wire_bytes(0, 3, -1000, std::numeric_limits<float>::infinity())));
		CHECK_FALSE(reads(wire_bytes(0, 3, -1000, std::numeric_limits<float>::quiet_NaN())));

		net::Snapshot s = sample_snapshot(rng, 0);
		net::WeaponsWire nan = sample_weapons_wire(rng);
		nan.timers[0].spread = std::numeric_limits<float>::quiet_NaN();
		s.own_weapons = nan;
		CHECK_FALSE(net::decode_snapshot(net::encode_snapshot(s)));
	}
}

TEST_SUITE("net-proto") {
	TEST_CASE("proto w3: the others' team round trips in Snapshot and SnapshotOthers; team 3 is refused") {
		Rng rng(311);
		net::OtherSoldier o;
		for (const uint8_t t : { uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(255) }) {
			net::set_other_team(o, t);
			CHECK(o.team == (t == 1 || t == 2 ? t : 0));
		}
		net::Snapshot s = sample_snapshot(rng, 0);
		net::SnapshotOthers so;
		so.seq = s.seq;
		so.server_tick = s.server_tick;
		for (uint8_t i = 1; i <= 6; ++i) {
			net::OtherSoldier x = sample_other_v2(rng, i, s.server_tick);
			net::set_other_team(x, uint8_t(i % 3));
			s.others.push_back(x);
			so.others.push_back(x);
		}
		auto ds = net::decode_snapshot(net::encode_snapshot(s));
		REQUIRE(ds);
		REQUIRE(ds->others.size() == s.others.size());
		auto dso = net::decode_snapshot_others(net::encode_snapshot_others(so));
		REQUIRE(dso);
		REQUIRE(dso->others.size() == so.others.size());
		for (size_t i = 0; i < s.others.size(); ++i) {
			CHECK(ds->others[i] == s.others[i]);
			CHECK(ds->others[i].team == uint8_t((i + 1) % 3));
			CHECK(dso->others[i] == so.others[i]);
		}

		s.others[2].team = 3;
		so.others[4].team = 3;
		CHECK_FALSE(net::decode_snapshot(net::encode_snapshot(s)));
		CHECK_FALSE(net::decode_snapshot_others(net::encode_snapshot_others(so)));
	}
}

namespace {

net::PlayerRow sample_row(Rng &r_rng, uint8_t p_slot) {
	net::PlayerRow row;
	row.slot = p_slot;
	row.team = uint8_t(r_rng.below(3));
	row.kit = uint8_t(r_rng.below(4));
	row.bot = (r_rng.next() & 1) != 0;
	row.alive = (r_rng.next() & 1) != 0;
	row.ping_ms = uint16_t(r_rng.next());
	row.kills = uint16_t(r_rng.next());
	row.deaths = uint16_t(r_rng.next());
	row.score = int32_t(uint32_t(r_rng.next()));
	const std::string name = "p" + std::to_string(p_slot) + (p_slot % 3 == 0 ? "\xC3\xA9\xE2\x82\xAC" : "");
	net::set_player_name(row, name);
	return row;
}

net::MatchState sample_match_state(Rng &r_rng) {
	net::MatchState m;
	m.phase = net::MatchPhase(r_rng.below(4));
	m.round_start_tick = uint32_t(r_rng.next());
	m.countdown_end_tick = uint32_t(r_rng.next());
	m.kills = { int32_t(uint32_t(r_rng.next())), -int32_t(r_rng.below(5)) };
	m.kill_limit = int32_t(r_rng.below(1000));
	m.winner = uint8_t(r_rng.below(3));
	m.killer_camera = (r_rng.next() & 1) != 0;
	m.round_end_tick = uint32_t(r_rng.next());
	m.unspawn_tick = uint32_t(r_rng.next());
	m.restart_tick = uint32_t(r_rng.next());
	m.restrict_until_tick = UINT32_MAX;
	m.restrict_mask = uint32_t(r_rng.next()) & net::restrict_bits::kAll;
	return m;
}

bool same_match_state(const net::MatchState &p_a, const net::MatchState &p_b) {
	return p_a.phase == p_b.phase && p_a.round_start_tick == p_b.round_start_tick && p_a.countdown_end_tick == p_b.countdown_end_tick &&
			p_a.kills == p_b.kills && p_a.kill_limit == p_b.kill_limit && p_a.winner == p_b.winner && p_a.killer_camera == p_b.killer_camera &&
			p_a.round_end_tick == p_b.round_end_tick && p_a.unspawn_tick == p_b.unspawn_tick && p_a.restart_tick == p_b.restart_tick &&
			p_a.restrict_until_tick == p_b.restrict_until_tick && p_a.restrict_mask == p_b.restrict_mask;
}

bool same_row(const net::PlayerRow &p_a, const net::PlayerRow &p_b) {
	return p_a.slot == p_b.slot && p_a.team == p_b.team && p_a.kit == p_b.kit && p_a.bot == p_b.bot && p_a.alive == p_b.alive &&
			p_a.name_bytes == p_b.name_bytes && p_a.ping_ms == p_b.ping_ms && p_a.kills == p_b.kills && p_a.deaths == p_b.deaths &&
			p_a.score == p_b.score && p_a.name == p_b.name;
}

bool same_player_list(const net::PlayerList &p_a, const net::PlayerList &p_b) {
	if (p_a.server_tick != p_b.server_tick || p_a.full != p_b.full || p_a.rows.size() != p_b.rows.size() || p_a.removed != p_b.removed) {
		return false;
	}
	for (size_t i = 0; i < p_a.rows.size(); ++i) {
		if (!same_row(p_a.rows[i], p_b.rows[i])) {
			return false;
		}
	}
	return true;
}

net::CombatEvents sample_combat_v3(Rng &r_rng, sim::Tick p_tick, size_t p_hits, size_t p_deaths, size_t p_detonations, size_t p_revives) {
	net::CombatEvents m = sample_combat(r_rng, p_tick, p_hits, p_deaths, p_detonations);
	for (size_t i = 0; i < m.deaths.size(); i += 2) {
		m.deaths[i].respawn_tick = p_tick + 600;
		m.deaths[i].revive_until = p_tick + 300;
		m.deaths[i].killer_health = r_rng.uniform(1, 100);
	}
	for (size_t i = 0; i < p_revives; ++i) {
		m.revives.push_back({ uint8_t(r_rng.below(128)), uint8_t(r_rng.below(128)), p_tick - r_rng.below(40) });
	}
	return m;
}

int decode_reencode_v3(net::MessageType p_type, std::span<const uint8_t> p_bytes) {
	switch (p_type) {
		case net::MessageType::MatchState: {
			auto a = net::decode_match_state(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_match_state(net::encode_match_state(*a));
			return b && same_match_state(*a, *b) ? 1 : 0;
		}
		case net::MessageType::PlayerList: {
			auto a = net::decode_player_list(p_bytes);
			if (!a) {
				return -1;
			}
			size_t clamped = 0;
			auto b = net::decode_player_list(net::encode_player_list(*a, &clamped));
			return b && clamped == 0 && same_player_list(*a, *b) ? 1 : 0;
		}
		case net::MessageType::SpawnRequest: {
			auto a = net::decode_spawn_request(p_bytes);
			if (!a) {
				return -1;
			}
			auto b = net::decode_spawn_request(net::encode_spawn_request(*a));
			return b && b->kit == a->kit && b->spawn_choice == a->spawn_choice ? 1 : 0;
		}
		default:
			return -1;
	}
}

void append_v3_samples(std::vector<std::pair<std::string, std::vector<uint8_t>>> &r_out) {
	Rng rng(0x3003);
	r_out.emplace_back("MatchState", net::encode_match_state(sample_match_state(rng)));
	net::PlayerList full;
	full.server_tick = 90000;
	full.full = true;
	for (uint8_t s = 0; s < 12; ++s) {
		full.rows.push_back(sample_row(rng, uint8_t(s * 5)));
	}
	r_out.emplace_back("PlayerList (full)", net::encode_player_list(full));
	net::PlayerList delta;
	delta.server_tick = 90015;
	delta.rows.push_back(sample_row(rng, 3));
	delta.rows.push_back(sample_row(rng, 63));
	delta.removed = { 0, 7, 40 };
	r_out.emplace_back("PlayerList (delta)", net::encode_player_list(delta));
	r_out.emplace_back("SpawnRequest", net::encode_spawn_request({ 2, 0 }));
	r_out.emplace_back("CombatEvents (v3)", net::encode_combat_events(sample_combat_v3(rng, 88888, 2, 3, 1, 2), kFrame));
	net::MatchInfo mi;
	mi.level = "MP_Subway";
	mi.mode = "TeamDeathMatch0";
	mi.logic_digest = 0xFEDCBA9876543210ull;
	mi.rules_flags = net::rules_flags::kTdm | net::rules_flags::kDeployScreen | net::rules_flags::kRevive;
	r_out.emplace_back("MatchInfo (v3)", net::encode_match_info(mi));
	net::Spawned sp{ 5, 18, sample_state(rng, 4100) };
	sp.kit = 1;
	sp.team = 1;
	sp.revived = true;
	r_out.emplace_back("Spawned (v3 revived)", net::encode_spawned(sp));
}

}

TEST_SUITE("net-proto") {
	TEST_CASE("proto v3: MatchState round trips every field; hostile bytes are refused") {
		Rng rng(331);
		for (int i = 0; i < 200; ++i) {
			const net::MatchState m = sample_match_state(rng);
			const std::vector<uint8_t> b = net::encode_match_state(m);
			REQUIRE(b.size() == 1 + 39);
			auto d = net::decode_match_state(b);
			REQUIRE(d);
			CHECK(same_match_state(m, *d));
		}
		net::MatchState m;
		CHECK(net::decode_match_state(net::encode_match_state(m))->restrict_mask == net::restrict_bits::kAll);

		m.phase = net::MatchPhase(9);
		m.winner = 7;
		m.kill_limit = -3;
		auto dm = net::decode_match_state(net::encode_match_state(m));
		REQUIRE(dm);
		CHECK(dm->phase == net::MatchPhase::RoundOver);
		CHECK(dm->winner == 2);
		CHECK(dm->kill_limit == 0);
		const auto state = [](uint32_t p_phase, uint32_t p_winner, int32_t p_limit) {
			return craft(net::MessageType::MatchState, [&](net::BitWriter &w) {
				w.write_bits(p_phase, 8);
				w.write_bits(0, 32);
				w.write_bits(0, 32);
				w.write_signed(0, 32);
				w.write_signed(0, 32);
				w.write_signed(p_limit, 32);
				w.write_bits(p_winner, 2);
				w.write_bool(true);
				for (int k = 0; k < 4; ++k) {
					w.write_bits(0, 32);
				}
				w.write_bits(0, 13);
			});
		};
		CHECK(net::decode_match_state(state(3, 2, 100)));
		CHECK_FALSE(net::decode_match_state(state(4, 0, 100)));
		CHECK_FALSE(net::decode_match_state(state(0, 3, 100)));
		CHECK_FALSE(net::decode_match_state(state(0, 0, -1)));
		std::vector<uint8_t> extra = state(1, 1, 5);
		extra.push_back(0);
		CHECK_FALSE(net::decode_match_state(extra));
	}

	TEST_CASE("proto v3: PlayerList round trips full and delta lists; the encoder canonicalises and counts") {
		Rng rng(332);
		net::PlayerList full;
		full.server_tick = 1234;
		full.full = true;
		for (uint8_t s = 0; s < net::kMaxPlayerRows; ++s) {
			full.rows.push_back(sample_row(rng, s));
		}
		size_t clamped = 99;
		const std::vector<uint8_t> b = net::encode_player_list(full, &clamped);
		CHECK(clamped == 0);
		auto d = net::decode_player_list(b);
		REQUIRE(d);
		CHECK(same_player_list(full, *d));
		MESSAGE("PlayerList: 64 rows ", b.size(), " B");
		CHECK(b.size() <= 1 + 5 + 64 * 29);
		CHECK(net::player_name(d->rows[3]) == "p3\xC3\xA9\xE2\x82\xAC");

		net::PlayerList messy;
		messy.server_tick = 99;
		messy.rows.push_back(sample_row(rng, 9));
		messy.rows.push_back(sample_row(rng, 2));
		messy.rows.push_back(sample_row(rng, 9));
		messy.rows.push_back(sample_row(rng, 64));
		messy.rows[1].team = 3;
		messy.rows[0].kit = 7;
		messy.rows[0].name = {};
		std::memcpy(messy.rows[0].name.data(), "abc\xE2\x82", 5);
		messy.rows[0].name_bytes = 5;
		messy.removed = { 30, 30, 2, 70 };
		auto dm = net::decode_player_list(net::encode_player_list(messy, &clamped));
		REQUIRE(dm);
		CHECK(clamped == 8);
		REQUIRE(dm->rows.size() == 2);
		CHECK(dm->rows[0].slot == 2);
		CHECK(dm->rows[0].team == 0);
		CHECK(dm->rows[1].slot == 9);
		CHECK(dm->rows[1].kit == 3);
		CHECK(net::player_name(dm->rows[1]) == "abc");
		CHECK(dm->removed == std::vector<uint8_t>{ 30 });

		net::PlayerList full_removed = full;
		full_removed.rows.resize(3);
		full_removed.removed = { 50 };
		auto df = net::decode_player_list(net::encode_player_list(full_removed, &clamped));
		REQUIRE(df);
		CHECK(clamped == 1);
		CHECK(df->removed.empty());

		net::PlayerList many = full;
		for (uint8_t s = 0; s < 10; ++s) {
			many.rows.push_back(sample_row(rng, s));
		}
		CHECK(net::decode_player_list(net::encode_player_list(many, &clamped))->rows.size() == net::kMaxPlayerRows);
		CHECK(clamped == 10);
	}

	TEST_CASE("proto v3: hostile PlayerList bytes are refused") {
		const auto list = [](bool p_full, uint32_t p_rows, uint32_t p_removed, auto &&p_body) {
			return craft(net::MessageType::PlayerList, [&](net::BitWriter &w) {
				w.write_bits(7, 32);
				w.write_bool(p_full);
				w.write_bits(p_rows, 7);
				w.write_bits(p_removed, 7);
				p_body(w);
			});
		};

		const auto row = [](net::BitWriter &w, uint32_t p_slot, uint32_t p_team, uint32_t p_name_bytes, std::string_view p_name) {
			w.write_bits(p_slot, 6);
			w.write_bits(p_team, 2);
			w.write_bits(1, 2);
			w.write_bool(false);
			w.write_bool(true);
			w.write_bits(p_name_bytes, 5);
			w.write_bits(30, 16);
			w.write_bits(1, 16);
			w.write_bits(2, 16);
			w.write_signed(-100, 32);
			for (const char ch : p_name) {
				w.write_bits(uint8_t(ch), 8);
			}
		};
		const auto none = [](net::BitWriter &) {};
		auto ok = net::decode_player_list(list(false, 2, 1, [&](net::BitWriter &w) {
			row(w, 1, 2, 3, "abc");
			row(w, 5, 0, 0, "");
			w.write_bits(9, 6);
		}));
		REQUIRE(ok);
		CHECK(ok->rows[0].score == -100);
		CHECK(net::player_name(ok->rows[0]) == "abc");
		CHECK(ok->removed == std::vector<uint8_t>{ 9 });
		CHECK(net::decode_player_list(list(true, 0, 0, none)));
		CHECK_FALSE(net::decode_player_list(list(false, 65, 0, none)));
		CHECK_FALSE(net::decode_player_list(list(false, 0, 65, none)));
		CHECK_FALSE(net::decode_player_list(list(false, 64, 0, none)));
		CHECK_FALSE(net::decode_player_list(list(true, 0, 1, [](net::BitWriter &w) { w.write_bits(3, 6); })));
		CHECK_FALSE(net::decode_player_list(list(false, 2, 0, [&](net::BitWriter &w) {
			row(w, 5, 1, 0, "");
			row(w, 5, 1, 0, "");
		})));
		CHECK_FALSE(net::decode_player_list(list(false, 2, 0, [&](net::BitWriter &w) {
			row(w, 6, 1, 0, "");
			row(w, 5, 1, 0, "");
		})));
		CHECK_FALSE(net::decode_player_list(list(false, 1, 0, [&](net::BitWriter &w) { row(w, 1, 3, 0, ""); })));
		CHECK_FALSE(net::decode_player_list(list(false, 1, 0, [&](net::BitWriter &w) { row(w, 1, 1, 17, "abcdefghijklmnopq"); })));
		CHECK_FALSE(net::decode_player_list(list(false, 1, 0, [&](net::BitWriter &w) { row(w, 1, 1, 2, "\xC3("); })));
		CHECK_FALSE(net::decode_player_list(list(false, 1, 0, [&](net::BitWriter &w) { row(w, 1, 1, 4, "ab"); })));
		CHECK_FALSE(net::decode_player_list(list(false, 1, 1, [&](net::BitWriter &w) {
			row(w, 4, 1, 0, "");
			w.write_bits(4, 6);
		})));
		CHECK_FALSE(net::decode_player_list(list(false, 0, 2, [](net::BitWriter &w) {
			w.write_bits(8, 6);
			w.write_bits(8, 6);
		})));
		std::vector<uint8_t> extra = list(false, 0, 0, none);
		extra.push_back(0);
		CHECK_FALSE(net::decode_player_list(extra));
	}

	TEST_CASE("proto v3: set_player_name keeps the longest valid UTF-8 prefix of at most 16 bytes") {
		net::PlayerRow row;
		CHECK_FALSE(net::set_player_name(row, "short"));
		CHECK(net::player_name(row) == "short");
		CHECK(net::set_player_name(row, "0123456789abcdefXYZ"));
		CHECK(net::player_name(row) == "0123456789abcdef");

		CHECK(net::set_player_name(row, "0123456789abcde\xC3\xA9"));
		CHECK(net::player_name(row) == "0123456789abcde");
		CHECK(row.name[15] == 0);
		CHECK(net::set_player_name(row, "ab\xFF"));
		CHECK(net::player_name(row) == "ab");
		CHECK_FALSE(net::set_player_name(row, ""));
		CHECK(row.name_bytes == 0);
	}

	TEST_CASE("proto v3: SpawnRequest round trips any value; the v3 fields of MatchInfo and Spawned") {
		for (const uint32_t kit : { 0u, 3u, 4u, 255u }) {
			auto d = net::decode_spawn_request(net::encode_spawn_request({ uint8_t(kit), uint8_t(kit ^ 1u) }));
			REQUIRE(d);
			CHECK(d->kit == kit);
			CHECK(d->spawn_choice == uint8_t(kit ^ 1u));
		}
		CHECK(net::encode_spawn_request({}).size() == 3);
		std::vector<uint8_t> short_request = net::encode_spawn_request({});
		short_request.pop_back();
		CHECK_FALSE(net::decode_spawn_request(short_request));
		net::MatchInfo mi;
		mi.logic_digest = 0x0102030405060708ull;
		mi.rules_flags = net::rules_flags::kTdm | net::rules_flags::kRevive;
		auto dmi = net::decode_match_info(net::encode_match_info(mi));
		REQUIRE(dmi);
		CHECK(dmi->logic_digest == mi.logic_digest);
		CHECK(dmi->rules_flags == mi.rules_flags);
		mi.rules_flags = 1u << 3;
		CHECK_FALSE(net::decode_match_info(net::encode_match_info(mi)));
		Rng rng(333);
		for (const bool revived : { false, true }) {
			net::Spawned sp{ 7, 3, sample_state(rng, 500) };
			sp.revived = revived;
			auto d = net::decode_spawned(net::encode_spawned(sp));
			REQUIRE(d);
			CHECK(d->revived == revived);
		}
	}

	TEST_CASE("proto v3: CombatEvents carries the death stamp and the revives; an unstamped death costs one bit") {
		Rng rng(334);
		const sim::Tick tick = 7000;
		const net::CombatEvents m = sample_combat_v3(rng, tick, 3, 4, 1, 5);
		size_t clamped = 99;
		auto d = net::decode_combat_events(net::encode_combat_events(m, kFrame, &clamped), kFrame);
		REQUIRE(d);
		CHECK(clamped == 0);
		REQUIRE(d->deaths.size() == 4);
		REQUIRE(d->revives.size() == 5);
		for (size_t i = 0; i < 4; ++i) {
			CHECK(d->deaths[i].respawn_tick == m.deaths[i].respawn_tick);
			CHECK(d->deaths[i].revive_until == m.deaths[i].revive_until);
			CHECK(std::fabs(d->deaths[i].killer_health - m.deaths[i].killer_health) <= 0.005f + 1e-4f);
		}
		for (size_t i = 0; i < 5; ++i) {
			CHECK(d->revives[i].reviver == m.revives[i].reviver);
			CHECK(d->revives[i].revived == m.revives[i].revived);
			CHECK(d->revives[i].tick == m.revives[i].tick);
		}
		const size_t empty = net::encode_combat_events(sample_combat_v3(rng, tick, 0, 0, 0, 0), kFrame).size();
		const size_t one_rev = net::encode_combat_events(sample_combat_v3(rng, tick, 0, 0, 0, 1), kFrame).size();
		net::CombatEvents stamped = sample_combat_v3(rng, tick, 0, 1, 0, 0);
		net::CombatEvents plain = stamped;
		plain.deaths[0].respawn_tick = 0;
		plain.deaths[0].revive_until = 0;
		plain.deaths[0].killer_health = 0;
		const size_t stamped_size = net::encode_combat_events(stamped, kFrame).size();
		const size_t plain_size = net::encode_combat_events(plain, kFrame).size();
		MESSAGE("CombatEvents v3: empty ", empty, " B; one revive +", one_rev - empty, " B; a stamped death ", stamped_size, " B, unstamped ",
				plain_size, " B");
		CHECK(empty == 1 + 4 + 4);
		CHECK(one_rev - empty <= 3);
		CHECK(stamped_size - plain_size == 11);

		net::CombatEvents many = sample_combat_v3(rng, tick, 250, 0, 0, 20);
		auto dmany = net::decode_combat_events(net::encode_combat_events(many, kFrame, &clamped), kFrame);
		REQUIRE(dmany);
		CHECK(dmany->revives.size() == net::kMaxCombatEventsPerMessage - 250);
		CHECK(clamped == 14);
		net::CombatEvents late = sample_combat_v3(rng, tick, 0, 0, 0, 2);
		late.revives[0].tick = tick + 5;
		late.revives[1].revived = 200;
		auto dlate = net::decode_combat_events(net::encode_combat_events(late, kFrame, &clamped), kFrame);
		REQUIRE(dlate);
		CHECK(clamped == 2);
		REQUIRE(dlate->revives.size() == 1);
		CHECK(dlate->revives[0].tick == tick);
		net::CombatEvents hot = sample_combat_v3(rng, tick, 0, 1, 0, 0);
		hot.deaths[0].killer_health = 1e9f;
		auto dhot = net::decode_combat_events(net::encode_combat_events(hot, kFrame, &clamped), kFrame);
		REQUIRE(dhot);
		CHECK(dhot->deaths[0].killer_health == 100000.0f);
		CHECK(clamped == 1);

		const auto message = [&](auto &&p_body) {
			return craft(net::MessageType::CombatEvents, [&](net::BitWriter &w) {
				w.write_bits(tick, 32);
				p_body(w);
			});
		};
		const auto revive = [](net::BitWriter &w, uint64_t p_back) {
			w.write_bits(1, 7);
			w.write_bits(2, 7);
			w.write_varint(p_back);
		};
		const auto counts = [](net::BitWriter &w, uint64_t p_deaths, uint64_t p_revives) {
			w.write_varint(0);
			w.write_varint(p_deaths);
			w.write_varint(0);
			w.write_varint(p_revives);
		};
		CHECK(net::decode_combat_events(message([&](net::BitWriter &w) {
			counts(w, 0, 1);
			revive(w, tick);
		}), kFrame));
		CHECK_FALSE(net::decode_combat_events(message([&](net::BitWriter &w) {
			counts(w, 0, 1);
			revive(w, tick + 1);
		}), kFrame));
		CHECK_FALSE(net::decode_combat_events(message([&](net::BitWriter &w) { counts(w, 0, 257); }), kFrame));
		CHECK_FALSE(net::decode_combat_events(message([&](net::BitWriter &w) { counts(w, 0, ~0ull); }), kFrame));
		CHECK_FALSE(net::decode_combat_events(message([&](net::BitWriter &w) { counts(w, 0, 40); }), kFrame));
		const auto death = [&](net::BitWriter &w, uint32_t p_killer_hp) {
			w.write_bits(1, 7);
			w.write_bits(2, 7);
			w.write_bits(3, 8);
			w.write_bits(1, 2);
			w.write_bits(77, 32);
			w.write_bool(false);
			w.write_varint(1);
			w.write_bits(0, 16);
			w.write_bool(true);
			w.write_bits(tick + 600, 32);
			w.write_bits(tick + 300, 32);
			w.write_bits(p_killer_hp, 24);
		};
		auto dd = net::decode_combat_events(message([&](net::BitWriter &w) {
			counts(w, 1, 0);
			death(w, 10'000'000);
		}), kFrame);
		REQUIRE(dd);
		CHECK(dd->deaths[0].respawn_tick == tick + 600);
		CHECK(dd->deaths[0].killer_health == 100000.0f);
		CHECK_FALSE(net::decode_combat_events(message([&](net::BitWriter &w) {
			counts(w, 1, 0);
			death(w, 10'000'001);
		}), kFrame));
	}
}

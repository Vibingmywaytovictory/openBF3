#include <doctest/doctest.h>

#include "bf3/core/util/splitmix64.h"
#include "bf3/net/bit_stream.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace net = bf3::net;
namespace sim = bf3::sim;
namespace sw = bf3::sim::weapons;

namespace {

constexpr size_t kMutations = 600000;
constexpr size_t kRandoms = 300000;

struct Rng {
	uint64_t s;
	explicit Rng(uint64_t p_seed) :
			s(p_seed) {}
	uint64_t next() { return bf3::core::splitmix64_next(s); }
	uint32_t below(uint32_t p_n) { return p_n == 0 ? 0 : uint32_t(next() % p_n); }
	float uniform(float p_lo, float p_hi) { return p_lo + (p_hi - p_lo) * float(double(next() >> 11) / 9007199254740992.0); }
};

const net::ShotQuantization kFrame{ { -2048.0f, -512.0f, -2048.0f } };

constexpr std::array<net::MessageType, 18> kTypes = {
	net::MessageType::Snapshot,
	net::MessageType::CommandPacket,
	net::MessageType::SnapshotOthers,
	net::MessageType::ShotEvents,
	net::MessageType::Hello,
	net::MessageType::MatchInfo,
	net::MessageType::Ready,
	net::MessageType::Spawned,
	net::MessageType::Loading,
	net::MessageType::Kick,
	net::MessageType::Bye,
	net::MessageType::CombatEvents,
	net::MessageType::KitSelect,
	net::MessageType::Debug,
	net::MessageType::DebugReport,
	net::MessageType::MatchState,
	net::MessageType::PlayerList,
	net::MessageType::SpawnRequest,
};

const char *type_name(net::MessageType p_type) {
	switch (p_type) {
		case net::MessageType::Snapshot:
			return "Snapshot";
		case net::MessageType::CommandPacket:
			return "CommandPacket";
		case net::MessageType::SnapshotOthers:
			return "SnapshotOthers";
		case net::MessageType::ShotEvents:
			return "ShotEvents";
		case net::MessageType::Hello:
			return "Hello";
		case net::MessageType::MatchInfo:
			return "MatchInfo";
		case net::MessageType::Ready:
			return "Ready";
		case net::MessageType::Spawned:
			return "Spawned";
		case net::MessageType::Loading:
			return "Loading";
		case net::MessageType::Kick:
			return "Kick";
		case net::MessageType::Bye:
			return "Bye";
		case net::MessageType::CombatEvents:
			return "CombatEvents";
		case net::MessageType::KitSelect:
			return "KitSelect";
		case net::MessageType::Debug:
			return "Debug";
		case net::MessageType::DebugReport:
			return "DebugReport";
		case net::MessageType::MatchState:
			return "MatchState";
		case net::MessageType::PlayerList:
			return "PlayerList";
		case net::MessageType::SpawnRequest:
			return "SpawnRequest";
	}
	return "?";
}

size_t type_index(net::MessageType p_type) {
	for (size_t i = 0; i < kTypes.size(); ++i) {
		if (kTypes[i] == p_type) {
			return i;
		}
	}
	return kTypes.size();
}

enum class Verdict { Refused, Stable, Unstable };

template <class Decode, class Encode>
Verdict check(std::span<const uint8_t> p_bytes, Decode &&p_decode, Encode &&p_encode) {
	auto a = p_decode(p_bytes);
	if (!a) {
		return Verdict::Refused;
	}
	bool lossless = true;
	const std::vector<uint8_t> e1 = p_encode(*a, lossless);
	auto b = p_decode(e1);
	if (!b || !lossless) {
		return Verdict::Unstable;
	}
	bool again = true;
	const std::vector<uint8_t> e2 = p_encode(*b, again);
	return e1 == e2 && again ? Verdict::Stable : Verdict::Unstable;
}

Verdict decode_as(net::MessageType p_type, std::span<const uint8_t> p_bytes) {
	switch (p_type) {
		case net::MessageType::Snapshot:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_snapshot(p); },
					[&](const net::Snapshot &p_m, bool &r_lossless) {
						size_t dropped = 0;
						std::vector<uint8_t> e = net::encode_snapshot(p_m, &dropped);

						r_lossless = dropped == 0 || p_bytes.size() > net::kMaxDatagramBytes;
						return e;
					});
		case net::MessageType::CommandPacket:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_command_packet(p); },
					[](const net::CommandPacket &p_m, bool &) { return net::encode_command_packet(p_m); });
		case net::MessageType::SnapshotOthers:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_snapshot_others(p); },
					[](const net::SnapshotOthers &p_m, bool &r_lossless) {
						size_t dropped = 0;
						std::vector<uint8_t> e = net::encode_snapshot_others(p_m, &dropped);
						r_lossless = dropped == 0;
						return e;
					});
		case net::MessageType::ShotEvents:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_shot_events(p, kFrame); },
					[](const net::ShotEvents &p_m, bool &r_lossless) {
						size_t dropped = 0;
						std::vector<uint8_t> e = net::encode_shot_events(p_m, kFrame, { 3.0f, 60.0f, -7.0f }, &dropped);
						r_lossless = dropped == 0;
						return e;
					});
		case net::MessageType::Hello:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_hello(p); },
					[](const net::Hello &p_m, bool &) { return net::encode_hello(p_m); });
		case net::MessageType::MatchInfo:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_match_info(p); },
					[](const net::MatchInfo &p_m, bool &) { return net::encode_match_info(p_m); });
		case net::MessageType::Ready:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_ready(p); },
					[](const net::Ready &p_m, bool &) { return net::encode_ready(p_m); });
		case net::MessageType::Spawned:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_spawned(p); },
					[](const net::Spawned &p_m, bool &) { return net::encode_spawned(p_m); });
		case net::MessageType::Loading:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_loading(p); },
					[](const net::Loading &p_m, bool &) { return net::encode_loading(p_m); });
		case net::MessageType::Kick:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_kick(p); },
					[](const net::Kick &p_m, bool &) { return net::encode_kick(p_m); });
		case net::MessageType::Bye:

			return p_bytes.size() == 1 && p_bytes[0] == uint8_t(net::MessageType::Bye) ? Verdict::Stable : Verdict::Refused;
		case net::MessageType::CombatEvents:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_combat_events(p, kFrame); },
					[](const net::CombatEvents &p_m, bool &r_lossless) {
						size_t clamped = 0;
						std::vector<uint8_t> e = net::encode_combat_events(p_m, kFrame, &clamped);
						r_lossless = clamped == 0;
						return e;
					});
		case net::MessageType::KitSelect:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_kit_select(p); },
					[](const net::KitSelect &p_m, bool &) { return net::encode_kit_select(p_m); });
		case net::MessageType::Debug:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_debug(p); },
					[](const net::DebugRequest &p_m, bool &) { return net::encode_debug(p_m); });
		case net::MessageType::DebugReport:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_debug_report(p); },
					[](const net::DebugReport &p_m, bool &) { return net::encode_debug_report(p_m); });
		case net::MessageType::MatchState:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_match_state(p); },
					[](const net::MatchState &p_m, bool &) { return net::encode_match_state(p_m); });
		case net::MessageType::PlayerList:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_player_list(p); },
					[](const net::PlayerList &p_m, bool &r_lossless) {
						size_t clamped = 0;
						std::vector<uint8_t> e = net::encode_player_list(p_m, &clamped);
						r_lossless = clamped == 0;
						return e;
					});
		case net::MessageType::SpawnRequest:
			return check(p_bytes, [](std::span<const uint8_t> p) { return net::decode_spawn_request(p); },
					[](const net::SpawnRequest &p_m, bool &) { return net::encode_spawn_request(p_m); });
	}
	return Verdict::Refused;
}

sim::Vec3 unit_vector(Rng &r_rng) {
	for (;;) {
		const float x = r_rng.uniform(-1, 1), y = r_rng.uniform(-1, 1), z = r_rng.uniform(-1, 1);
		const float n = std::sqrt(x * x + y * y + z * z);
		if (n > 0.1f && n <= 1.0f) {
			return { x / n, y / n, z / n };
		}
	}
}

sim::SoldierState sample_state(Rng &r_rng, sim::Tick p_tick) {
	sim::SoldierState s;
	s.tick = p_tick;
	s.life = r_rng.below(1000) + 1;
	s.position = { r_rng.uniform(-300, 300), r_rng.uniform(50, 80), r_rng.uniform(-300, 300) };
	s.velocity = { r_rng.uniform(-7, 7), r_rng.uniform(-20, 4), r_rng.uniform(-7, 7) };
	s.ground_normal = { 0.0f, 1.0f, 0.0f };
	s.yaw = uint16_t(r_rng.next());
	s.pitch = int16_t(int32_t(r_rng.below(2 * 8191 + 1)) - 8191);
	s.pose = sim::Pose(r_rng.below(3));
	s.target_pose = sim::Pose(r_rng.below(3));
	s.mode = sim::MoveMode(r_rng.below(uint32_t(sim::kMoveModeCount)));
	s.flags = uint16_t(r_rng.below(64));
	s.health = r_rng.uniform(0, 100);
	return s;
}

net::WeaponsWire sample_weapons(Rng &r_rng) {
	net::WeaponsWire w;
	w.next_shot_index = uint32_t(r_rng.next());
	w.active = 1;
	w.pending = 6;
	w.events = uint32_t(r_rng.below(1u << 16));
	w.sprint_recover_until = net::kWeaponsWireTimeZero;
	for (uint8_t i : { uint8_t(0), uint8_t(1), uint8_t(6), uint8_t(7) }) {
		w.slots[i].def = uint16_t(r_rng.below(40));
		w.slots[i].magazine = int32_t(r_rng.below(31));
		w.slots[i].reserve = int32_t(r_rng.below(200));
		net::WeaponTimersWire &t = i == 1 ? w.timers[0] : i == 6 ? w.timers[1] : w.idle[i];
		t.slot = i;
		t.phase = sw::WeaponPhase(r_rng.below(8));
		t.phase_start = -int32_t(r_rng.below(1000000));
		t.phase_end = int32_t(r_rng.below(1000000));
		t.next_shot_time = net::kWeaponsWireTimeZero;
		t.spread_time = -int32_t(r_rng.below(100000));
		t.hold_start = net::kWeaponsWireTimeZero;
		t.spread = r_rng.uniform(0, 4);
		t.recoil = { r_rng.uniform(-1, 1), r_rng.uniform(-1, 1) };
	}
	return w;
}

net::OtherSoldier sample_other(Rng &r_rng, uint8_t p_slot, sim::Tick p_tick) {
	net::OtherSoldier o = net::quantize_other(p_slot, sample_state(r_rng, p_tick));
	net::set_other_weapon(o, uint16_t(r_rng.below(260)), sw::WeaponPhase(r_rng.below(8)), (r_rng.next() & 1) != 0, (r_rng.next() & 1) != 0,
			uint32_t(r_rng.next()), uint32_t(r_rng.next()));
	return o;
}

net::RemoteShot sample_shot(Rng &r_rng, uint8_t p_slot, uint32_t p_index, sim::Tick p_tick) {
	net::RemoteShot s;
	s.slot = p_slot;
	s.shot_index = p_index;
	s.def = uint16_t(r_rng.below(net::kMaxWireDefs));
	s.time = sw::tick_start(p_tick - 1) + int64_t(r_rng.next() % uint64_t(2 * sw::kTimeUnitsPerTick));
	s.origin = { r_rng.uniform(-300, 300), r_rng.uniform(-50, 120), r_rng.uniform(-300, 300) };
	s.direction = unit_vector(r_rng);
	s.pellets = r_rng.below(3) == 0 ? uint8_t(1 + r_rng.below(64)) : uint8_t(1);
	s.tracer = r_rng.below(2) == 0;
	if (s.tracer) {
		s.aim = unit_vector(r_rng);
	}
	if (s.pellets > 1) {
		s.pellet_cone = r_rng.uniform(0, 90);
	}
	if (r_rng.below(3) == 0) {
		s.power = r_rng.uniform(0, 10);
	}
	return s;
}

struct Sample {
	net::MessageType type;
	std::string name;
	std::vector<uint8_t> bytes;
};

std::vector<Sample> samples() {
	Rng rng(0x5EED);
	std::vector<Sample> out;
	const sim::Tick tick = 54321;
	{
		net::Snapshot s;
		s.seq = 777;
		s.server_tick = tick;
		s.applied_sequence = 999;
		s.own = sample_state(rng, tick);
		s.own_weapons = sample_weapons(rng);
		for (uint8_t i = 1; i <= 10; ++i) {
			s.others.push_back(sample_other(rng, i, tick));
		}
		out.push_back({ net::MessageType::Snapshot, "Snapshot", net::encode_snapshot(s) });
		s.own.reset();
		s.own_weapons.reset();
		out.push_back({ net::MessageType::Snapshot, "Snapshot (no own)", net::encode_snapshot(s) });
	}
	{
		net::CommandPacket p;
		p.newest_snapshot = 777;
		p.snapshot_ack_bits = 0xF0F0F0F0u;
		p.others_ack_bits = 0x0F0F0F0Fu;
		p.count = 4;
		for (uint32_t i = 0; i < 4; ++i) {
			sim::Command &c = p.commands[i];
			c.tick = 5000 - i;
			c.sequence = 900 - i;
			c.buttons = uint32_t(rng.next()) & sim::buttons::kWireMask;
			c.yaw = uint16_t(rng.next());
			c.view_back_q8 = uint16_t(rng.next());
			c.view_quality = uint8_t(rng.below(4));
			c.view_seq8 = uint8_t(rng.next());
		}
		p.commands[2] = p.commands[1];
		p.commands[2].tick = 4998;
		p.commands[2].sequence = 898;
		out.push_back({ net::MessageType::CommandPacket, "CommandPacket", net::encode_command_packet(p) });
	}
	{
		net::SnapshotOthers so;
		so.seq = 778;
		so.server_tick = tick;
		for (uint8_t i = 50; i < 70; ++i) {
			so.others.push_back(sample_other(rng, i, tick));
		}
		out.push_back({ net::MessageType::SnapshotOthers, "SnapshotOthers", net::encode_snapshot_others(so) });
	}
	{
		net::ShotEvents se;
		se.server_tick = tick;
		for (uint32_t i = 0; i < 24; ++i) {
			se.shots.push_back(sample_shot(rng, uint8_t(1 + i % 5), 100 + i * (i % 7 == 6 ? 30 : 1), tick));
		}
		out.push_back({ net::MessageType::ShotEvents, "ShotEvents", net::encode_shot_events(se, kFrame, {}) });
	}
	out.push_back({ net::MessageType::Hello, "Hello", net::encode_hello({ net::kProtocolVersion, "fuzz\xC3\xA9" }) });
	{
		net::MatchInfo mi;
		mi.level = "MP_Subway";
		mi.mode = "TeamDeathMatch0";
		mi.slot = 5;
		mi.server_tick = tick;
		mi.source_digest = bf3::core::Sha256Digest{};
		mi.motor_hash = rng.next();
		mi.match_seed = rng.next();
		mi.weapon_hash = rng.next();
		mi.weapon_flags = net::weapon_flags::kInfiniteMagazine;
		mi.kit_hash = rng.next();
		mi.max_rewind_ticks = 21;
		out.push_back({ net::MessageType::MatchInfo, "MatchInfo", net::encode_match_info(mi) });
		mi.logic_digest = rng.next();
		mi.rules_flags = net::rules_flags::kTdm | net::rules_flags::kDeployScreen | net::rules_flags::kRevive;
		out.push_back({ net::MessageType::MatchInfo, "MatchInfo (v3 rules)", net::encode_match_info(mi) });
		out.push_back({ net::MessageType::Ready, "Ready", net::encode_ready({ mi.source_digest }) });
	}
	{
		net::Spawned sp{ 5, 17, sample_state(rng, tick) };
		sp.kit = 3;
		sp.team = 2;
		out.push_back({ net::MessageType::Spawned, "Spawned", net::encode_spawned(sp) });
		sp.revived = true;
		out.push_back({ net::MessageType::Spawned, "Spawned (revived)", net::encode_spawned(sp) });
	}
	out.push_back({ net::MessageType::Loading, "Loading", net::encode_loading({ 4, 500 }) });
	out.push_back({ net::MessageType::Kick, "Kick", net::encode_kick({ "weapon table mismatch" }) });
	out.push_back({ net::MessageType::Bye, "Bye", net::encode_bye() });
	{
		net::CombatEvents c;
		c.server_tick = tick;
		for (uint32_t i = 0; i < 6; ++i) {
			net::HitEvent h;
			h.victim = uint8_t(rng.below(128));
			h.shooter = uint8_t(rng.below(128));
			h.shot_index = uint32_t(rng.next());
			h.pellet = uint8_t(rng.below(64));
			h.zone = sw::HitZone(rng.below(4));
			h.damage = rng.uniform(0, 100);
			h.health_after = rng.uniform(0, 100);
			h.position = { rng.uniform(-100, 100), rng.uniform(0, 100), rng.uniform(-100, 100) };
			h.normal = unit_vector(rng);
			h.time = sw::tick_start(tick - rng.below(3)) + 77;
			c.hits.push_back(h);
		}
		net::DeathEvent d;
		d.victim = 3;
		d.killer = 3;
		d.time = sw::tick_start(tick);
		c.deaths.push_back(d);
		d.def = 7;
		d.killer = 9;
		d.headshot = true;
		c.deaths.push_back(d);
		net::DetonationWire det;
		det.shooter = 9;
		det.shot_index = 1234;
		det.def = 12;
		det.time = sw::tick_start(tick) + 5;
		det.position = { 1, 2, 3 };
		det.normal = { 0, 1, 0 };
		c.detonations.push_back(det);
		out.push_back({ net::MessageType::CombatEvents, "CombatEvents", net::encode_combat_events(c, kFrame) });
		c.deaths[1].respawn_tick = tick + 600;
		c.deaths[1].revive_until = tick + 300;
		c.deaths[1].killer_health = 42.5f;
		c.revives.push_back({ 9, 3, tick });
		c.revives.push_back({ 11, 12, tick - 2 });
		out.push_back({ net::MessageType::CombatEvents, "CombatEvents (v3)", net::encode_combat_events(c, kFrame) });
	}
	out.push_back({ net::MessageType::KitSelect, "KitSelect", net::encode_kit_select({ 2, 1 }) });
	{
		net::DebugRequest dr;
		dr.kind = net::DebugKind::SpawnTarget;
		dr.id = 9;
		dr.position = { 1.5f, 70.25f, -20.0f };
		dr.yaw = 16384;
		dr.slot = 33;
		dr.pose = sim::Pose::Crouch;
		out.push_back({ net::MessageType::Debug, "Debug", net::encode_debug(dr) });
	}
	{
		net::DebugReport rep;
		rep.id = 9;
		rep.ok = true;
		rep.text = "trace";
		rep.sweep.push_back({ 0, true, { 1, 2, 3 }, { 1, 2.01f, 3 }, 0.01f, 0.0f, 0.02f, true, false });
		net::TargetInfoRow row;
		row.slot = 4;
		row.target.capsule_count = 2;
		row.target.bound_radius = 1;
		row.target.capsules[1].radius = 0.2f;
		rep.targets.push_back(row);
		for (uint32_t i = 0; i < 3; ++i) {
			rep.trace.push_back(sample_state(rng, 7000 + i));
			net::TraceGunplayTick g;
			g.tick = 7000 + i;
			sw::ShotEvent s;
			s.shot_index = i;
			s.player_slot = 5;
			s.direction = { 0, 0, 1 };
			g.shots.push_back(s);
			g.projectiles.push_back({ 1, 2, 3 });
			g.projectile_shots.push_back(i);
			sw::DamageEvent dmg;
			dmg.amount = 20;
			g.damage.push_back(dmg);
			g.detonations.push_back({});
			g.deaths.push_back({ 1, 5, i });
			rep.trace_gunplay.push_back(g);
		}
		rep.trace_dropped = 3;
		out.push_back({ net::MessageType::DebugReport, "DebugReport", net::encode_debug_report(rep) });
	}

	{
		net::MatchState m;
		m.phase = net::MatchPhase::InRound;
		m.round_start_tick = tick - 600;
		m.kills = { 17, 23 };
		m.kill_limit = 100;
		m.unspawn_tick = tick + 9000;
		m.restrict_until_tick = tick - 300;
		m.restrict_mask = net::restrict_bits::kYaw | net::restrict_bits::kPitch | net::restrict_bits::kJump;
		m.players_needed = 3;
		out.push_back({ net::MessageType::MatchState, "MatchState", net::encode_match_state(m) });
		net::PlayerList pl;
		pl.server_tick = tick;
		pl.full = true;
		for (uint8_t s = 0; s < 20; ++s) {
			net::PlayerRow row;
			row.slot = uint8_t(s * 3);
			row.team = uint8_t(1 + s % 2);
			row.kit = uint8_t(s % 4);
			row.bot = s % 5 != 0;
			row.alive = s % 3 != 0;
			row.ping_ms = uint16_t(rng.below(400));
			row.kills = uint16_t(rng.below(50));
			row.deaths = uint16_t(rng.below(50));
			row.score = int32_t(rng.below(5000)) - 100;
			net::set_player_name(row, s % 4 == 0 ? std::string("fuzz\xC3\xA9") + std::to_string(s) : "bot" + std::to_string(s));
			pl.rows.push_back(row);
		}
		out.push_back({ net::MessageType::PlayerList, "PlayerList (full)", net::encode_player_list(pl) });
		pl.full = false;
		pl.rows.resize(3);
		pl.removed = { 1, 2, 63 };
		out.push_back({ net::MessageType::PlayerList, "PlayerList (delta)", net::encode_player_list(pl) });
		out.push_back({ net::MessageType::SpawnRequest, "SpawnRequest", net::encode_spawn_request({ 1, 0 }) });
	}	return out;
}

struct Tally {
	size_t stable = 0;
	size_t refused = 0;
	size_t unstable = 0;
};

}

TEST_SUITE("net-proto-fuzz") {
	TEST_CASE("net.proto.fuzz: every sample of every type decodes and is stable") {
		const std::vector<Sample> all = samples();
		std::array<bool, kTypes.size()> covered{};
		for (const Sample &s : all) {
			INFO(s.name);
			REQUIRE_FALSE(s.bytes.empty());
			REQUIRE(net::peek_type(s.bytes) == s.type);
			CHECK(decode_as(s.type, s.bytes) == Verdict::Stable);
			covered[type_index(s.type)] = true;
			std::printf("net.proto.fuzz: sample %-18s %6zu bytes\n", s.name.c_str(), s.bytes.size());
		}
		for (size_t i = 0; i < kTypes.size(); ++i) {
			INFO(type_name(kTypes[i]));
			CHECK(covered[i]);
		}
	}

	TEST_CASE("net.proto.fuzz: the structured malformed cases are all refused and counted") {
		const std::vector<Sample> all = samples();
		size_t cases = 0, refused = 0;
		for (const Sample &s : all) {
			INFO(s.name);
			size_t here = 0, here_refused = 0;

			for (size_t n = 0; n < s.bytes.size(); ++n) {
				++here;
				here_refused += decode_as(s.type, std::span<const uint8_t>(s.bytes.data(), n)) == Verdict::Refused ? 1 : 0;
			}

			for (const uint8_t extra : { uint8_t(0), uint8_t(0x5A), uint8_t(0xFF) }) {
				std::vector<uint8_t> longer = s.bytes;
				longer.push_back(extra);
				++here;
				here_refused += decode_as(s.type, longer) == Verdict::Refused ? 1 : 0;
			}

			for (const net::MessageType other : kTypes) {
				if (other != s.type) {
					++here;
					here_refused += decode_as(other, s.bytes) == Verdict::Refused ? 1 : 0;
				}
			}
			std::printf("net.proto.fuzz: malformed %-18s %5zu / %5zu refused\n", s.name.c_str(), here_refused, here);
			CHECK(here_refused == here);
			cases += here;
			refused += here_refused;
		}
		std::printf("net.proto.fuzz: structured malformed cases: %zu / %zu refused\n", refused, cases);
		CHECK(refused == cases);
	}

	TEST_CASE("net.proto.fuzz: seeded mutations and random bytes never crash; whatever decodes is stable") {
		const std::vector<Sample> all = samples();
		std::array<Tally, kTypes.size()> tally{};
		Rng rng(0xF0225EEDull);
		const auto run = [&](const std::vector<uint8_t> &p_bytes) {
			const std::optional<net::MessageType> type = net::peek_type(p_bytes);
			if (!type) {
				return;
			}
			Tally &t = tally[type_index(*type)];
			switch (decode_as(*type, p_bytes)) {
				case Verdict::Stable:
					++t.stable;
					break;
				case Verdict::Refused:
					++t.refused;
					break;
				case Verdict::Unstable:
					++t.unstable;
					if (t.unstable <= 3) {
						std::string hex;
						for (size_t i = 0; i < std::min<size_t>(p_bytes.size(), 64); ++i) {
							char b[4];
							std::snprintf(b, sizeof(b), "%02X", p_bytes[i]);
							hex += b;
						}
						std::printf("net.proto.fuzz: UNSTABLE %s (%zu bytes): %s\n", type_name(*type), p_bytes.size(), hex.c_str());
					}
					break;
			}
		};
		for (size_t i = 0; i < kMutations; ++i) {
			const Sample &s = all[rng.below(uint32_t(all.size()))];
			std::vector<uint8_t> b = s.bytes;
			const uint32_t edits = 1 + rng.below(6);
			for (uint32_t e = 0; e < edits && !b.empty(); ++e) {
				switch (rng.below(6)) {
					case 0:
					case 1:
						b[rng.below(uint32_t(b.size()))] ^= uint8_t(1u << rng.below(8));
						break;
					case 2:
						b[rng.below(uint32_t(b.size()))] = uint8_t(rng.next());
						break;
					case 3:
						b.resize(rng.below(uint32_t(b.size()) + 1));
						break;
					case 4:
						b.insert(b.begin() + rng.below(uint32_t(b.size()) + 1), uint8_t(rng.next()));
						break;
					default: {

						const Sample &o = all[rng.below(uint32_t(all.size()))];
						const size_t head = rng.below(uint32_t(b.size()) + 1);
						const size_t from = rng.below(uint32_t(o.bytes.size()) + 1);
						b.resize(std::max<size_t>(head, 1));
						b.insert(b.end(), o.bytes.begin() + std::ptrdiff_t(from), o.bytes.end());
						break;
					}
				}
			}
			run(b);
		}
		for (size_t i = 0; i < kRandoms; ++i) {
			const net::MessageType type = kTypes[rng.below(uint32_t(kTypes.size()))];
			const bool reliable = uint8_t(type) >= 16;
			std::vector<uint8_t> b(1 + rng.below(reliable && rng.below(8) == 0 ? 8192 : 1400));
			for (uint8_t &x : b) {
				x = uint8_t(rng.next());
			}
			b[0] = uint8_t(type);
			run(b);
			if (rng.below(4) == 0) {
				b[0] = uint8_t(rng.next());
				run(b);
			}
		}
		size_t stable = 0, refused = 0, unstable = 0;
		for (size_t i = 0; i < kTypes.size(); ++i) {
			std::printf("net.proto.fuzz: %-15s %7zu decoded+stable %7zu refused %3zu unstable\n", type_name(kTypes[i]), tally[i].stable, tally[i].refused,
					tally[i].unstable);
			stable += tally[i].stable;
			refused += tally[i].refused;
			unstable += tally[i].unstable;
			INFO(type_name(kTypes[i]));
			CHECK(tally[i].unstable == 0);
			CHECK(tally[i].refused > 0);
		}
		std::printf("net.proto.fuzz: total %zu decoded+stable, %zu refused, %zu unstable\n", stable, refused, unstable);
		CHECK(unstable == 0);
		CHECK(stable > 0);
	}
}

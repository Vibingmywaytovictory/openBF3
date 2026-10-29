#include <doctest/doctest.h>

#include "bf3/anim/server_pose.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/fx/atlas_pixels.h"
#include "bf3/fx/render_prep.h"
#include "bf3/host/client_projectiles.h"
#include "bf3/host/client_session.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/kits.h"
#include "bf3/host/lag_comp.h"
#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/host/server_view.h"
#include "bf3/host/target_bots.h"
#include "bf3/net/bit_stream.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/sim/command.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/mechanics_tuning.h"
#include "bf3/sim/weapons/posed_target.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rules.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include "gunplay_fixture.h"
#include "test_support.h"
#include "weapons_support.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

using namespace bf3;
namespace sw = bf3::sim::weapons;
namespace tw = bf3::test::weapons;
namespace tg = bf3::test::gunplay;

namespace {

constexpr bool kSvRulesLanded = true;

constexpr bool kNtLanded = true;

void pending(const char *p_key, const char *p_row, const char *p_reason) {
	std::printf("m2a.contract: PENDING (%s): %s -- %s\n", p_key, p_row, p_reason);
	const std::string text = std::string("PENDING (") + p_key + "): " + p_row;
	MESSAGE(text);
}

static_assert(sizeof(sim::Command) == 24, "Command: 24 B with the view sample (m2a.md §6.2)");
static_assert(offsetof(sim::Command, tick) == 0);
static_assert(offsetof(sim::Command, sequence) == 4);
static_assert(offsetof(sim::Command, buttons) == 8);
static_assert(offsetof(sim::Command, move_forward) == 12);
static_assert(offsetof(sim::Command, move_right) == 13);
static_assert(offsetof(sim::Command, yaw) == 14);
static_assert(offsetof(sim::Command, pitch) == 16);
static_assert(offsetof(sim::Command, pose) == 18);
static_assert(offsetof(sim::Command, flags) == 19);
static_assert(offsetof(sim::Command, view_back_q8) == 20);
static_assert(offsetof(sim::Command, view_quality) == 22);
static_assert(offsetof(sim::Command, view_seq8) == 23);
static_assert(std::is_trivially_copyable_v<sim::Command>);

static_assert(sizeof(host::ShotView) == 16);
static_assert(sizeof(host::PresentEvent) == 64);
static_assert(sizeof(net::OtherPose) == 60);
static_assert(sizeof(core::gameplay::CapsuleLocal) == 32);
static_assert(sizeof(fx::FxInstance) == 80);
static_assert(std::is_trivially_copyable_v<host::ShotView>);
static_assert(std::is_trivially_copyable_v<host::PresentEvent>);
static_assert(std::is_trivially_copyable_v<net::OtherPose>);
static_assert(std::is_trivially_copyable_v<net::TimedOther>);
static_assert(std::is_trivially_copyable_v<host::PresentationFrame>);

static_assert((sw::weapon_buttons::kAll & ~sim::buttons::kWireMask) == 0);
static_assert((sw::weapon_buttons::kAll & (sim::buttons::kJump | sim::buttons::kSprint | sim::buttons::kUse)) == 0);

static_assert(uint8_t(sim::ViewQuality::None) == 0 && uint8_t(sim::ViewQuality::Interpolated) == 1 && uint8_t(sim::ViewQuality::Extrapolated) == 2 &&
		uint8_t(sim::ViewQuality::Held) == 3);
static_assert(net::weapon_flags::kExactAim == 1 && net::weapon_flags::kInfiniteMagazine == 2);
static_assert(uint8_t(net::SampleKind::Interpolate) == 0 && uint8_t(net::SampleKind::Extrapolate) == 1 && uint8_t(net::SampleKind::Hold) == 2);
static_assert(uint8_t(host::RewindVerdict::Present) == 0 && uint8_t(host::RewindVerdict::Refused) == 3);
static_assert(uint8_t(host::KitRole::Assault) == 0 && uint8_t(host::KitRole::Recon) == 3);
static_assert(host::kKitCount == 4 && host::kKitSlots[0] == 0 && host::kKitSlots[1] == 1 && host::kKitSlots[2] == 6 && host::kKitSlots[3] == 7);
static_assert(core::gameplay::kCapsuleRows == 9 && core::gameplay::kMaxTableCapsules == sw::kMaxZoneCapsules);

static_assert(host::kDefaultMaxRewindTicks == 21);
static_assert(host::kHitIndicatorSeconds == 0.3 && host::kHitIndicatorLevel == 1.0f);

constexpr std::array<net::MessageType, 4> kNewTypes{ net::MessageType::SnapshotOthers, net::MessageType::ShotEvents, net::MessageType::CombatEvents,
	net::MessageType::KitSelect };
constexpr std::array<net::MessageType, 11> kM1Types{ net::MessageType::Snapshot, net::MessageType::CommandPacket, net::MessageType::Hello,
	net::MessageType::MatchInfo, net::MessageType::Ready, net::MessageType::Spawned, net::MessageType::Loading, net::MessageType::Kick,
	net::MessageType::Bye, net::MessageType::Debug, net::MessageType::DebugReport };

namespace m2a_fake {

bool snapshot_acked(const net::CommandPacket &p_packet, uint16_t p_seq) {
	if (p_seq == p_packet.newest_snapshot) {
		return true;
	}
	if (net::seq_newer(p_seq, p_packet.newest_snapshot)) {
		return false;
	}
	const uint16_t back = uint16_t(p_packet.newest_snapshot - 1u - p_seq);
	return back < 32 && ((p_packet.snapshot_ack_bits >> back) & 1u) != 0;
}

bool others_acked(const net::CommandPacket &p_packet, uint16_t p_seq) {
	if (net::seq_newer(p_seq, p_packet.newest_snapshot)) {
		return false;
	}
	const uint16_t back = uint16_t(p_packet.newest_snapshot - p_seq);
	return back < 32 && ((p_packet.others_ack_bits >> back) & 1u) != 0;
}
}

struct AckRow {
	const char *name;
	uint16_t newest;
	uint32_t snapshot_bits;
	uint32_t others_bits;
	std::vector<uint16_t> snapshot_acked;
	std::vector<uint16_t> others_acked;
};

std::vector<uint16_t> seq_range(uint16_t p_first, uint16_t p_last) {
	std::vector<uint16_t> out;
	for (uint16_t s = p_first;; ++s) {
		out.push_back(s);
		if (s == p_last) {
			break;
		}
	}
	return out;
}

std::vector<AckRow> ack_table() {
	std::vector<AckRow> rows;

	rows.push_back({ "plain", 100, 0b101u, 0b110u, { 97, 99, 100 }, { 98, 99 } });

	rows.push_back({ "wrap", 1, 0b11u, 0b101u, { 65535, 0, 1 }, { 65535, 1 } });

	{
		AckRow r{ "full", 500, 0xFFFFFFFFu, 0xFFFFFFFFu, seq_range(468, 500), seq_range(469, 500) };
		rows.push_back(r);
	}

	rows.push_back({ "empty", 7, 0u, 0u, { 7 }, {} });

	rows.push_back({ "edge bits", 40000, 1u << 31, 1u << 31, { 39968, 40000 }, { 39969 } });
	return rows;
}

constexpr uint64_t kMatchSeed = 0x6d32612d636f6e74ull;

sw::WeaponContext standing_context() {
	sw::WeaponContext ctx;
	ctx.eye_start = { 0, 1.6f, 0 };
	ctx.eye_end = ctx.eye_start;
	return ctx;
}

sim::Command weapon_command(sim::Tick p_tick, const sw::WeaponInput &p_in, uint16_t p_yaw = 0, int16_t p_pitch = 0) {
	sim::Command c;
	c.tick = p_tick;
	c.sequence = p_tick;
	c.buttons = sw::with_weapon_input(0, p_in);
	c.yaw = p_yaw;
	c.pitch = p_pitch;
	return c;
}

bool bits_equal(float p_a, float p_b) {
	return std::memcmp(&p_a, &p_b, sizeof(float)) == 0;
}
bool bits_equal(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return bits_equal(p_a[0], p_b[0]) && bits_equal(p_a[1], p_b[1]) && bits_equal(p_a[2], p_b[2]);
}

std::string shot_difference(const sw::ShotEvent &p_a, const sw::ShotEvent &p_b) {
	if (p_a.time != p_b.time) {
		return "time";
	}
	if (p_a.spawn_time != p_b.spawn_time) {
		return "spawn_time";
	}
	if (p_a.seed != p_b.seed) {
		return "seed";
	}
	if (p_a.player_slot != p_b.player_slot || p_a.shot_index != p_b.shot_index) {
		return "player_slot / shot_index";
	}
	if (p_a.def != p_b.def || p_a.weapon_slot != p_b.weapon_slot || p_a.pellets != p_b.pellets) {
		return "def / weapon_slot / pellets";
	}
	if (!bits_equal(p_a.origin, p_b.origin)) {
		return "origin";
	}
	if (!bits_equal(p_a.aim, p_b.aim)) {
		return "aim";
	}
	if (!bits_equal(p_a.direction, p_b.direction)) {
		return "direction";
	}
	if (!bits_equal(p_a.spread, p_b.spread)) {
		return "spread";
	}
	if (!bits_equal(p_a.pellet_cone, p_b.pellet_cone)) {
		return "pellet_cone";
	}
	if (!bits_equal(p_a.recoil.pitch, p_b.recoil.pitch) || !bits_equal(p_a.recoil.yaw, p_b.recoil.yaw)) {
		return "recoil";
	}
	if (p_a.tracer != p_b.tracer) {
		return "tracer";
	}
	if (!bits_equal(p_a.power, p_b.power)) {
		return "power";
	}
	return {};
}

struct AimRun {
	std::vector<sw::ShotEvent> shots;
	std::vector<uint64_t> state_hashes;
	std::vector<sw::ImpactEvent> impacts;
	std::vector<sw::DamageEvent> damage;
};

AimRun aim_run(const std::shared_ptr<const sw::WeaponTable> &p_table, bool p_post_process) {
	AimRun out;
	const uint16_t def = tw::id_of(*p_table, "weapons/m16a4/u_m16a4");
	const uint16_t loadout[3] = { def, sw::kNoWeapon, sw::kNoWeapon };
	sw::SoldierWeaponsState st;
	const core::Status spawned = sw::spawn_soldier_weapons(st, *p_table, loadout, 0, 0);
	REQUIRE(spawned.ok());
	if (!spawned.ok()) {
		return out;
	}
	std::unique_ptr<sim::CollisionWorld> world = tw::wall_world({ 40.0f, 80.0f });
	sw::StaticTargets targets({ tw::capsule_target(1, { -0.6f, 0.9f, 20.0f }, { -0.6f, 1.7f, 20.0f }, 0.25f, sw::HitZone::Chest),
			tw::capsule_target(2, { 0.0f, 1.0f, 20.0f }, { 0.0f, 1.6f, 20.0f }, 0.3f, sw::HitZone::Body),
			tw::capsule_target(3, { 0.6f, 1.75f, 20.0f }, { 0.6f, 1.75f, 20.0f }, 0.12f, sw::HitZone::Head) });
	sw::ProjectileWorld pw(p_table);
	sw::ProjectileStepOutput pout;
	sw::WeaponTickOutput wout;
	for (sim::Tick t = 1; t <= 300; ++t) {
		sw::WeaponInput in;
		const uint32_t phase = t % 50;
		in.fire = t > 45 && (phase < 18 || phase == 30 || phase == 31 || phase == 40);
		in.zoom = t > 150;
		const uint16_t yaw = uint16_t(int32_t(t % 11) * 40 - 200);
		const int16_t pitch = int16_t(int32_t(t % 7) * 15 - 45);
		const sim::Command c = weapon_command(t, in, yaw, pitch);
		sw::WeaponContext ctx = standing_context();
		ctx.yaw = c.yaw;
		ctx.pitch = c.pitch;
		ctx.sway.zoom = in.zoom ? sw::Zoom::Zoom : sw::Zoom::NoZoom;
		sw::step_soldier_weapons(*p_table, st, c, ctx, kMatchSeed, wout);
		out.state_hashes.push_back(sw::soldier_weapons_hash(st));
		for (uint8_t i = 0; i < wout.shot_count; ++i) {
			sw::ShotEvent shot = wout.shots[i];
			if (p_post_process) {
				shot.direction = sw::aim_direction(c.yaw, c.pitch, sw::RecoilOffset{});
				shot.pellet_cone = 0;
			}
			out.shots.push_back(shot);
			(void)pw.spawn(shot);
		}
		pout.clear();
		pw.step(t, *world, targets, pout);
		out.impacts.insert(out.impacts.end(), pout.impacts.begin(), pout.impacts.end());
		out.damage.insert(out.damage.end(), pout.damage.begin(), pout.damage.end());
	}
	for (sim::Tick t = 301; t <= 420; ++t) {
		pout.clear();
		pw.step(t, *world, targets, pout);
		out.impacts.insert(out.impacts.end(), pout.impacts.begin(), pout.impacts.end());
		out.damage.insert(out.damage.end(), pout.damage.begin(), pout.damage.end());
	}
	return out;
}

std::shared_ptr<const sw::WeaponTable> with_exact_aim(const std::shared_ptr<const sw::WeaponTable> &p_base) {
	auto t = std::make_shared<sw::WeaponTable>(*p_base);
	t->exact_aim = true;
	t->hash = sw::weapon_table_hash(*t);
	return t;
}

struct RuleTick {
	sim::Tick tick = 0;
	uint32_t events = 0;
	uint8_t flags = 0;
	uint8_t mode = 0;
	float spread = 0;
	sw::WeaponPhase phase = sw::WeaponPhase::None;
};

template <typename InputFn, typename SprintFn>
std::vector<RuleTick> rules_run(const sw::WeaponTable &p_table, sim::Tick p_spawn_tick, uint32_t p_ticks, InputFn p_input, SprintFn p_sprinting) {
	const uint16_t def = tw::id_of(p_table, "weapons/m16a4/u_m16a4");
	const uint16_t loadout[3] = { def, sw::kNoWeapon, sw::kNoWeapon };
	sw::SoldierWeaponsState st;
	std::vector<RuleTick> out;
	const core::Status spawned = sw::spawn_soldier_weapons(st, p_table, loadout, 0, p_spawn_tick);
	REQUIRE(spawned.ok());
	if (!spawned.ok()) {
		return out;
	}
	sw::WeaponTickOutput wout;
	for (uint32_t i = 1; i <= p_ticks; ++i) {
		const sim::Tick t = p_spawn_tick + i;
		const sw::WeaponInput in = p_input(t);
		const sim::Command c = weapon_command(t, in);
		sw::WeaponContext ctx = standing_context();
		ctx.sprinting = p_sprinting(t);
		ctx.sway.move = ctx.sprinting ? sw::SwayMove::Sprinting : sw::SwayMove::Base;
		ctx.sway.zoom = in.zoom ? sw::Zoom::Zoom : sw::Zoom::NoZoom;
		sw::step_weapons_with_rules(p_table, st, c, ctx, kMatchSeed, p_spawn_tick, wout);
		const sw::WeaponSlotState &s = st.current();
		out.push_back({ t, wout.events, s.flags, s.mode, s.spread, s.phase });
	}
	return out;
}

size_t first_with(const std::vector<RuleTick> &p_run, uint32_t p_event) {
	for (size_t i = 0; i < p_run.size(); ++i) {
		if ((p_run[i].events & p_event) != 0) {
			return i;
		}
	}
	return p_run.size();
}

}

TEST_SUITE("m2a-contract") {
	TEST_CASE("m2a.contract: sizes, layout, wire mask and enum values") {

		std::printf("m2a.contract: sizeof Command %zu, ShotView %zu, PresentEvent %zu, OtherPose %zu, CapsuleLocal %zu, FxInstance %zu, "
					"PresentationFrame %zu\n",
				sizeof(sim::Command), sizeof(host::ShotView), sizeof(host::PresentEvent), sizeof(net::OtherPose), sizeof(core::gameplay::CapsuleLocal),
				sizeof(fx::FxInstance), sizeof(host::PresentationFrame));
		CHECK(sizeof(sim::Command) == 24);
		CHECK(sizeof(host::ShotView) == 16);
		CHECK(sizeof(host::PresentEvent) == 64);
		CHECK(sizeof(net::OtherPose) == 60);
		CHECK((sw::weapon_buttons::kAll & ~sim::buttons::kWireMask) == 0);

		for (uint32_t bits = 0; bits < 8; ++bits) {
			for (uint8_t counter = 0; counter < 4; ++counter) {
				for (uint8_t slot = 0; slot <= sw::kMaxWeaponSlots; ++slot) {
					sw::WeaponInput in;
					in.fire = (bits & 1) != 0;
					in.zoom = (bits & 2) != 0;
					in.reload = (bits & 4) != 0;
					in.fire_mode_counter = counter;
					in.select_slot = slot < sw::kMaxWeaponSlots ? slot : uint8_t(0xFF);
					const uint32_t motor = sim::buttons::kJump | sim::buttons::kSprint;
					const uint32_t b = sw::with_weapon_input(motor, in);
					CHECK((b & ~sim::buttons::kWireMask) == 0);
					CHECK((b & ~sw::weapon_buttons::kAll) == motor);
					sim::Command c;
					c.buttons = b;
					CHECK(sw::weapon_input(c) == in);
				}
			}
		}
		for (const net::MessageType t : kNewTypes) {
			for (const net::MessageType m : kM1Types) {
				CHECK(uint8_t(t) != uint8_t(m));
			}
		}
		CHECK(std::set<uint8_t>{ uint8_t(kNewTypes[0]), uint8_t(kNewTypes[1]), uint8_t(kNewTypes[2]), uint8_t(kNewTypes[3]) }.size() == 4);
		CHECK(uint8_t(net::DebugKind::SpawnTarget) != uint8_t(net::DebugKind::TargetInfo));
		CHECK(uint8_t(net::DebugKind::SpawnTarget) > uint8_t(net::DebugKind::SetSeed));
	}

	TEST_CASE("m2a.contract: defaults of the fields W2.0 added to M1 and wp structs") {
		sim::Command c;
		CHECK(c.view_back_q8 == 0);
		CHECK(c.view_quality == uint8_t(sim::ViewQuality::None));
		CHECK(c.view_seq8 == 0);
		sw::TargetQuery q;
		CHECK_FALSE(q.has_segment);
		CHECK(q.radius == 0.0f);
		CHECK(q.from == sim::Vec3{});
		CHECK(q.to == sim::Vec3{});
		CHECK_FALSE(sw::WeaponTable{}.exact_aim);
		CHECK_FALSE(sw::WeaponTableOptions{}.exact_aim);
		host::ClientSessionConfig cc;
		CHECK(cc.mechanics_cfg == std::string(sw::kMechanicsTuningPath));
		CHECK(host::GunplayConfig{}.mechanics_cfg == std::string(sw::kMechanicsTuningPath));
		CHECK_FALSE(host::GunplayConfig{}.exact_aim);
		CHECK(host::GunplayConfig{}.lag.max_rewind_ticks == host::kDefaultMaxRewindTicks);
		const auto frame = std::make_unique<host::PresentationFrame>();
		CHECK(frame->newest_seq == 0);
		host::PresentedSoldier ps;
		CHECK(ps.target_pose == sim::Pose::Stand);
		CHECK(ps.transition == 0.0f);
		net::OtherSoldier o;
		CHECK(o.weapon == 0xFF);
		CHECK(o.weapon_phase == 0);
		CHECK_FALSE(o.firing);
		CHECK_FALSE(o.ads);
		CHECK(o.reload_seq == 0);
		CHECK(o.burst_seq == 0);
		CHECK(net::CommandPacket{}.others_ack_bits == 0);
		CHECK_FALSE(net::Snapshot{}.own_weapons.has_value());
		net::MatchInfo mi;
		CHECK(mi.match_seed == 0);
		CHECK(mi.weapon_hash == 0);
		CHECK(mi.weapon_flags == 0);
		CHECK(mi.kit_hash == 0);
		CHECK(net::Spawned{}.team == 1);
		CHECK(net::Spawned{}.kit == 0);
		net::DebugRequest dr;
		CHECK(dr.slot == 0);
		CHECK(dr.pose == sim::Pose::Stand);
		net::DebugReport rep;
		CHECK(rep.targets.empty());
		CHECK(rep.trace_gunplay.empty());
		CHECK(rep.trace_dropped == 0);
		net::OtherPose op;
		CHECK(op.kind == uint8_t(net::SampleKind::Interpolate));
		CHECK_FALSE(op.extrapolated);
		CHECK_FALSE(op.held);
		host::ShotView sv;
		CHECK(sv.quality == uint8_t(sim::ViewQuality::None));
		CHECK(sv.main_bits == 0);
		CHECK(sv.others_bits == 0);
		if constexpr (kNtLanded) {
			CHECK(net::kProtocolVersion == 4);
		} else {
			CHECK(net::kProtocolVersion == 1);
			pending("nt", "kProtocolVersion == 2", "nt bumps the version with the v2 codecs");
		}
	}

	TEST_CASE("m2a.contract: the two ack-bit conventions of §7.2 (table of newest, bits -> acked seqs)") {
		for (const AckRow &row : ack_table()) {
			CAPTURE(row.name);
			net::CommandPacket p;
			p.newest_snapshot = row.newest;
			p.snapshot_ack_bits = row.snapshot_bits;
			p.others_ack_bits = row.others_bits;
			const std::set<uint16_t> want_snapshot(row.snapshot_acked.begin(), row.snapshot_acked.end());
			const std::set<uint16_t> want_others(row.others_acked.begin(), row.others_acked.end());
			std::set<uint16_t> got_snapshot, got_others;
			for (int32_t d = -40; d <= 8; ++d) {
				const uint16_t seq = uint16_t(int32_t(row.newest) + d);
				if (m2a_fake::snapshot_acked(p, seq)) {
					got_snapshot.insert(seq);
				}
				if (m2a_fake::others_acked(p, seq)) {
					got_others.insert(seq);
				}
				if constexpr (kNtLanded) {
					CAPTURE(seq);
					CHECK(net::snapshot_acked(p, seq) == m2a_fake::snapshot_acked(p, seq));
					CHECK(net::others_acked(p, seq) == m2a_fake::others_acked(p, seq));
				}
			}
			CHECK(got_snapshot == want_snapshot);
			CHECK(got_others == want_others);

			CHECK(m2a_fake::snapshot_acked(p, row.newest));
			CHECK(m2a_fake::others_acked(p, row.newest) == ((row.others_bits & 1u) != 0));

			CHECK_FALSE(m2a_fake::snapshot_acked(p, uint16_t(row.newest + 1)));
			CHECK_FALSE(m2a_fake::others_acked(p, uint16_t(row.newest + 1)));

			p.count = 1;
			p.commands[0].tick = 1000;
			p.commands[0].sequence = 77;
			const std::vector<uint8_t> bytes = net::encode_command_packet(p);
			REQUIRE_FALSE(bytes.empty());
			core::Result<net::CommandPacket> back = net::decode_command_packet(bytes);
			REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
			if (!back.ok()) {
				continue;
			}
			CHECK(back->newest_snapshot == row.newest);
			CHECK(back->snapshot_ack_bits == row.snapshot_bits);
			for (int32_t d = -40; d <= 8; ++d) {
				const uint16_t seq = uint16_t(int32_t(row.newest) + d);
				CHECK(m2a_fake::snapshot_acked(*back, seq) == m2a_fake::snapshot_acked(p, seq));
			}
			if constexpr (kNtLanded) {
				CHECK(back->others_ack_bits == row.others_bits);
				for (int32_t d = -40; d <= 8; ++d) {
					const uint16_t seq = uint16_t(int32_t(row.newest) + d);
					CHECK(net::others_acked(*back, seq) == m2a_fake::others_acked(p, seq));
				}
			}
		}
		if constexpr (!kNtLanded) {
			pending("nt", "ack table through net::snapshot_acked / others_acked and others_ack_bits through the v2 command codec",
					"the predicates and the v2 encoder are nt's bodies; the table is checked against the m2a_fake reference and M1's codec now");
		}
	}

	TEST_CASE("m2a.contract: exact_aim changes the hash and equals the range post-process field for field (§4.2)") {

		const std::shared_ptr<const sw::WeaponTable> off = tw::synthetic_table(true);
		const std::shared_ptr<const sw::WeaponTable> on = with_exact_aim(off);
		const bool hash_differs = on->hash != off->hash;
		std::printf("m2a.contract: weapon_table_hash flag off %016llx, on %016llx (%s)\n", (unsigned long long)off->hash, (unsigned long long)on->hash,
				hash_differs ? "differ" : "equal");

		const AimRun post = aim_run(off, true);
		const AimRun flag = aim_run(on, false);
		const AimRun plain = aim_run(off, false);
		size_t shot_mismatches = 0;
		std::string first;
		for (size_t i = 0; i < std::min(post.shots.size(), flag.shots.size()); ++i) {
			const std::string d = shot_difference(post.shots[i], flag.shots[i]);
			if (!d.empty()) {
				++shot_mismatches;
				if (first.empty()) {
					first = "shot " + std::to_string(i) + ": " + d;
				}
			}
		}
		size_t impact_mismatches = 0;
		for (size_t i = 0; i < std::min(post.impacts.size(), flag.impacts.size()); ++i) {
			const sw::ImpactEvent &a = post.impacts[i], &b = flag.impacts[i];
			impact_mismatches += (a.time != b.time || !bits_equal(a.position, b.position) || a.kind != b.kind || a.shot_index != b.shot_index) ? 1 : 0;
		}
		size_t damage_mismatches = 0;
		for (size_t i = 0; i < std::min(post.damage.size(), flag.damage.size()); ++i) {
			const sw::DamageEvent &a = post.damage[i], &b = flag.damage[i];
			damage_mismatches += (a.time != b.time || a.target != b.target || a.zone != b.zone || !bits_equal(a.amount, b.amount) ||
										 a.shot_index != b.shot_index)
					? 1
					: 0;
		}
		std::printf("m2a.contract: exact_aim run: %zu / %zu shots (post-process / flag), %zu / %zu impacts, %zu / %zu damage events; %zu shot, %zu impact, "
					"%zu damage mismatches%s%s; plain run %zu shots, %zu damage events\n",
				post.shots.size(), flag.shots.size(), post.impacts.size(), flag.impacts.size(), post.damage.size(), flag.damage.size(), shot_mismatches,
				impact_mismatches, damage_mismatches, first.empty() ? "" : ", first ", first.c_str(), plain.shots.size(), plain.damage.size());

		CHECK(post.shots.size() >= 25);
		CHECK(!post.damage.empty());
		CHECK(post.state_hashes == plain.state_hashes);
		size_t plain_diff = 0;
		for (size_t i = 0; i < std::min(post.shots.size(), plain.shots.size()); ++i) {
			plain_diff += bits_equal(post.shots[i].direction, plain.shots[i].direction) ? 0 : 1;
		}
		CHECK(plain_diff > 0);

		sw::WeaponTableOptions options;
		options.exact_aim = true;
		std::vector<core::gameplay::WeaponDef> defs;
		for (const tw::Synthetic &s : tw::synthetic_gate_weapons()) {
			defs.push_back(s.def);
		}
		core::Result<std::shared_ptr<const sw::WeaponTable>> built = sw::make_weapon_table(defs, off->materials, options);
		REQUIRE_MESSAGE(built.ok(), (built.ok() ? std::string() : built.error().message));
		if constexpr (kSvRulesLanded) {
			CHECK(hash_differs);
			CHECK(post.shots.size() == flag.shots.size());
			CHECK_MESSAGE(shot_mismatches == 0, first);
			CHECK(post.impacts.size() == flag.impacts.size());
			CHECK(impact_mismatches == 0);
			CHECK(post.damage.size() == flag.damage.size());
			CHECK(damage_mismatches == 0);
			CHECK(post.state_hashes == flag.state_hashes);
			CHECK((built.ok() && (*built)->exact_aim));
		} else {
			pending("sv", "exact_aim hash change and field-for-field equality with the range post-process",
					"R3 (the exact_aim branch in step_soldier_weapons and the hash line in weapon_table_hash) is sv's; the arms ran above");
		}
	}

	TEST_CASE("m2a.contract: step_weapons_with_rules rows (§5.2): zoom refused while deploying and sprinting; the fire-mode counter at spawn") {
		const std::shared_ptr<const sw::WeaponTable> table = tw::synthetic_table();
		const sim::Tick spawn = 100;
		auto never = [](sim::Tick) { return false; };
		auto zoom = [](sim::Tick) {
			sw::WeaponInput in;
			in.zoom = true;
			return in;
		};
		auto idle = [](sim::Tick) { return sw::WeaponInput{}; };

		const std::vector<RuleTick> z = rules_run(*table, spawn, 90, zoom, never);
		const std::vector<RuleTick> h = rules_run(*table, spawn, 90, idle, never);
		const size_t deployed = first_with(z, sw::weapon_events::kDeployFinished);
		REQUIRE(deployed + 1 < z.size());
		if (deployed + 1 >= z.size()) {
			return;
		}
		CHECK(deployed == first_with(h, sw::weapon_events::kDeployFinished));
		bool row1 = true;
		for (size_t i = 0; i <= deployed; ++i) {
			row1 = row1 && (z[i].flags & sw::slot_flags::kZoomWasDown) == 0 && bits_equal(z[i].spread, h[i].spread);
		}
		row1 = row1 && (z[deployed + 1].flags & sw::slot_flags::kZoomWasDown) != 0;
		std::printf("m2a.contract: row 1: DeployFinished at tick spawn + %zu; zoom flag %s through it, %s after it\n", deployed + 1,
				(z[deployed].flags & sw::slot_flags::kZoomWasDown) != 0 ? "set" : "clear",
				(z[deployed + 1].flags & sw::slot_flags::kZoomWasDown) != 0 ? "set" : "clear");

		const sim::Tick sprint_end = spawn + 120;
		auto sprinting = [sprint_end](sim::Tick p_t) { return p_t <= sprint_end; };
		const std::vector<RuleTick> zs = rules_run(*table, spawn, 150, zoom, sprinting);
		const std::vector<RuleTick> hs = rules_run(*table, spawn, 150, idle, sprinting);
		bool row2 = true;
		size_t after = zs.size();
		for (size_t i = 0; i < zs.size(); ++i) {
			if (zs[i].tick <= sprint_end) {
				row2 = row2 && (zs[i].flags & sw::slot_flags::kZoomWasDown) == 0 && bits_equal(zs[i].spread, hs[i].spread);
			} else if (after == zs.size()) {
				after = i;
			}
		}
		REQUIRE(after < zs.size());
		if (after >= zs.size()) {
			return;
		}
		row2 = row2 && (zs[after].flags & sw::slot_flags::kZoomWasDown) != 0;

		auto counter = [spawn](sim::Tick p_t) {
			sw::WeaponInput in;
			in.fire_mode_counter = p_t < spawn + 60 ? 2 : 3;
			return in;
		};
		const std::vector<RuleTick> m = rules_run(*table, spawn, 90, counter, never);
		bool row3 = m[0].mode == 0 && (m[0].events & sw::weapon_events::kFireModeChanged) == 0;
		size_t changes = 0;
		for (size_t i = 0; i < m.size(); ++i) {
			if ((m[i].events & sw::weapon_events::kFireModeChanged) != 0) {
				++changes;
				row3 = row3 && m[i].tick == spawn + 60;
			}
			row3 = row3 && m[i].mode == (m[i].tick < spawn + 60 ? 0 : 1);
		}
		row3 = row3 && changes == 1;
		std::printf("m2a.contract: rows 1-3 with the current weapon_rules.cpp: %s / %s / %s (mode after the first command %u, %zu mode changes)\n",
				row1 ? "hold" : "fail", row2 ? "hold" : "fail", row3 ? "hold" : "fail", unsigned(m[0].mode), changes);
		if constexpr (kSvRulesLanded) {
			CHECK_MESSAGE(row1, "zoom held from spawn through the deploy");
			CHECK_MESSAGE(row2, "zoom held while sprinting");
			CHECK_MESSAGE(row3, "the fire-mode counter at spawn");
		} else {
			pending("sv", "the three step_weapons_with_rules rows", "weapon_rules.cpp is a forwarding stub until sv lands the two rules; the runs above report the stub");
		}
	}

	TEST_CASE("m2a.contract: the own-weapons wire view round-trips a 600-tick weapon run (§7.3)") {
		const std::shared_ptr<const sw::WeaponTable> table = tw::synthetic_table();
		const uint16_t def = tw::id_of(*table, "weapons/m16a4/u_m16a4");
		const uint16_t other = tw::id_of(*table, "weapons/m9/u_m9");
		const uint16_t loadout[3] = { def, other, sw::kNoWeapon };
		sw::SoldierWeaponsState st;
		st.player_slot = 3;
		REQUIRE(sw::spawn_soldier_weapons(st, *table, loadout, 0, 0).ok());
		std::mt19937 rng(7);
		sw::WeaponTickOutput wout;
		std::vector<sw::SoldierWeaponsState> states;
		std::vector<sim::Command> commands;
		std::vector<sw::WeaponContext> contexts;
		uint8_t counter = 0;
		for (sim::Tick t = 1; t <= 600; ++t) {
			const uint32_t r = rng();
			sw::WeaponInput in;
			in.fire = (r % 3) != 0;
			in.zoom = (r & 8) != 0;
			in.reload = (r % 97) == 0;
			counter = uint8_t(counter + ((r % 89) == 0 ? 1 : 0));
			in.fire_mode_counter = uint8_t(counter & 3);
			in.select_slot = (r % 71) == 0 ? uint8_t((r >> 8) % 2) : uint8_t(0xFF);
			const sim::Command c = weapon_command(t, in, uint16_t(r >> 16), int16_t(int32_t((r >> 4) % 401) - 200));
			sw::WeaponContext ctx = standing_context();
			ctx.yaw = c.yaw;
			ctx.pitch = c.pitch;
			ctx.sprinting = (r % 53) == 0;
			ctx.sway.zoom = in.zoom ? sw::Zoom::Zoom : sw::Zoom::NoZoom;
			states.push_back(st);
			commands.push_back(c);
			contexts.push_back(ctx);
			sw::step_soldier_weapons(*table, st, c, ctx, kMatchSeed, wout);
		}
		CHECK(states.size() == 600);
		if constexpr (kNtLanded) {
			size_t bad = 0;
			for (size_t i = 0; i < states.size(); ++i) {
				const sim::Tick tick = states[i].tick;
				const net::WeaponsWire wire = net::weapons_wire_view(states[i], tick);
				net::BitWriter w(4096);
				net::write_weapons_wire(w, wire);
				REQUIRE_FALSE(w.overflowed());
				net::BitReader rd(w.bytes());
				net::WeaponsWire read;
				REQUIRE(net::read_weapons_wire(rd, read).ok());
				bad += read == wire ? 0 : 1;
				core::Result<sw::SoldierWeaponsState> rebuilt = net::weapons_from_wire(read, tick, states[i].player_slot, table->defs.size());
				REQUIRE_MESSAGE(rebuilt.ok(), (rebuilt.ok() ? std::string() : rebuilt.error().message));
				CHECK(net::weapons_wire_view(*rebuilt, tick) == wire);
				sw::SoldierWeaponsState a = states[i], b = *rebuilt;
				sw::WeaponTickOutput oa, ob;
				sw::step_soldier_weapons(*table, a, commands[i], contexts[i], kMatchSeed, oa);
				sw::step_soldier_weapons(*table, b, commands[i], contexts[i], kMatchSeed, ob);
				CHECK(net::weapons_wire_view(a, tick + 1) == net::weapons_wire_view(b, tick + 1));
			}
			CHECK(bad == 0);
		} else {
			pending("nt", "WeaponsWire round trip of a 600-tick run (view -> bits -> view -> state -> step equal)",
					"weapons_wire.cpp is nt's; the 600 states were generated");
		}
	}

	TEST_CASE("m2a.contract: the gunplay fixture's loopback session on the synthetic floor spawns and answers a Teleport") {

		const std::shared_ptr<const host::SharedLevel> level = tg::synthetic_floor();
		REQUIRE(level != nullptr);
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<tg::Session> s = tg::start_session(level);
		REQUIRE(s != nullptr);
		if (s == nullptr) {
			return;
		}
		const bool spawned = tg::wait_spawned(*s, 10.0);
		REQUIRE(spawned);
		if (!spawned) {
			return;
		}
		const std::optional<net::DebugReport> r = tg::teleport(*s, { 5.0f, 0.0f, 5.0f }, 0);
		REQUIRE(r.has_value());
		if (!r.has_value()) {
			return;
		}
		CHECK_MESSAGE(r->ok, r->text);
		bool moved = false;
		tg::run_for(*s, 1.0, [&](bool, uint64_t, double) {
			const sim::Vec3 &p = s->frame->own_curr.position;
			moved = moved || (std::fabs(p[0] - 5.0f) < 0.2f && std::fabs(p[2] - 5.0f) < 0.2f);
		});
		CHECK(moved);
		const sim::Vec3 eye = tg::own_eye(*s->frame);
		std::printf("m2a.contract: fixture session: teleported, eye (%.2f, %.2f, %.2f), '%s'\n", double(eye[0]), double(eye[1]), double(eye[2]), r->text.c_str());
		s->client.stop();
		s->server.stop();
	}
}

static_assert(net::kMaxKits == host::kKitCount, "KitSelect / Spawned kit bound");
static_assert(net::kMaxTeam == host::kKitTeams, "KitSelect / Spawned team bound");

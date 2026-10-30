#include <doctest/doctest.h>

#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/percentile.h"
#include "bf3/host/ai_bot_source.h"
#include "bf3/host/game_server.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/sim/bots/aim_model.h"
#include "bf3/sim/bots/perception.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/logic/round_natives.h"
#include "bf3/sim/static_world.h"

#include "gunplay_fixture.h"
#include "tdm_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace bf3;
namespace tg = bf3::test::gunplay;
namespace tt = bf3::test::tdm;
namespace sb = bf3::sim::bots;

namespace {

constexpr bool kRuNtLanded = true;

constexpr float kDeg = sim::kPi / 180.0f;

constexpr uint64_t kReplayDecisionHash = 0xf02880db4231ee07ull;
constexpr uint64_t kReplayWorldHash = 0x89d71173b371ca38ull;

void pending_skip(const char *p_row) {
	std::printf("%s: PENDING (ru, nt): needs run_tdm and the v3 codecs on the branch\n", p_row);
	bf3::test::skip_install_case("PENDING (ru, nt)");
}

void quad(std::vector<sim::StaticTriangle> &r_out, const sim::Vec3 &p_a, const sim::Vec3 &p_b, const sim::Vec3 &p_c, const sim::Vec3 &p_d) {
	sim::StaticTriangle t;
	t.v = { p_a, p_b, p_c };
	r_out.push_back(t);
	t.v = { p_a, p_c, p_d };
	r_out.push_back(t);
}

std::unique_ptr<sim::CollisionWorld> wall_world() {
	std::vector<sim::StaticTriangle> t;
	for (float x = -100.0f; x < 100.0f; x += 10.0f) {
		for (float z = -100.0f; z < 100.0f; z += 10.0f) {
			quad(t, { x, 0, z }, { x, 0, z + 10.0f }, { x + 10.0f, 0, z + 10.0f }, { x + 10.0f, 0, z });
		}
	}
	quad(t, { -5, 0, 10 }, { -5, 4, 10 }, { 5, 4, 10 }, { 5, 0, 10 });
	quad(t, { -5, 0, 10 }, { 5, 0, 10 }, { 5, 4, 10 }, { -5, 4, 10 });
	core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes = sim::build_static_world(std::span<const sim::StaticTriangle>(t));
	REQUIRE(shapes.ok());
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(*shapes);
	REQUIRE(world.ok());
	return std::move(*world);
}

std::unique_ptr<sim::CollisionWorld> low_wall_world() {
	std::vector<sim::StaticTriangle> t;
	quad(t, { -100, 0, -100 }, { -100, 0, 100 }, { 100, 0, 100 }, { 100, 0, -100 });
	quad(t, { -5, 0, 18 }, { -5, 1.45f, 18 }, { 5, 1.45f, 18 }, { 5, 0, 18 });
	quad(t, { -5, 0, 18 }, { 5, 0, 18 }, { 5, 1.45f, 18 }, { -5, 1.45f, 18 });
	core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes = sim::build_static_world(std::span<const sim::StaticTriangle>(t));
	REQUIRE(shapes.ok());
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(*shapes);
	REQUIRE(world.ok());
	return std::move(*world);
}

sim::SoldierState soldier_at(const sim::Vec3 &p_feet, float p_yaw = 0.0f) {
	sim::SoldierState s;
	s.position = p_feet;
	s.yaw = sim::quantize_yaw(p_yaw);
	s.flags = sim::soldier_flags::kAlive | sim::soldier_flags::kGrounded;
	s.mode = sim::MoveMode::OnGround;
	s.life = 1;
	s.health = 100.0f;
	return s;
}

}

namespace m2b_fake {

struct FakeView final : host::ServerView {
	std::vector<sim::SoldierState> slots;
	std::vector<uint8_t> teams;
	const sim::CollisionWorld *world_ = nullptr;
	sim::Tick now = 0;
	std::vector<host::DownedSoldier> down;
	net::MatchPhase phase_ = net::MatchPhase::InRound;
	bool restricted = false;
	std::vector<uint8_t> kits;

	std::span<const sim::SoldierState> states() const override { return slots; }
	uint8_t team(uint8_t p_slot) const override { return p_slot < teams.size() ? teams[p_slot] : 0; }
	bool alive(uint8_t p_slot) const override {
		return p_slot < slots.size() && (slots[p_slot].flags & sim::soldier_flags::kAlive) != 0 && slots[p_slot].mode != sim::MoveMode::Dead;
	}
	const sim::weapons::SoldierWeaponsState &weapons(uint8_t) const override {
		static const sim::weapons::SoldierWeaponsState s_empty;
		return s_empty;
	}
	const sim::CollisionWorld &world() const override { return *world_; }
	sim::Tick tick() const override { return now + 1; }
	std::span<const host::DownedSoldier> downed() const override { return down; }
	net::MatchPhase phase() const override { return phase_; }
	bool input_restricted() const override { return restricted; }
	uint8_t kit(uint8_t p_slot) const override { return p_slot < kits.size() ? kits[p_slot] : 0; }
};

struct Recorder final : host::TdmBotObserver {
	std::vector<std::pair<uint8_t, uint8_t>> detected_pairs;
	std::vector<host::TdmBotActivity> activities;
	void detected(const host::ServerView &, sim::Tick, uint8_t p_bot, uint8_t p_target, const sim::Vec3 &) override { detected_pairs.push_back({ p_bot, p_target }); }
	void activity(sim::Tick, uint8_t, host::TdmBotActivity p_activity) override { activities.push_back(p_activity); }
};

}

TEST_SUITE("host-tdm-bots-unit") {
	TEST_CASE("perception: FOV, range, walls, detection time, forgetting, the stagger") {
		const std::unique_ptr<sim::CollisionWorld> world = wall_world();
		sb::PerceptionParams params;
		CHECK(sb::detection_time(params, 10.0f) == doctest::Approx(0.2f));
		CHECK(sb::detection_time(params, 100.0f) == doctest::Approx(1.5f));
		CHECK(sb::detection_time(params, 55.0f) == doctest::Approx(0.85f));
		CHECK(sb::detection_time(params, 1.0f) == doctest::Approx(0.2f));
		CHECK(sb::detection_time(params, 500.0f) == doctest::Approx(1.5f));

		uint32_t hits = 0;
		for (sim::Tick t = 0; t < 60; ++t) {
			for (uint8_t s = 0; s < 12; ++s) {
				hits += sb::perception_tick(s, t, 6) ? 1u : 0u;
			}
		}
		CHECK(hits == 120);
		CHECK(sb::perception_tick(5, 1, 6));
		CHECK_FALSE(sb::perception_tick(5, 2, 6));

		std::vector<sim::SoldierState> states(6);
		std::vector<uint8_t> teams{ 1, 2, 2, 2, 2, 1 };
		std::vector<uint8_t> alive(6, 1);
		std::vector<uint8_t> firing(6, 0);
		states[0] = soldier_at({ 0, 0, 0 });
		states[1] = soldier_at({ 20, 0, 20 });
		states[2] = soldier_at({ 0, 0, 20 });
		states[3] = soldier_at({ 0, 0, -20 });
		states[4] = soldier_at({ 8, 0, 130 });
		states[5] = soldier_at({ -20, 0, 20 });
		sb::Perception p(params);
		sb::PerceptionInput in;
		in.self = 0;
		in.team = 1;
		in.eye = { 0, 1.6f, 0 };
		in.yaw = sim::quantize_yaw(0.0f);
		in.states = states;
		in.teams = teams;
		in.alive = alive;
		in.firing = firing;
		in.world = world.get();

		int detected_at = -1;
		uint32_t rays = 0;
		for (int u = 0; u < 8; ++u) {
			in.tick = sim::Tick(6 * (u + 1));
			rays += p.update(in);
			const sb::PerceivedEnemy *e1 = p.find(1);
			REQUIRE(e1 != nullptr);
			CHECK(e1->visible);
			if (e1->detected && detected_at < 0) {
				detected_at = u;
				CHECK(p.newly_detected().size() == 1);
				CHECK(p.newly_detected()[0] == 1);
			}
		}
		CHECK(detected_at == 4);
		CHECK(p.find(2) == nullptr);
		CHECK(p.find(3) == nullptr);
		CHECK(p.find(4) == nullptr);
		CHECK(p.find(5) == nullptr);
		CHECK(rays <= 8u * 2u * params.max_candidates);

		states[1].velocity = { 3.0f, 0.0f, 0.0f };
		firing[1] = 1;
		sb::Perception fast(params);
		in.tick = 6;
		fast.update(in);
		CHECK_FALSE(fast.find(1)->detected);
		in.tick = 12;
		fast.update(in);
		CHECK(fast.find(1)->detected);

		states[1].position = { 0, 0, 25 };
		const sim::Tick seen = p.find(1)->last_visible;
		in.tick = seen + 6;
		p.update(in);
		REQUIRE(p.find(1) != nullptr);
		CHECK_FALSE(p.find(1)->visible);
		CHECK(p.find(1)->last_visible == seen);
		in.tick = seen + 180;
		p.update(in);
		CHECK(p.find(1) != nullptr);
		in.tick = seen + 186;
		p.update(in);
		CHECK(p.find(1) == nullptr);

		states[1].position = { 20, 0, 20 };
		in.tick += 6;
		p.update(in);
		REQUIRE(p.find(1) != nullptr);
		alive[1] = 0;
		in.tick += 6;
		p.update(in);
		CHECK(p.find(1) == nullptr);

		std::vector<sim::SoldierState> many(11);
		std::vector<uint8_t> many_teams(11, 2);
		many_teams[0] = 1;
		many[0] = soldier_at({ 0, 0, 0 });
		for (int i = 1; i <= 10; ++i) {
			many[size_t(i)] = soldier_at({ 24.0f + 4.0f * float(i), 0, 40 });
		}
		sb::PerceptionInput m = in;
		m.states = many;
		m.teams = many_teams;
		m.alive = {};
		m.firing = {};
		sb::Perception pm(params);
		m.tick = 6;
		CHECK(pm.update(m) == 4u);
		CHECK(pm.find(1)->chest_visible);
		CHECK(pm.enemies().size() == 4u);
		CHECK(pm.find(1) != nullptr);
		CHECK(pm.find(4) != nullptr);
		CHECK(pm.find(5) == nullptr);
	}

	TEST_CASE("perception: the chest ray first; only the head over a low wall is recorded as such") {
		const std::unique_ptr<sim::CollisionWorld> world = low_wall_world();
		sb::PerceptionParams params;
		std::vector<sim::SoldierState> states(3);
		const std::vector<uint8_t> teams{ 1, 2, 2 };
		states[0] = soldier_at({ 0, 0, 0 });
		states[1] = soldier_at({ 0, 0, 20 });
		states[2] = soldier_at({ 15, 0, 20 });
		sb::Perception p(params);
		sb::PerceptionInput in;
		in.self = 0;
		in.team = 1;
		in.eye = { 0, 1.6f, 0 };
		in.yaw = sim::quantize_yaw(0.0f);
		in.states = states;
		in.teams = teams;
		in.world = world.get();
		in.tick = 6;
		CHECK(p.update(in) == 3u);
		const sb::PerceivedEnemy *e1 = p.find(1);
		const sb::PerceivedEnemy *e2 = p.find(2);
		REQUIRE(e1 != nullptr);
		REQUIRE(e2 != nullptr);
		CHECK(e1->visible);
		CHECK_FALSE(e1->chest_visible);
		CHECK(e2->visible);
		CHECK(e2->chest_visible);
	}

	TEST_CASE("perception: hearing gives a position estimate, never a detection; hostile values are skipped") {
		sb::PerceptionParams params;
		std::vector<sim::SoldierState> states(3);
		std::vector<uint8_t> teams{ 1, 2, 2 };
		states[0] = soldier_at({ 0, 0, 0 });
		states[1] = soldier_at({ 0, 0, -40 });
		states[2] = soldier_at({ 0, 0, -90 });
		std::vector<sb::HeardShot> shots{ { 1, { 0, 1.5f, -40 }, 10 }, { 2, { 0, 1.5f, -90 }, 10 }, { 1, { 0, 1.5f, -40 }, 1 } };
		sb::Perception p(params);
		sb::PerceptionInput in;
		in.self = 0;
		in.team = 1;
		in.tick = 12;
		in.eye = { 0, 1.6f, 0 };
		in.states = states;
		in.teams = teams;
		in.shots = shots;
		in.seed = 7;
		CHECK(p.update(in) == 0u);
		const sb::PerceivedEnemy *e = p.find(1);
		REQUIRE(e != nullptr);
		CHECK(e->heard);
		CHECK_FALSE(e->detected);
		CHECK_FALSE(e->ever_visible);
		CHECK(e->last_seen == 10);
		const float err = std::hypot(e->last_position[0] - 0.0f, e->last_position[2] + 40.0f);
		CHECK(err <= params.hearing_error_m + 1e-3f);
		CHECK(p.find(2) == nullptr);
		CHECK(p.heard_last() == 1u);

		sb::Perception q(params);
		q.update(in);
		CHECK(q.find(1)->last_position == e->last_position);

		states[1].position = { NAN, 0, 0 };
		std::vector<sb::HeardShot> bad{ { 1, { NAN, 0, 0 }, 12 }, { 200, { 0, 0, 1 }, 12 }, { 0, { 0, 0, 1 }, 12 } };
		in.shots = bad;
		sb::Perception r(params);
		CHECK(r.update(in) == 0u);
		CHECK(r.enemies().empty());
	}

	TEST_CASE("aim model: the error cone, lead and drop, hostile inputs") {
		const sb::AimParams a;
		CHECK(sb::aim_error(a, 0.5f, 0.0f, false, 0.0f) == doctest::Approx(3.0f));
		CHECK(sb::aim_error(a, 0.5f, 100.0f, false, 0.0f) == doctest::Approx(0.5f));
		CHECK(sb::aim_error(a, 0.5f, 0.6f, false, 0.0f) == doctest::Approx(0.5f + 2.5f * std::exp(-1.0f)));
		CHECK(sb::aim_error(a, 0.5f, 100.0f, true, 0.0f) == doctest::Approx(2.0f));
		CHECK(sb::aim_error(a, 0.5f, 100.0f, false, 4.0f) == doctest::Approx(2.5f));
		CHECK(sb::aim_error(a, 0.3f, 0.0f, false, 0.0f) == doctest::Approx(4.2f));
		CHECK(sb::aim_error(a, 0.8f, 0.0f, false, 0.0f) == doctest::Approx(1.2f));
		CHECK(sb::aim_error(a, 0.3f, 1.0f, false, 0.0f) > sb::aim_error(a, 0.8f, 1.0f, false, 0.0f));
		CHECK(sb::aim_error(a, 0.5f, 0.2f, false, 0.0f) > sb::aim_error(a, 0.5f, 0.8f, false, 0.0f));

		CHECK(sb::aim_error(a, NAN, NAN, false, NAN) == doctest::Approx(3.0f));
		CHECK(sb::aim_error(a, 0.5f, -5.0f, false, -2.0f) == doctest::Approx(4.0f));
		CHECK(sb::aim_error(a, 0.5f, 0.0f, true, 1e30f) == doctest::Approx(45.0f));
		sb::AimParams bad;
		bad.e0_deg = NAN;
		bad.tau_s = -1.0f;
		const float s = sb::aim_error(bad, 0.5f, 1.0f, false, 0.0f);
		CHECK(std::isfinite(s));
		CHECK(s >= 0.0f);

		const sim::Vec3 o{ 0, 1.6f, 0 }, t{ 0, 1.6f, 100 }, v{ 5, 0, 0 };
		const sim::Vec3 l = sb::lead_point(o, t, v, 500.0f, 0.0f);
		CHECK(l[0] == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(l[2] == doctest::Approx(100.0f));

		const sim::Vec3 d = sb::lead_point(o, t, {}, 500.0f, -9.81f);
		CHECK(d[1] - t[1] == doctest::Approx(0.5f * 9.81f * 0.04f).epsilon(0.01));

		CHECK(sb::lead_point(o, t, v, 0.0f, -9.81f) == t);
		CHECK(sb::lead_point(o, t, v, NAN, -9.81f) == t);
		CHECK(sb::lead_point({ NAN, 0, 0 }, t, v, 500.0f, 0.0f) == t);
		const sim::Vec3 nv = sb::lead_point(o, t, { NAN, 0, 0 }, 500.0f, 0.0f);
		CHECK(nv == t);
		const sim::Vec3 far = sb::lead_point(o, { 0, 0, 1e6f }, v, 1.0f, -9.81f);
		CHECK(std::isfinite(far[1]));
		CHECK(far[0] == doctest::Approx(25.0f));
	}

	TEST_CASE("bot_tuning.cfg: the shipped file is the compiled-in defaults; hostile files fail") {
		std::ifstream f(OPENBF3_BOT_TUNING_CFG, std::ios::binary);
		REQUIRE(f.good());
		std::stringstream ss;
		ss << f.rdbuf();
		host::TdmBotConfig parsed;
		parsed.count = 7;
		const core::Status st = host::parse_bot_tuning(ss.str(), parsed);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
		const host::TdmBotConfig def;
		CHECK(parsed.count == 7);
		CHECK(parsed.skill_min == def.skill_min);
		CHECK(parsed.skill_max == def.skill_max);
		CHECK(parsed.reaction_min_s == def.reaction_min_s);
		CHECK(parsed.reaction_max_s == def.reaction_max_s);
		CHECK(parsed.w_distance == def.w_distance);
		CHECK(parsed.burst_min_rounds == def.burst_min_rounds);
		CHECK(parsed.burst_max_rounds == def.burst_max_rounds);
		CHECK(parsed.perception.fov_h_deg == def.perception.fov_h_deg);
		CHECK(parsed.perception.update_ticks == def.perception.update_ticks);
		CHECK(parsed.perception.hearing_radius_m == def.perception.hearing_radius_m);
		CHECK(parsed.perception.chest_height_m == def.perception.chest_height_m);
		CHECK(parsed.aim.e0_deg == def.aim.e0_deg);
		CHECK(parsed.aim.skill_scale == def.aim.skill_scale);
		CHECK(parsed.revive_radius_m == def.revive_radius_m);
		CHECK(parsed.regen_until_hp == def.regen_until_hp);
		CHECK(parsed.behaviours == def.behaviours);
		CHECK(parsed.strategy == def.strategy);

		host::TdmBotConfig one;
		REQUIRE(host::parse_bot_tuning("# c\n  aim.e0_deg = 2.5 # trailing\n\nperception.update_ticks=3\n", one).ok());
		CHECK(one.aim.e0_deg == doctest::Approx(2.5f));
		CHECK(one.perception.update_ticks == 3);
		CHECK(one.aim.e_inf_deg == def.aim.e_inf_deg);
		CHECK(host::parse_bot_tuning("ai.max_bots = 12\nai.enable_vehicle_ai = true\nskill_min = 0.3\n", one).ok());
		REQUIRE(host::parse_bot_tuning("bb.random.weight = 2.5\nbb.decision_ticks = 3\n", one).ok());
		CHECK(one.behaviours.curves[size_t(ai::InfantryBehaviour::Random)].weight == doctest::Approx(2.5f));
		CHECK(one.behaviours.decision_ticks == 3u);

		for (const char *bad : { "nope = 1\n", "skill_min = nan\n", "skill_min = 2\n", "skill_min\n",
					 "skill_min = 1x\n", "perception.update_ticks = 0\n", "aim.tau_s = 1e40\n", "count = 65\n", "= 3\n",
					 "ai.nope = 1\n", "ai.max_bots = 65\n", "ai.enable_air_ai = 1\n", "bb.nope = 1\n",
					 "bb.decision_ticks = 0\n", "bb.idle.weight = -1\n", "sai.nope = 1\n", "sai.period_ticks = 0\n",
					 "sai.order.random = -1\n", "bb.mission_arrive_m = 0\n" }) {
			host::TdmBotConfig c;
			c.skill_min = 0.42f;
			const core::Status r = host::parse_bot_tuning(bad, c);
			CHECK_MESSAGE(!r.ok(), bad);
			CHECK(c.skill_min == doctest::Approx(0.42f));
		}
		host::TdmBotConfig big;
		CHECK_FALSE(host::parse_bot_tuning(std::string(host::kMaxBotTuningBytes + 1, '#'), big).ok());
		CHECK_FALSE(host::parse_bot_tuning(std::string(host::kMaxBotTuningLines, '\n'), big).ok());
		CHECK(host::parse_bot_tuning(std::string(host::kMaxBotTuningLines - 1, '\n'), big).ok());
		CHECK_FALSE(host::load_bot_tuning("no/such/bot_tuning.cfg", big).ok());
	}

	TEST_CASE("brain on a fake ServerView: the pre-round idle, sight through the open only, turning onto the target") {
		const std::unique_ptr<sim::CollisionWorld> world = wall_world();
		m2b_fake::FakeView view;
		view.world_ = world.get();
		view.slots.resize(4);
		view.teams = { 1, 2, 2, 1 };
		view.kits = { 0, 1, 2, 3 };
		view.slots[0] = soldier_at({ 0, 0, 0 }, 0.0f);
		view.slots[1] = soldier_at({ 0, 0, 25 });
		view.slots[2] = soldier_at({ 30, 0, 30 });
		view.slots[3] = soldier_at({ -3, 0, -3 });
		auto rec = std::make_shared<m2b_fake::Recorder>();
		auto cfg = std::make_shared<host::TdmBotConfig>();
		cfg->observer = rec;
		cfg->stats = std::make_shared<host::TdmBotStats>();
		host::TdmBrainHost h;
		h.view = &view;
		h.match_seed = 99;
		h.rules = true;
		core::Result<std::shared_ptr<host::TdmShared>> shared = host::make_tdm_shared(h, cfg);
		REQUIRE(shared.ok());
		CHECK(cfg->stats->walk_unavailable.load() == 1u);
		host::AiBotSource brain(*shared, 0);
		brain.admitted(0);

		view.restricted = true;
		view.phase_ = net::MatchPhase::PreRound;
		for (sim::Tick t = 1; t <= 30; ++t) {
			view.now = t - 1;
			view.slots[0].tick = t - 1;
			const sim::Command c = brain.command(t, view.slots[0]);
			CHECK(c.buttons == 0u);
			CHECK(c.move_forward == 0);
			CHECK(c.move_right == 0);
		}
		CHECK(rec->activities.back() == host::TdmBotActivity::Restricted);
		CHECK(rec->detected_pairs.empty());

		view.restricted = false;
		view.phase_ = net::MatchPhase::InRound;
		float yaw = 0.0f;
		for (sim::Tick t = 31; t <= 150; ++t) {
			view.now = t - 1;
			const sim::Command c = brain.command(t, view.slots[0]);
			yaw = sim::yaw_radians(c.yaw);
			view.slots[0].yaw = c.yaw;
			view.slots[0].pitch = c.pitch;
		}
		REQUIRE(rec->detected_pairs.size() == 1u);
		CHECK(rec->detected_pairs[0].first == 0);
		CHECK(rec->detected_pairs[0].second == 2);
		CHECK(rec->activities.back() == host::TdmBotActivity::Fight);
		const float want = std::atan2(30.0f, 30.0f);
		float diff = std::fmod(std::fabs(yaw - want), sim::kTwoPi);
		diff = std::min(diff, sim::kTwoPi - diff);
		CHECK(diff < 6.0f * kDeg);
		CHECK(cfg->stats->perception_updates.load() >= 19u);
		CHECK(cfg->stats->detections.load() == 1u);
		CHECK(cfg->stats->last_brains_ns.load() > 0u);

		auto cfg2 = std::make_shared<host::TdmBotConfig>();
		core::Result<std::shared_ptr<host::TdmShared>> shared2 = host::make_tdm_shared(h, cfg2);
		REQUIRE(shared2.ok());
		host::AiBotSource a(*shared2, 0), b(*host::make_tdm_shared(h, cfg2), 0);
		a.admitted(0);
		b.admitted(0);
		view.slots[0] = soldier_at({ 0, 0, 0 }, 0.0f);
		uint64_t ha = 1469598103934665603ull, hb = ha;
		for (sim::Tick t = 200; t <= 320; ++t) {
			view.now = t - 1;
			const sim::Command ca = a.command(t, view.slots[0]);
			const sim::Command cb = b.command(t, view.slots[0]);
			ha = (ha ^ (uint64_t(ca.yaw) << 32 | uint64_t(uint16_t(ca.pitch)) << 16 | ca.buttons)) * 1099511628211ull;
			hb = (hb ^ (uint64_t(cb.yaw) << 32 | uint64_t(uint16_t(cb.pitch)) << 16 | cb.buttons)) * 1099511628211ull;
		}
		CHECK(ha == hb);

		sim::SoldierState dead = view.slots[0];
		dead.flags = 0;
		dead.mode = sim::MoveMode::Dead;
		const sim::Command cd = brain.command(400, dead);
		CHECK(cd.buttons == 0u);
		CHECK(rec->activities.back() == host::TdmBotActivity::Dead);
	}

	TEST_CASE("add_tdm_bots on the synthetic floor without rules: M2a teams, every bot alive, no rays through walls") {
		const std::shared_ptr<const host::SharedLevel> level = tg::synthetic_floor();
		REQUIRE(level != nullptr);
		host::GameServer server;
		host::GameServerConfig cfg;
		cfg.level = level;
		cfg.max_slots = 16;
		auto bots = std::make_shared<host::TdmBotConfig>();
		bots->count = 8;
		bots->stats = std::make_shared<host::TdmBotStats>();
		cfg.tdm_bots = bots;
		REQUIRE(server.init(cfg).ok());
		CHECK(bots->stats->bots.load() == 8u);
		CHECK(bots->stats->walk_unavailable.load() == 1u);
		const host::ServerView &view = server.view();
		uint32_t team1 = 0, team2 = 0;
		for (uint8_t s = 0; s < 8; ++s) {
			team1 += view.team(s) == 1 ? 1u : 0u;
			team2 += view.team(s) == 2 ? 1u : 0u;
		}
		CHECK(team1 == 4u);
		CHECK(team2 == 4u);
		std::vector<host::OutDatagram> out;
		for (int i = 0; i < 240; ++i) {
			out.clear();
			server.tick(out);
		}
		for (uint8_t s = 0; s < 8; ++s) {
			CHECK(view.alive(s));
		}
		CHECK(bots->stats->perception_updates.load() >= 8u * 39u);
		CHECK(bots->stats->detections.load() > 0u);

		host::GameServer full;
		host::GameServerConfig fc = cfg;
		auto many = std::make_shared<host::TdmBotConfig>();
		many->count = 40;
		many->stats = std::make_shared<host::TdmBotStats>();
		fc.tdm_bots = many;
		fc.max_slots = 20;
		REQUIRE(full.init(fc).ok());
		CHECK(many->stats->bots.load() == 20u);

		host::GameServer bad;
		host::GameServerConfig bc = cfg;
		auto tuned = std::make_shared<host::TdmBotConfig>();
		tuned->count = 2;
		tuned->tuning_cfg = "no/such/bot_tuning.cfg";
		bc.tdm_bots = tuned;
		CHECK_FALSE(bad.init(bc).ok());
	}
}

namespace {

[[maybe_unused]] tt::TdmRunConfig base_config(uint64_t p_seed, uint32_t p_bots, bool p_peer, int p_kill_limit) {
	tt::TdmRunConfig c;
	c.seed = p_seed;
	c.bots = p_bots;
	c.peer = p_peer;
	c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	c.rules.deploy_screen = false;
	c.rules.revive = true;
	c.rules.kill_limit_override = p_kill_limit;
	c.rules.preround_override_s = 0.0f;
	c.rules.max_players = 32;
	c.configure_bots = [](host::TdmBotConfig &r_bots) { r_bots.tuning_cfg = OPENBF3_BOT_TUNING_CFG; };
	return c;
}

[[maybe_unused]] void print_result(const char *p_row, const tt::TdmRunResult &p_r) {
	std::printf("%s: ticks %u, round over %d at %u, winner %u, kills %d / %d, deaths %u / %u, K/D %.3f, revives %u, stuck %u, sight %u, "
				"stale aim %u, blind %u, spawns scored %u / fallback %u / enemy within 10 m %u, init %.2f s, run %.2f s\n",
			p_row, p_r.ticks, int(p_r.round_over), p_r.round_over_tick, unsigned(p_r.winner), p_r.kills[1], p_r.kills[2], p_r.deaths[1], p_r.deaths[2],
			p_r.team_kd, p_r.revives, p_r.stuck_bots, p_r.sight_violations, p_r.stale_aim_shots, p_r.blind_fire, p_r.spawns_scored, p_r.spawns_fallback,
			p_r.spawns_enemy_within_10m, p_r.seconds_init, p_r.seconds_run);
}

[[maybe_unused]] void print_checks(const char *p_row, const tt::BotChecks &p_checks, bool p_slots = true) {
	uint64_t total = 0;
	for (const uint64_t n : p_checks.activity_ticks) {
		total += n;
	}
	std::printf("%s: detections %llu, aimed fire ticks %llu, blind fire ticks %llu, stuck windows %llu / %llu; bot-ticks:", p_row,
			(unsigned long long)p_checks.detections, (unsigned long long)p_checks.aimed_fire_ticks, (unsigned long long)p_checks.blind_fire_ticks,
			(unsigned long long)p_checks.stuck_windows, (unsigned long long)p_checks.windows);
	for (size_t i = 0; i < p_checks.activity_ticks.size(); ++i) {
		std::printf(" %s %.1f%%", host::tdm_bot_activity_name(host::TdmBotActivity(i)), total > 0 ? 100.0 * double(p_checks.activity_ticks[i]) / double(total) : 0.0);
	}
	std::printf("\n");
	std::printf("%s: bots per team at the end: %u / %u (no team %u); gunplay: shots %llu, hits %llu, deaths %llu, protected refused %llu, team damage "
				"dropped %llu\n",
			p_row, p_checks.team_slots[1], p_checks.team_slots[2], p_checks.team_slots[0], (unsigned long long)p_checks.gunplay.shots,
			(unsigned long long)p_checks.gunplay.hits, (unsigned long long)p_checks.gunplay.deaths, (unsigned long long)p_checks.gunplay.protected_refused,
			(unsigned long long)p_checks.gunplay.team_damage_dropped);
	for (uint8_t team = 1; team <= 2 && p_slots; ++team) {
		const double sn = std::max<double>(p_checks.spawn_n[team], 1.0), dn = std::max<double>(p_checks.death_n[team], 1.0);
		std::printf("%s: team %u: %u spawns around (%.1f, %.1f), %u deaths around (%.1f, %.1f)\n", p_row, unsigned(team), p_checks.spawn_n[team],
				p_checks.spawn_x[team] / sn, p_checks.spawn_z[team] / sn, p_checks.death_n[team], p_checks.death_x[team] / dn, p_checks.death_z[team] / dn);
		std::printf("%s: team %u slot:kit:kills/deaths:", p_row, unsigned(team));
		for (const net::PlayerRow &row : p_checks.players) {
			if (row.team == team) {
				std::printf(" %u:%u:%u/%u", unsigned(row.slot), unsigned(row.kit), unsigned(row.kills), unsigned(row.deaths));
			}
		}
		std::printf("\n");
		for (const net::PlayerRow &row : p_checks.players) {
			if (row.team == team && row.kills == 0 && row.deaths == 0 && row.slot < p_checks.last_position.size()) {
				const sim::Vec3 &at = p_checks.last_position[row.slot];
				std::printf("%s:   slot %u never killed nor died: at (%.1f, %.1f, %.1f), %s\n", p_row, unsigned(row.slot), at[0], at[1], at[2],
						host::tdm_bot_activity_name(p_checks.last[row.slot]));
			}
		}
	}
	for (const tt::BotChecks::StuckReport &st : p_checks.stuck_reports) {
		std::printf("%s: stuck slot %u at tick %u, (%.1f, %.1f, %.1f), farthest %.2f m; window ticks:", p_row, unsigned(st.slot), st.tick, st.position[0],
				st.position[1], st.position[2], st.farthest);
		for (size_t i = 0; i < st.acts.size(); ++i) {
			if (st.acts[i] != 0) {
				std::printf(" %s %u", host::tdm_bot_activity_name(host::TdmBotActivity(i)), st.acts[i]);
			}
		}
		std::printf("\n");
	}
	if (p_checks.stats != nullptr) {
		const host::TdmBotStats &s = *p_checks.stats;
		const auto v = [](const auto &p_a) { return (unsigned long long)p_a.load(std::memory_order_relaxed); };
		std::printf("%s: stats: walk unavailable %llu, retargets %llu, bursts %llu, blind %llu, grenades %llu, revive attempts %llu / shots %llu / "
					"aborts %llu, retreats %llu, reloads %llu, sidearm %llu, knife %llu, walker resets %llu, heat goals %llu, heard %llu, hunts given up %llu, off-main bot-ticks %llu\n",
				p_row, v(s.walk_unavailable), v(s.retargets), v(s.bursts), v(s.blind_fire), v(s.grenades), v(s.revive_attempts), v(s.revive_shots),
				v(s.revive_aborts), v(s.retreats), v(s.reloads), v(s.sidearm_switches), v(s.knife_attacks), v(s.walker_resets), v(s.heat_goals), v(s.heard), v(s.hunts_given_up), v(s.off_main_ticks));
		std::printf("%s: brain: %llu of %llu bots on AiBotSource, %llu decisions, %llu of them on a sense change\n",
				p_row, v(s.bf2_bots), v(s.bots), v(s.decisions), v(s.unplanned_decisions));
	}
}

using bf3::core::nearest_rank;
}

TEST_SUITE("host-tdm-round") {
	TEST_CASE("E1: a TDM round to 100 kills through the interpreter, 1 peer + 31 bots, then the restart in place") {
		if constexpr (!kRuNtLanded) {
			pending_skip("host.tdm.round");
		} else {
			tt::TdmLevel *level = tt::load_tdm_level_once();
			if (level == nullptr) {
				return;
			}
			tt::TdmRunConfig c = base_config(1, 31, true, 0);
			c.max_ticks = 21600 + 3600 + 300;
			c.after_round_ticks = 3600 + 240;

			std::vector<sim::logic::RoundNatives::NativeCoverage> coverage;
			std::vector<net::MatchState> peer_states;
			std::optional<net::MatchState> server_over;
			uint64_t peer_decode_errors = 0;
			const core::gameplay::LogicGraph *graph = level->shared->logic.get();
			size_t timeline_seen = 0;
			std::vector<sim::logic::NativeTimelineEntry> spawn_inputs;
			sim::Tick unspawn_seen = 0;
			sim::Tick restart_seen = 0;
			sim::Tick restart_announced = 0;
			c.on_tick = [&](sim::Tick p_tick, host::GameServer &r_server, tt::ScriptedPeer *p_peer) {
				if (const host::TdmRules *rules = r_server.rules(); rules != nullptr) {
					if (unspawn_seen == 0 && rules->match_state().unspawn_tick != 0) {
						unspawn_seen = rules->match_state().unspawn_tick;
						restart_announced = rules->match_state().restart_tick;
					}
					if (unspawn_seen != 0 && restart_seen == 0 && rules->phase() != net::MatchPhase::RoundOver) {
						restart_seen = p_tick;
					}
					const std::span<const sim::logic::RoundNatives::NativeCoverage> cov = rules->natives().coverage();
					coverage.assign(cov.begin(), cov.end());
					if (!server_over && rules->phase() == net::MatchPhase::RoundOver) {
						server_over = rules->match_state();
					}

					const std::span<const sim::logic::NativeTimelineEntry> tl = rules->natives().timeline();
					for (; timeline_seen < tl.size(); ++timeline_seen) {
						const sim::logic::NativeTimelineEntry &e = tl[timeline_seen];
						if (e.what == sim::logic::NativeTimelineEntry::What::Input && graph != nullptr && e.node < graph->nodes().size() &&
								graph->nodes()[e.node].type == core::gameplay::LogicTypeId::CharacterSpawn && spawn_inputs.size() < 16) {
							spawn_inputs.push_back(e);
						}
					}
				}
				if (p_peer != nullptr) {
					for (size_t i = peer_states.size(); i < p_peer->match_states.size(); ++i) {
						peer_states.push_back(p_peer->match_states[i]);
					}
					peer_decode_errors = p_peer->decode_errors;
				}
			};
			const std::shared_ptr<tt::BotChecks> checks = tt::attach_bot_checks(c, level);
			core::Result<tt::TdmRunResult> r = tt::run_tdm(*level, c);
			REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
			if (!r.ok()) {
				return;
			}
			tt::apply_bot_checks(*checks, *r);
			print_result("host.tdm.round", *r);
			print_checks("host.tdm.round", *checks, false);
			CHECK(r->failure.empty());
			CHECK_FALSE(r->runtime_faulted);
			CHECK(r->rules.runtime_faults == 0u);

			using LT = core::gameplay::LogicTypeId;
			const LT tdm_types[] = { LT::KillCounter, LT::PreRound, LT::RoundOver, LT::HumanPlayer, LT::AutoTeam, LT::InputRestriction, LT::LevelControl,
				LT::CharacterSpawn, LT::AlternateSpawn };
			CHECK_FALSE(coverage.empty());
			uint32_t tdm_handled = 0, tdm_recorded = 0, other_recorded = 0;
			for (const sim::logic::RoundNatives::NativeCoverage &row : coverage) {
				const bool tdm = std::find(std::begin(tdm_types), std::end(tdm_types), row.type) != std::end(tdm_types);
				if (!tdm) {
					other_recorded += row.recorded;
					continue;
				}
				tdm_handled += row.handled;
				tdm_recorded += row.recorded;
				CHECK_MESSAGE(row.recorded == 0u, "LogicTypeId " << int(row.type) << " recorded " << row.recorded);
			}
			std::printf("host.tdm.round: natives coverage: %zu types, TDM deliveries handled %u, recorded %u; other types recorded %u\n", coverage.size(),
					tdm_handled, tdm_recorded, other_recorded);
			for (const sim::logic::NativeTimelineEntry &e : spawn_inputs) {
				std::printf("host.tdm.round: CharacterSpawn node %u input port %lld at tick %u went to the stubs\n", e.node, (long long)e.value, e.tick);
			}
			CHECK(tdm_handled > 0u);
			CHECK(peer_decode_errors == 0u);
			REQUIRE(r->round_over);
			REQUIRE((r->winner == 1 || r->winner == 2));
			if (!r->round_over || (r->winner != 1 && r->winner != 2)) {
				return;
			}
			CHECK(r->round_over_tick <= 21600u);
			CHECK(r->rules.rounds == 1u);
			CHECK(r->rules.restarts >= 1u);

			REQUIRE(server_over.has_value());
			if (!server_over) {
				return;
			}
			const int w = int(r->winner) - 1;
			CHECK(server_over->winner == r->winner);
			CHECK(server_over->kills[size_t(w)] >= 100);
			CHECK(server_over->kills[size_t(1 - w)] < 100);

			const auto over = std::find_if(peer_states.begin(), peer_states.end(), [](const net::MatchState &p_s) { return p_s.phase == net::MatchPhase::RoundOver; });
			REQUIRE(over != peer_states.end());
			if (over == peer_states.end()) {
				return;
			}
			CHECK(over->winner == server_over->winner);
			CHECK(over->kills == server_over->kills);
			const auto restarted = std::find_if(over, peer_states.end(), [](const net::MatchState &p_s) { return p_s.phase != net::MatchPhase::RoundOver; });
			REQUIRE(restarted != peer_states.end());
			if (restarted == peer_states.end()) {
				return;
			}
			std::printf("host.tdm.round: rules at RoundOver: winner %u, %d / %d; peer MatchStates %zu: RoundOver winner %u, %d / %d; first after the "
						"restart: phase %d, %d / %d (run end %d / %d)\n",
					unsigned(server_over->winner), server_over->kills[0], server_over->kills[1], peer_states.size(), unsigned(over->winner), over->kills[0],
					over->kills[1], int(restarted->phase), restarted->kills[0], restarted->kills[1], peer_states.back().kills[0], peer_states.back().kills[1]);
			CHECK(restarted->kills[0] == 0);
			CHECK(restarted->kills[1] == 0);

			REQUIRE(level->shared->eor != nullptr);
			const sim::Tick eor_ticks =
					sim::Tick(std::lround(double(level->shared->eor->restart_delay_s) * double(sim::kSimHz)));
			std::printf("host.tdm.round: unspawn at %u, restart announced for %u, the restart in place at %u "
						"(end-of-round delay %.1f s from "
						"uiendofroundlogic)\n",
					unspawn_seen, restart_announced, restart_seen, double(level->shared->eor->restart_delay_s));
			CHECK(unspawn_seen != 0u);
			CHECK(restart_announced == unspawn_seen + eor_ticks);
			CHECK(restart_seen == restart_announced);
		}
	}
	TEST_CASE("host-tdm-round: install unchanged") {
		if constexpr (kRuNtLanded) {
			tt::TdmLevel *level = tt::load_tdm_level_once();
			if (level != nullptr) {
				tt::check_install_unchanged(*level);
			}
		} else {
			pending_skip("host.tdm.round");
		}
	}
}

TEST_SUITE("host-tdm-bots") {
	TEST_CASE("E2: bots-only TDM, seeds 1-3 to 50 kills: kills, team K/D about 1, no sight through walls, no stuck bot, revives") {
		if constexpr (!kRuNtLanded) {
			pending_skip("host.tdm.bots");
		} else {
			tt::TdmLevel *level = tt::load_tdm_level_once();
			if (level == nullptr) {
				return;
			}
			struct Run {
				std::string error;
				tt::TdmRunResult result;
				uint32_t stuck_m2a = 0;
				std::shared_ptr<tt::BotChecks> checks;
			};
			const std::vector<Run> runs = core::parallel_map<Run>(3, [&](size_t i) {
				Run run;
				tt::TdmRunConfig c = base_config(uint64_t(i + 1), 32, false, 50);
				c.after_round_ticks = 0;
				const std::shared_ptr<tt::BotChecks> checks = tt::attach_bot_checks(c, level);
				core::Result<tt::TdmRunResult> r = tt::run_tdm(*level, c);
				if (!r) {
					run.error = r.error().message;
					return run;
				}
				run.result = std::move(*r);
				tt::apply_bot_checks(*checks, run.result, &run.stuck_m2a);
				run.checks = checks;
				return run;
			}, 3);
			int32_t k1 = 0, k2 = 0;
			uint32_t revives = 0;
			for (size_t i = 0; i < runs.size(); ++i) {
				REQUIRE_MESSAGE(runs[i].error.empty(), runs[i].error);
				const tt::TdmRunResult &r = runs[i].result;
				std::printf("host.tdm.bots seed %zu: ", i + 1);
				print_result("", r);
				std::printf("  (run_tdm's own stuck count, without the exemptions: %u)\n", runs[i].stuck_m2a);
				if (runs[i].checks != nullptr) {
					print_checks("  checks", *runs[i].checks);
				}
				CHECK(r.failure.empty());
				CHECK(r.round_over);
				CHECK(r.sight_violations == 0u);
				CHECK(r.stale_aim_shots == 0u);
				CHECK(r.stuck_bots == 0u);
				CHECK(r.spawns_enemy_within_10m == 0u);
				k1 += r.kills[1];
				k2 += r.kills[2];
				revives += std::max<uint32_t>(r.revives, uint32_t(r.rules.revives));
			}
			const double kd = k2 > 0 ? double(k1) / double(k2) : 0.0;
			std::printf("host.tdm.bots pooled: kills %d / %d, team K/D %.3f, revives %u\n", k1, k2, kd, revives);
			CHECK(kd >= 0.75);
			CHECK(kd <= 1.33);
			CHECK(revives >= 1u);
		}
	}
	TEST_CASE("host-tdm-bots: install unchanged") {
		if constexpr (kRuNtLanded) {
			tt::TdmLevel *level = tt::load_tdm_level_once();
			if (level != nullptr) {
				tt::check_install_unchanged(*level);
			}
		} else {
			pending_skip("host.tdm.bots");
		}
	}
}

TEST_SUITE("host-tdm-replay") {
	TEST_CASE("E6: two runs of one seed replay tick for tick, bot commands included, and match the recorded hashes; another seed differs") {
		if constexpr (!kRuNtLanded) {
			pending_skip("host.tdm.replay");
		} else {
			tt::TdmLevel *level = tt::load_tdm_level_once();
			if (level == nullptr) {
				return;
			}
			const uint64_t seeds[3] = { 11, 11, 12 };
			struct Run {
				std::string error;
				tt::TdmRunResult result;
			};
			const std::vector<Run> runs = core::parallel_map<Run>(3, [&](size_t i) {
				Run run;
				tt::TdmRunConfig c = base_config(seeds[i], 32, true, 20);
				c.after_round_ticks = 600;
				c.hashes = true;
				c.record_decisions = true;
				core::Result<tt::TdmRunResult> r = tt::run_tdm(*level, c);
				if (!r) {
					run.error = r.error().message;
				} else {
					run.result = std::move(*r);
				}
				return run;
			}, 3);
			for (const Run &run : runs) {
				REQUIRE_MESSAGE(run.error.empty(), run.error);
				CHECK(run.result.round_over);
			}
			const tt::TdmRunResult &a = runs[0].result, &b = runs[1].result, &c = runs[2].result;
			size_t differ = 0;
			const size_t n = std::min(a.world_hashes.size(), b.world_hashes.size());
			for (size_t t = 0; t < n; ++t) {
				differ += a.world_hashes[t] != b.world_hashes[t] ? 1u : 0u;
			}
			size_t differ_other = 0;
			for (size_t t = 0; t < std::min(a.world_hashes.size(), c.world_hashes.size()); ++t) {
				differ_other += a.world_hashes[t] != c.world_hashes[t] ? 1u : 0u;
			}
			std::printf("host.tdm.replay: %zu / %zu ticks differ (same seed), %zu differ (other seed); trace %016llx / %016llx / %016llx; match bytes "
						"%016llx / %016llx\n",
					differ, n, differ_other, (unsigned long long)a.trace_hash, (unsigned long long)b.trace_hash, (unsigned long long)c.trace_hash,
					(unsigned long long)a.match_bytes_hash, (unsigned long long)b.match_bytes_hash);
			CHECK(n > 0u);
			CHECK(a.world_hashes.size() == b.world_hashes.size());
			CHECK(differ == 0u);
			CHECK(a.trace_hash == b.trace_hash);
			CHECK(a.match_bytes_hash == b.match_bytes_hash);
			CHECK(a.kills == b.kills);
			CHECK(differ_other > 0u);

			const size_t decision_ticks = std::min(a.decision_hashes.size(), b.decision_hashes.size());
			size_t decisions_differ = 0;
			for (size_t t = 0; t < decision_ticks; ++t) {
				decisions_differ += a.decision_hashes[t] != b.decision_hashes[t] ? 1u : 0u;
			}
			size_t decisions_differ_other = 0;
			for (size_t t = 0; t < std::min(a.decision_hashes.size(), c.decision_hashes.size()); ++t) {
				decisions_differ_other += a.decision_hashes[t] != c.decision_hashes[t] ? 1u : 0u;
			}
			const auto digest = [](const std::vector<uint64_t> &p_hashes) {
				uint64_t h = 0xcbf29ce484222325ull;
				for (const uint64_t x : p_hashes) {
					for (size_t i = 0; i < sizeof(x); ++i) {
						h ^= (x >> (8 * i)) & 0xFFu;
						h *= 0x100000001b3ull;
					}
				}
				return h;
			};
			std::printf("host.tdm.replay: seed %llu, %u ticks, round over at %u: %llu bot commands over %zu ticks, %zu / %zu ticks differ (same seed), %zu "
						"differ (other seed); decisions %016llx / %016llx / %016llx; world %016llx\n",
					(unsigned long long)seeds[0], a.ticks, a.round_over_tick, (unsigned long long)a.decisions, a.decision_hashes.size(), decisions_differ,
					decision_ticks, decisions_differ_other, (unsigned long long)a.decision_hash, (unsigned long long)b.decision_hash,
					(unsigned long long)c.decision_hash, (unsigned long long)digest(a.world_hashes));
			CHECK(decision_ticks > 0u);
			CHECK(a.decision_hashes.size() == b.decision_hashes.size());
			CHECK(decisions_differ == 0u);
			CHECK(a.decision_hash == b.decision_hash);
			CHECK(a.decisions == b.decisions);
			CHECK(decisions_differ_other > 0u);
			CHECK(a.decision_hash == kReplayDecisionHash);
			CHECK(digest(a.world_hashes) == kReplayWorldHash);
		}
	}
	TEST_CASE("host-tdm-replay: install unchanged") {
		if constexpr (kRuNtLanded) {
			tt::TdmLevel *level = tt::load_tdm_level_once();
			if (level != nullptr) {
				tt::check_install_unchanged(*level);
			}
		} else {
			pending_skip("host.tdm.replay");
		}
	}
}

TEST_SUITE("host-tdm-perf") {
	TEST_CASE("E8: 64 bots TDM, 120 s: perception <= 0.2 ms, brains <= 1.5 ms, server tick <= 4 ms at p99") {
		if constexpr (!kRuNtLanded) {
			pending_skip("host.tdm.perf");
		} else {
			tt::TdmLevel *level = tt::load_tdm_level_once();
			if (level == nullptr) {
				return;
			}
			tt::TdmRunConfig c = base_config(5, 64, false, 100000);
			c.rules.max_players = 64;
			c.max_ticks = 600 + 7200;
			c.after_round_ticks = 0;
			c.hashes = false;
			const std::shared_ptr<tt::BotChecks> checks = tt::attach_bot_checks(c, level);
			core::Result<tt::TdmRunResult> r = tt::run_tdm(*level, c);
			REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
			if (!r.ok()) {
				return;
			}
			tt::apply_bot_checks(*checks, *r);
			const auto window = [](const std::vector<double> &p_v) {
				return p_v.size() > 600 ? std::vector<double>(p_v.begin() + 600, p_v.end()) : p_v;
			};
			const double perception = nearest_rank(window(r->perception_ms), 0.99);
			const double brains = nearest_rank(window(r->brains_ms), 0.99);
			const double tick = nearest_rank(window(r->tick_ms), 0.99);
			std::printf("host.tdm.perf: 64 bots, %u ticks: p99 perception %.3f ms, brains %.3f ms, server tick %.3f ms; kills %d / %d\n", r->ticks, perception,
					brains, tick, r->kills[1], r->kills[2]);
#if !defined(OPENBF3_TIMING_REPORT_ONLY)
			CHECK(perception <= 0.2);
			CHECK(brains <= 1.5);
			CHECK(tick <= 4.0);
#endif
		}
	}
	TEST_CASE("host-tdm-perf: install unchanged") {
		if constexpr (kRuNtLanded) {
			tt::TdmLevel *level = tt::load_tdm_level_once();
			if (level != nullptr) {
				tt::check_install_unchanged(*level);
			}
		} else {
			pending_skip("host.tdm.perf");
		}
	}
}

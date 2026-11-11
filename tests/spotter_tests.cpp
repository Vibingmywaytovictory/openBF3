#include "bf3/host/spotter.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/commo.h"
#include "bf3/sim/spotting.h"
#include "bf3/sim/static_world.h"
#include "server_spotting.h"

#include <doctest/doctest.h>

#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace bf3;
namespace sp = bf3::sim::spotting;
namespace cm = bf3::sim::commo;

sp::SpotterDef retail() {
	sp::SpotterDef d;
	d.fov_deg = 4.0f;
	d.team_order_fov_deg = 15.0f;
	d.distance = 1200.0f;
	d.passive_interval = 1.0f;
	d.passive_required = 1.0f;
	d.cooldown_history_time = 3.0f;
	d.cooldown_allowed = 3;
	d.order_heal_percent = 60.0f;
	d.order_need_ammo_percent = 50.0f;
	d.order_repair_percent = 75.0f;
	d.pickup_order_distance = 50.0f;
	d.ammo_order_distance = 30.0f;
	d.repair_order_distance = 50.0f;
	return d;
}

core::gameplay::SoldierDef soldier() {
	core::gameplay::SoldierDef d;
	d.radius = 0.35f;
	for (const auto &[pose, height] : { std::pair{ core::gameplay::SoldierPose::Stand, 1.8f },
				 std::pair{ core::gameplay::SoldierPose::Crouch, 1.15f },
				 std::pair{ core::gameplay::SoldierPose::Prone, 0.3f } }) {
		core::gameplay::PoseDef p;
		p.pose = pose;
		p.height = height;
		d.poses.push_back(p);
	}
	return d;
}

host::SpotSubject subject(uint8_t p_slot, uint8_t p_team, float p_x, float p_z, sim::Pose p_pose = sim::Pose::Stand) {
	host::SpotSubject s;
	s.slot = p_slot;
	s.team = p_team;
	REQUIRE(host::place_spot_body(s, soldier(), p_pose, { p_x, 0.1f, p_z }));
	s.party.position = s.centre;
	s.party.health = 100.0f;
	s.party.max_health = 100.0f;
	return s;
}

sp::SpottingDef server_def() {
	sp::SpottingDef def;
	def.spotter_present = true;
	def.target_present = true;
	def.spotter = retail();
	def.target.active_time = 7.0f;
	def.target.passive_time = 1.5f;
	return def;
}

std::unique_ptr<sim::CollisionWorld> wall_world() {
	const sim::Vec3 a{ -3.0f, 0.0f, 20.0f };
	const sim::Vec3 b{ 3.0f, 0.0f, 20.0f };
	const sim::Vec3 c{ 3.0f, 4.0f, 20.0f };
	const sim::Vec3 d{ -3.0f, 4.0f, 20.0f };
	std::vector<sim::StaticTriangle> t(4);
	t[0].v = { a, b, c };
	t[1].v = { a, c, d };
	t[2].v = { a, c, b };
	t[3].v = { a, d, c };
	core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes =
			sim::build_static_world(std::span<const sim::StaticTriangle>(t));
	REQUIRE(shapes.ok());
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(*shapes);
	REQUIRE(world.ok());
	return std::move(*world);
}

host::SpotterInput input(const std::vector<host::SpotSubject> &p_subjects, double p_clock, bool p_pressed) {
	host::SpotterInput in;
	in.clock = p_clock;
	in.dt = 1.0f / 60.0f;
	in.pressed = p_pressed;
	in.own_slot = 0;
	in.own_team = 1;
	in.eye = { 0.0f, 1.0f, 0.0f };
	in.view = { 0.0f, 0.0f, 1.0f };
	in.me.position = in.eye;
	in.me.health = 100.0f;
	in.me.max_health = 100.0f;
	in.subjects = p_subjects;
	return in;
}

}

TEST_SUITE("spotting-unit") {
	TEST_CASE("spotter: a spot body is the soldier's PhysicalRadius capsule over the current pose's height") {
		const host::SpotSubject stand = subject(3, 2, 0.0f, 40.0f);
		CHECK(stand.body.radius == 0.35f);
		CHECK(stand.body.a[1] == doctest::Approx(0.45f));
		CHECK(stand.body.b[1] == doctest::Approx(1.55f));
		CHECK(stand.centre[1] == doctest::Approx(1.0f));
		const host::SpotSubject crouch = subject(3, 2, 0.0f, 40.0f, sim::Pose::Crouch);
		CHECK(crouch.body.b[1] == doctest::Approx(0.9f));
		CHECK(crouch.centre[1] == doctest::Approx(0.675f));
		const host::SpotSubject prone = subject(3, 2, 0.0f, 40.0f, sim::Pose::Prone);
		CHECK(prone.body.a[1] == doctest::Approx(0.45f));
		CHECK(prone.body.b[1] == doctest::Approx(0.45f));
		CHECK(prone.centre[1] == doctest::Approx(0.45f));
		const std::vector<host::SpotSubject> lying{ prone };
		CHECK(host::spot_ray_hits({ -5.0f, 0.5f, 40.0f }, { 5.0f, 0.5f, 40.0f }, lying, 0, nullptr).size() == 1u);
		CHECK(host::spot_ray_hits({ -5.0f, 0.9f, 40.0f }, { 5.0f, 0.9f, 40.0f }, lying, 0, nullptr).empty());
		CHECK(host::spot_ray_hits({ -5.0f, 0.9f, 40.0f }, { 5.0f, 0.9f, 40.0f },
					  std::vector<host::SpotSubject>{ crouch }, 0, nullptr)
						.size() == 1u);
		host::SpotSubject none;
		core::gameplay::SoldierDef no_poses;
		no_poses.radius = 0.35f;
		CHECK_FALSE(host::place_spot_body(none, no_poses, sim::Pose::Stand, {}));
	}

	TEST_CASE("spotter: a press spots the enemy the crosshair ray meets first, a teammate in front is skipped") {
		const std::vector<host::SpotSubject> subjects{ subject(0, 1, 0.0f, 0.0f), subject(2, 1, 0.0f, 10.0f),
			subject(3, 2, 0.0f, 40.0f), subject(4, 2, 30.0f, 40.0f) };
		host::Spotter s(retail());
		const host::SpotterOutput out = s.update(input(subjects, 0.0, true));
		REQUIRE(out.spot.has_value());
		CHECK(out.spot->type == uint8_t(sp::SpotType::Active));
		CHECK(out.spot->targets == std::vector<uint8_t>{ 3 });
		CHECK_FALSE(out.refused);
		CHECK_FALSE(out.order.has_value());
	}

	TEST_CASE("spotter: presses beyond CoolDownAllowedSpotsWithinHistory inside CoolDownHistoryTime are refused") {
		const std::vector<host::SpotSubject> subjects{ subject(3, 2, 0.0f, 40.0f) };
		host::Spotter s(retail());
		for (int i = 0; i < 3; ++i) {
			CHECK(s.update(input(subjects, 0.5 * i, true)).spot.has_value());
		}
		const host::SpotterOutput refused = s.update(input(subjects, 1.6, true));
		CHECK(refused.refused);
		CHECK_FALSE(refused.spot.has_value());
		CHECK(s.update(input(subjects, 4.7, true)).spot.has_value());
	}

	TEST_CASE("spotter: the passive scan sends a Passive spot two updates after the scan at the retail 1 s / 1 s") {
		const std::vector<host::SpotSubject> subjects{ subject(3, 2, 0.0f, 40.0f) };
		host::Spotter s(retail());
		const host::SpotterOutput scan = s.update(input(subjects, 0.0, false));
		CHECK(scan.scanned);
		CHECK_FALSE(scan.spot.has_value());
		const host::SpotterOutput sent = s.update(input(subjects, 1.0 / 60.0, false));
		REQUIRE(sent.spot.has_value());
		CHECK(sent.spot->type == uint8_t(sp::SpotType::Passive));
		CHECK(sent.spot->targets == std::vector<uint8_t>{ 3 });
		CHECK_FALSE(s.update(input(subjects, 2.0 / 60.0, false)).spot.has_value());
	}

	TEST_CASE("spotter: the press also gives the teammate nearest the crosshair inside TeamOrderFov the contextual "
			  "order") {
		std::vector<host::SpotSubject> subjects{ subject(5, 1, 2.0f, 20.0f), subject(6, 1, 0.5f, 20.0f) };
		subjects[1].party.supplies.heals = true;
		subjects[0].party.supplies.heals = true;
		host::Spotter s(retail());
		host::SpotterInput in = input(subjects, 0.0, true);
		in.me.health = 40.0f;
		const host::SpotterOutput out = s.update(in);
		REQUIRE(out.order.has_value());
		CHECK(out.order->order == cm::OrderType::RequestHeal);
		CHECK(out.order->target == 6);
		CHECK(host::order_teammate(retail(), in) == std::optional<uint8_t>(6));
		in.view = { 1.0f, 0.0f, 0.0f };
		CHECK_FALSE(host::order_teammate(retail(), in).has_value());
	}
}

TEST_SUITE("spotting-unit") {
	TEST_CASE("spotter: bots press spot while an enemy stands in their cone and not Active, through the server's spot "
			  "rule") {
		host::ServerSpotting server;
		server.configure(8, server_def());
		const std::vector<host::SpotSubject> subjects{ subject(0, 1, 0.0f, 0.0f), subject(3, 2, 0.0f, 40.0f),
			subject(4, 2, 30.0f, 0.0f) };
		const std::vector<uint8_t> alive{ 1, 0, 0, 1, 1, 0, 0, 0 };
		const std::vector<host::BotEye> eyes{ { 0, 1, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
		server.step_bots(0.0, 1.0f / 60.0f, eyes, subjects, nullptr, alive);
		CHECK(server.target(3).state == sp::SpotState::Active);
		CHECK(server.target(3).spotter == 0);
		CHECK(server.target(4).state == sp::SpotState::None);
		CHECK(server.stats().spots_applied == 1u);
		server.step_bots(1.0 / 60.0, 1.0f / 60.0f, eyes, subjects, nullptr, alive);
		CHECK(server.stats().spot_requests == 1u);

		server.queue_order(3, net::OrderRequest{ cm::OrderType::RequestHeal, 4 });
		const auto queued = server.take_queued_orders();
		REQUIRE(queued.size() == 1u);
		const std::optional<host::OrderDelivery> d = server.order(queued[0].first, queued[0].second, alive);
		REQUIRE(d.has_value());
		CHECK(d->target == 4);
		CHECK(d->notice.issuer == 3);
		CHECK(server.stored_order(4).type == cm::OrderType::RequestHeal);
		CHECK(server.stored_order(4).serial == 1u);
		CHECK_FALSE(server.order(3, net::OrderRequest{ cm::OrderType::RequestHeal, 1 }, alive).has_value());
		CHECK(server.stats().orders_refused == 1u);
	}

	TEST_CASE("spotter: a bot presses spot only on an enemy it can see, so an enemy behind a wall does not use up its "
			  "presses") {
		host::ServerSpotting server;
		server.configure(8, server_def());
		const std::unique_ptr<sim::CollisionWorld> world = wall_world();
		std::vector<host::SpotSubject> subjects{ subject(0, 1, 0.0f, 0.0f), subject(3, 2, 0.0f, 40.0f) };
		const std::vector<uint8_t> alive{ 1, 0, 0, 1, 1, 0, 0, 0 };
		const std::vector<host::BotEye> eyes{ { 0, 1, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
		const float dt = 1.0f / 60.0f;
		int tick = 0;
		for (; tick < 240; ++tick) {
			server.step_bots(double(tick) * dt, dt, eyes, subjects, world.get(), alive);
		}
		CHECK(server.target(3).state == sp::SpotState::None);
		CHECK(server.stats().spots_applied == 0u);
		subjects.push_back(subject(4, 2, 0.0f, 10.0f));
		server.step_bots(double(tick) * dt, dt, eyes, subjects, world.get(), alive);
		++tick;
		CHECK(server.target(4).state == sp::SpotState::Active);
		CHECK(server.target(4).spotter == 0);
		for (; tick < 480; ++tick) {
			server.step_bots(double(tick) * dt, dt, eyes, subjects, world.get(), alive);
		}
		CHECK(server.target(3).state == sp::SpotState::None);
		CHECK(server.stats().spots_applied == 1u);
	}

	TEST_CASE("spotter: the cool-down is ready while fewer than CoolDownAllowedSpotsWithinHistory presses lie inside "
			  "CoolDownHistoryTime") {
		const sp::SpotterDef def = retail();
		sp::SpotCooldown c;
		for (int i = 0; i < 3; ++i) {
			CHECK(c.ready(0.5 * i, def));
			CHECK(c.press(0.5 * i, def));
		}
		CHECK_FALSE(c.ready(1.5, def));
		CHECK_FALSE(c.ready(3.0, def));
		CHECK(c.ready(3.01, def));
	}
}

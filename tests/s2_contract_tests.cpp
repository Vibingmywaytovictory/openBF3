#include <doctest/doctest.h>

#include "bf3/core/gameplay/vehicle_drive.h"
#include "bf3/core/level/vehicle_spawns.h"
#include "bf3/net/bit_stream.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vehicle_ghost.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim_harness/vehicle_course.h"
#include "bf3/sim_harness/vehicle_replay.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <type_traits>

namespace sim = bf3::sim;
namespace net = bf3::net;
namespace gp = bf3::core::gameplay;

static_assert(std::is_trivially_copyable_v<sim::VehicleCommand>);
static_assert(sizeof(sim::VehicleCommand) <= 16);
static_assert(std::is_trivially_copyable_v<sim::VehicleState>);
static_assert(std::is_trivially_copyable_v<sim::VehicleWheelState>);
static_assert(std::is_trivially_copyable_v<sim::VehicleGhostCodes>);
static_assert(std::is_trivially_copyable_v<sim::VehiclePlacement>);
static_assert(std::is_trivially_copyable_v<sim::VehicleContacts>);
static_assert(std::is_trivially_copyable_v<sim::VehicleHandling>);
static_assert(sim::kMaxVehicleWheels == 8);
static_assert(sim::kMaxVehicleWheels <= gp::kMaxDriveWheels);
static_assert(sim::kNoContactBody == 0xFFFFFFFFu);
static_assert((sim::vehicle_buttons::kHandbrake & ~sim::vehicle_buttons::kWireMask) == 0);

static_assert(sim::kVehicleObjectLayer == sim::kStaticLayerCount + 1);

static_assert(sim::vehicle_restore::kAll == 0x3FFu);
static_assert((sim::vehicle_restore::kGhost & ~sim::vehicle_restore::kAll) == 0);
static_assert((sim::vehicle_restore::kGhost & sim::vehicle_restore::kLambdas) == 0);
static_assert((sim::vehicle_restore::kGhost & sim::vehicle_restore::kWheelContacts) == 0);

static_assert((sim::vehicle_restore::kGhost & sim::vehicle_restore::kBodySleep) == 0);
static_assert((sim::vehicle_restore::kBody | sim::vehicle_restore::kController | sim::vehicle_restore::kControllerInputs |
					  sim::vehicle_restore::kWheels | sim::vehicle_restore::kWheelContacts | sim::vehicle_restore::kLambdas |
					  sim::vehicle_restore::kPitchRoll | sim::vehicle_restore::kStep | sim::vehicle_restore::kDriver |
					  sim::vehicle_restore::kBodySleep) ==
		sim::vehicle_restore::kAll);

static_assert(sim::kJoltBodyStreamBytes ==
		1 + sizeof(sim::VehicleBodyState::com_position) + sizeof(sim::VehicleBodyState::rotation) + sizeof(sim::VehicleBodyState::linear_velocity) +
				sizeof(sim::VehicleBodyState::angular_velocity) + sizeof(sim::VehicleBodyState::force) + sizeof(sim::VehicleBodyState::torque) +
				sizeof(sim::VehicleBodyState::sleep_test_spheres) + sizeof(sim::VehicleBodyState::sleep_test_timer) + 1);
static_assert(sizeof(sim::VehicleSleepSphere) == 16);
static_assert(sim::jolt_vehicle_constraint_stream_bytes(4) == 337);

static_assert(sim::kG1Ticks == sim::seconds_to_ticks(60.0f));
static_assert(sim::kG2ReplayTicks[0] == sim::seconds_to_ticks(0.1f));
static_assert(sim::kG2ReplayTicks[1] == sim::seconds_to_ticks(0.15f));
static_assert(sim::kG2SnapshotInterval * 30 == sim::kSimHz);
static_assert(sim::kG2MaxError == 0.02f);
static_assert(sim::kVehicleCourseKindCount == size_t(sim::VehicleCourseKind::SlopeIdle) + 1);
static_assert(sim::kVehicleRestoreModeCount == size_t(sim::VehicleRestoreMode::Full) + 1);
static_assert(sim::kVehicleSnapshotSectionCount == size_t(sim::VehicleSnapshotSection::Constraints) + 1);

static_assert(sim::kSettleCommand.brake == 255 && sim::kSettleCommand.throttle == 0 && sim::kSettleCommand.steer == 0 &&
		sim::kSettleCommand.buttons == 0);
static_assert(sim::kSettleDriftTicks < sim::kSettleTicks);

static_assert(sim::kVehicleActivity.size() == sim::kVehicleCourseKindCount);
static_assert(!sim::kVehicleActivity[size_t(sim::VehicleCourseKind::SlopeIdle)].must_stay_awake);
static_assert(sim::kVehicleActivity[size_t(sim::VehicleCourseKind::SlopeIdle)].max_creep > 0.0f);
static_assert(sim::kVehicleActivity[size_t(sim::VehicleCourseKind::Collision)].min_ticks_static_contact >= 30);
static_assert(sim::kVehicleActivity[size_t(sim::VehicleCourseKind::Circle)].min_yaw_turns > 0.0f);

static_assert(sim::kDefaultVehicleGhost.position_xz.min == net::quant::kPositionXZ.min);
static_assert(sim::kDefaultVehicleGhost.position_xz.step == net::quant::kPositionXZ.step);
static_assert(sim::kDefaultVehicleGhost.position_xz.bits == net::quant::kPositionXZ.bits);
static_assert(sim::kDefaultVehicleGhost.position_y.min == net::quant::kPositionY.min);
static_assert(sim::kDefaultVehicleGhost.position_y.step == net::quant::kPositionY.step);
static_assert(sim::kDefaultVehicleGhost.position_y.bits == net::quant::kPositionY.bits);

static_assert(sim::kDefaultVehicleGhost.orientation_bits == 11);
static_assert(sim::kDefaultVehicleGhost.linear_velocity.bits == 16 && sim::kDefaultVehicleGhost.linear_velocity.min == -512.0f);
static_assert(sim::kDefaultVehicleGhost.angular_velocity.bits == 13 && sim::kDefaultVehicleGhost.angular_velocity.min == -16.0f);
static_assert(double(sim::kDefaultVehicleGhost.linear_velocity.max_code()) * double(sim::kDefaultVehicleGhost.linear_velocity.step) >= 1023.9);
static_assert(double(sim::kDefaultVehicleGhost.angular_velocity.max_code()) * double(sim::kDefaultVehicleGhost.angular_velocity.step) >= 31.9);

static_assert(sim::kDefaultVehicleGhost.position_xz.bits * 2 + sim::kDefaultVehicleGhost.position_y.bits + 2 +
				3 * sim::kDefaultVehicleGhost.orientation_bits + 3 * sim::kDefaultVehicleGhost.linear_velocity.bits +
				3 * sim::kDefaultVehicleGhost.angular_velocity.bits + 1 ==
		183);

static_assert(sim::kDefaultVehicleGhost.send_suspension);
static_assert(440 - 4 * sim::kDefaultVehicleGhost.suspension_bits == 376);
static_assert(sim::kDefaultVehicleGhost.position_xz.encode(-5000.0f) == 0);
static_assert(sim::kDefaultVehicleGhost.position_xz.encode(5000.0f) == sim::kDefaultVehicleGhost.position_xz.max_code());
static_assert(sim::kDefaultVehicleGhost.engine_rpm.encode(5000.0f) == 20000);

TEST_CASE("s2 contract: ghost quantisers round-trip within half a step and clamp") {
	const sim::VehicleGhostQuantisation &q = sim::kDefaultVehicleGhost;
	const sim::GhostQuantizer all[] = { q.position_xz, q.position_y, q.linear_velocity, q.angular_velocity, q.wheel_spin, q.engine_rpm,
		q.clutch, q.gearbox_timer, q.driver_filter };
	for (const sim::GhostQuantizer &g : all) {
		const double lo = double(g.min);
		const double hi = double(g.min) + double(g.max_code()) * double(g.step);
		for (int i = 0; i <= 1000; ++i) {
			const float v = float(lo + (hi - lo) * double(i) / 1000.0);
			const float back = g.decode(g.encode(v));
			CHECK(std::fabs(double(back) - double(v)) <= double(g.step) * 0.5 + 1e-6 * std::fabs(double(v)) + 1e-9);
		}
		CHECK(g.encode(float(lo - 1000.0)) == 0);
		CHECK(g.encode(float(hi + 1000.0)) == g.max_code());
		CHECK(g.encode(std::nanf("")) == 0);
		CHECK(g.decode(g.max_code() + 1) == g.decode(g.max_code()));
	}

	CHECK(q.position_xz.encode(765.79f) < q.position_xz.max_code());
	CHECK(q.position_xz.encode(-158.91f) > 0);
	CHECK(q.position_y.encode(141.49f) < q.position_y.max_code());
}

TEST_CASE("s2 contract: default plain values") {
	const sim::VehicleCommand c;
	CHECK(c.throttle == 0);
	CHECK(c.brake == 0);
	CHECK(c.steer == 0);
	const sim::VehicleState s;
	CHECK(s.wheel_count == 0);
	CHECK(s.body.rotation[3] == 1.0f);
	CHECK(s.wheels[0].contact_body == sim::kNoContactBody);
	CHECK(s.body.active == 1);
	CHECK(s.body.allow_sleeping == 1);
	CHECK(s.body.sleep_test_timer == 0.0f);
	const gp::DriveTransform t;
	CHECK(t.right[0] == 1.0f);
	CHECK(t.up[1] == 1.0f);
	CHECK(t.forward[2] == 1.0f);
	const sim::VehicleTuning tuning;
	CHECK(tuning.static_sag_fraction == 7.0f / 16.0f);
	CHECK(tuning.damping_ratio == 0.45f);
	CHECK(tuning.steer_max_degrees == 32.0f);
	const sim::VehicleReplayOptions o;
	CHECK(o.ticks == sim::kG1Ticks);
	CHECK(o.snapshot_interval == sim::kG2SnapshotInterval);
	CHECK(o.replay_ticks.size() == sim::kG2ReplayTicks.size());
	CHECK(o.keep_hashes);
	CHECK(o.modes == (1u << sim::kVehicleRestoreModeCount) - 1u);
	CHECK(sim::VehicleContacts{}.chassis_vehicle_contacts == 0);
	const sim::VehicleCourseSearch search;
	CHECK(search.turnaround_radius > 0.0f);
	CHECK(search.max_queries_per_kind > 0);
	for (size_t k = 0; k < sim::kVehicleCourseKindCount; ++k) {
		const sim::VehicleActivityThresholds &a = sim::kVehicleActivity[k];
		if (k == size_t(sim::VehicleCourseKind::SlopeIdle)) {
			continue;
		}
		CHECK(a.must_stay_awake);

		CHECK((a.min_distance > 0.0f || a.min_yaw_turns > 0.0f || a.min_static_impacts > 0));
	}
}

#ifdef OPENBF3_VEHICLE_TUNING_CFG
TEST_CASE("s2 contract: vehicle_tuning.cfg names exactly the VehicleTuning keys") {

	const std::set<std::string> expected = { "gravity", "ground_friction", "static_sag_fraction", "damping_ratio", "suspension_min_fraction",
		"wheel_angular_damping", "brake_torque_per_force", "hand_brake_torque", "longitudinal_grip", "lateral_grip", "tester", "steer_model",
		"steer_max_degrees", "engine_inertia", "engine_angular_damping", "shift_up_rpm", "shift_down_rpm", "switch_time", "clutch_release_time",
		"switch_latency", "clutch_strength", "differential_limited_slip", "anti_roll_scale", "drag_scale", "max_pitch_roll_degrees",
		"stand_still_speed", "reverse_switch_speed", "collision_steps", "velocity_steps", "position_steps" };
	std::ifstream f(OPENBF3_VEHICLE_TUNING_CFG, std::ios::binary);
	REQUIRE(f.good());
	std::set<std::string> found;
	std::string s;
	size_t lines = 0;
	while (lines < 1000 && std::getline(f, s)) {
		++lines;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
			s.pop_back();
		}
		if (s.empty() || s[0] == '#') {
			continue;
		}
		const size_t eq = s.find(" = ");
		REQUIRE_MESSAGE(eq != std::string::npos, s);
		CHECK_MESSAGE(found.insert(s.substr(0, eq)).second, "duplicate key " << s);
	}
	CHECK(found == expected);
}
#endif

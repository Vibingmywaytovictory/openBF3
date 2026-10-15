// M1 interface contract (docs/design/m1.md §14): every cross-track header of milestone M1 compiles on its own
// set of includes, the plain types keep the properties the tracks rely on (trivially copyable states, the wire
// constants), and the inline helpers both sides share (command quantisation, ticks, quantisers) behave as the
// design says. No data, no Godot; nothing here calls a function a track has yet to implement.
#include <doctest/doctest.h>

#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/host/client_session.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_load.h"
#include "bf3/host/rest_check.h"
#include "bf3/host/server_host.h"
#include "bf3/net/bit_stream.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"
#include "bf3/render/level_scene.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace sim = bf3::sim;
namespace net = bf3::net;

// ---- compile-time contract ---------------------------------------------------------------------------------

static_assert(sim::kSimHz == 60);
static_assert(sim::seconds_to_ticks(0.3f) == 18);
static_assert(sim::seconds_to_ticks(0.5f) == 30);
static_assert(sim::seconds_to_ticks(0.7f) == 42);
static_assert(sim::seconds_to_ticks(1.05f) == 63);
static_assert(sim::seconds_to_ticks(1.0f) == 60); // JumpPenaltyTime
static_assert(sim::seconds_to_ticks(2.0f) == 120); // ImmortalTimeAfterSpawn
static_assert(sim::seconds_to_ticks(0.0f) == 0);
static_assert(std::is_trivially_copyable_v<sim::Command>);
static_assert(std::is_trivially_copyable_v<sim::SoldierState>);
static_assert(std::is_trivially_copyable_v<sim::MotorParams>);
static_assert(std::is_trivially_copyable_v<sim::SurfaceTag>);
static_assert(sizeof(sim::SurfaceTag) == 8);
static_assert(uint8_t(sim::Pose::Stand) == 0 && uint8_t(sim::Pose::Crouch) == 1 && uint8_t(sim::Pose::Prone) == 2);
static_assert(uint8_t(sim::StaticLayer::Collision) == 0 && uint8_t(sim::StaticLayer::Raycast) == 1 && uint8_t(sim::StaticLayer::Character) == 2);
static_assert(sim::query_mask::kMovement == (sim::query_mask::kCollision | sim::query_mask::kCharacter));
static_assert(sim::query_mask::kProjectile == (sim::query_mask::kCollision | sim::query_mask::kRaycast));
static_assert((sim::buttons::kEdgeMask & ~sim::buttons::kWireMask) == 0);
static_assert(net::kChannelCount == 3);
static_assert(net::kMaxDatagramBytes <= 1392 - 64); // ENet MTU minus its headers
static_assert(net::kCommandsPerPacket == 4);
static_assert(net::quant::kPositionXZ.bits + net::quant::kPositionY.bits + net::quant::kPositionXZ.bits == 60);
static_assert(net::quant::kPositionXZ.encode(-5000.0f) == 0);
static_assert(net::quant::kPositionXZ.encode(5000.0f) == net::quant::kPositionXZ.max_code());
// Plan §7.5: the largest infantry distance (2,252 m, XP5_002) is inside the XZ range.
static_assert(net::quant::kPositionXZ.encode(2252.0f) < net::quant::kPositionXZ.max_code());
static_assert(net::quant::kPositionXZ.encode(-2252.0f) > 0);
static_assert(std::is_trivially_copyable_v<net::OtherSoldier>);
static_assert(std::is_trivially_copyable_v<bf3::host::ClientStats>);
static_assert(std::is_trivially_copyable_v<bf3::host::PresentationFrame>);
static_assert(std::is_trivially_copyable_v<bf3::host::RestSample>);
static_assert(std::is_trivially_copyable_v<sim::PredictionEntry>);
static_assert(net::seq_newer(1, 0) && !net::seq_newer(0, 1) && !net::seq_newer(5, 5));
static_assert(net::seq_newer(2, 65530) && !net::seq_newer(65530, 2)); // wrap-around
static_assert(bf3::core::level::kCollisionSoupVersion >= 1);
static_assert(sim::idle_command(5, 7, -3, sim::Pose::Prone).tick == 5);
static_assert(sim::idle_command(5, 7, -3, sim::Pose::Prone).buttons == 0);
static_assert(uint8_t(bf3::host::LoadStage::Failed) + 1 == bf3::host::kLoadStageCount);
static_assert(uint8_t(net::DebugKind::Trace) == 5);
static_assert(net::kMaxTraceTicks >= 20 * sim::kSimHz); // E1's longest course (the 46 m drop and the transitions)
static_assert(std::is_same_v<decltype(bf3::host::ServerConfig{}.game), std::shared_ptr<const bf3::host::GameServerConfig>>);

TEST_CASE("m1 contract: yaw quantisation wraps and round-trips within half a step") {
	CHECK(sim::quantize_yaw(0.0f) == 0);
	CHECK(sim::quantize_yaw(sim::kPi * 0.5f) == 16384);
	CHECK(sim::quantize_yaw(-sim::kPi * 0.5f) == 49152);
	CHECK(sim::quantize_yaw(sim::kTwoPi) == 0);
	CHECK(sim::quantize_yaw(sim::kTwoPi * 3.0f + sim::kPi) == 32768);
	const float half_step = sim::kTwoPi / 65536.0f * 0.5f;
	for (int i = -720; i <= 720; i += 7) {
		const float r = float(i) * sim::kPi / 180.0f;
		float back = sim::yaw_radians(sim::quantize_yaw(r));
		float d = std::remainder(back - r, sim::kTwoPi);
		CHECK(std::fabs(d) <= half_step * 1.01f + 1e-6f);
	}
}

TEST_CASE("m1 contract: pitch and axes clamp and round-trip") {
	CHECK(sim::quantize_pitch(0.0f) == 0);
	CHECK(sim::quantize_pitch(sim::kPi) == sim::kPitchLimit);
	CHECK(sim::quantize_pitch(-sim::kPi) == -sim::kPitchLimit);
	CHECK(sim::quantize_pitch(sim::kPi * 0.25f) == 4096);
	CHECK(std::fabs(sim::pitch_radians(sim::quantize_pitch(0.3f)) - 0.3f) <= sim::kPi / 16384.0f);
	CHECK(sim::quantize_axis(1.0f) == 127);
	CHECK(sim::quantize_axis(-1.0f) == -127);
	CHECK(sim::quantize_axis(2.0f) == 127);
	CHECK(sim::quantize_axis(0.0f) == 0);
	CHECK(sim::axis_value(-128) == doctest::Approx(-1.0f));
	CHECK(sim::axis_value(127) == doctest::Approx(1.0f));
}

TEST_CASE("m1 contract: a repeated command keeps held buttons, clears edges and is marked") {
	sim::Command c;
	c.tick = 100;
	c.sequence = 7;
	c.buttons = sim::buttons::kJump | sim::buttons::kSprint;
	c.move_forward = 127;
	c.yaw = 1234;
	c.pose = sim::Pose::Crouch;
	const sim::Command r = sim::repeated_command(c, 101);
	CHECK(r.tick == 101);
	CHECK(r.sequence == 7);
	CHECK((r.buttons & sim::buttons::kJump) == 0);
	CHECK((r.buttons & sim::buttons::kSprint) != 0);
	CHECK((r.flags & sim::command_flags::kRepeated) != 0);
	CHECK(r.move_forward == 127);
	CHECK(r.yaw == 1234);
	CHECK(r.pose == sim::Pose::Crouch);
}

TEST_CASE("m1 contract: the hold command is the idle command of the state's tick, view and target pose") {
	// The server repeats it from the spawn tick until the first real command arrives; the client's catch-up runs
	// the same commands (docs §9.2), so they are pinned here.
	sim::SoldierState st;
	st.tick = 4000;
	st.yaw = 777;
	st.pitch = -12;
	st.pose = sim::Pose::Stand;
	st.target_pose = sim::Pose::Crouch;
	const sim::Command h = sim::hold_command(st);
	CHECK(h.tick == 4000);
	CHECK(h.yaw == 777);
	CHECK(h.pitch == -12);
	CHECK(h.pose == sim::Pose::Crouch);
	CHECK(h.buttons == 0);
	CHECK(h.move_forward == 0);
	CHECK(h.move_right == 0);
	const sim::Command r = sim::repeated_command(h, 4001);
	CHECK(r.tick == 4001);
	CHECK((r.flags & sim::command_flags::kRepeated) != 0);
	// Two repeats of the same hold are equal: the server and the client step identical inputs.
	CHECK(sim::repeated_command(r, 4002) == sim::repeated_command(h, 4002));
}

TEST_CASE("m1 contract: snapshot quantisers stay within half a step in range") {
	const net::Quantizer qs[] = { net::quant::kPositionXZ, net::quant::kPositionY, net::quant::kSoldierVelocity };
	for (const net::Quantizer &q : qs) {
		const float top = q.decode(q.max_code());
		for (int i = 0; i <= 1000; ++i) {
			const float v = q.min + (top - q.min) * float(i) / 1000.0f;
			CHECK(std::fabs(q.decode(q.encode(v)) - v) <= q.step * 0.5f + std::fabs(v) * 1e-6f);
		}
	}
	// Metro's soldier area (y 59.8-73.4 at the spawns) is inside the Y range at 7.8 mm.
	CHECK(net::quant::kPositionY.decode(net::quant::kPositionY.encode(66.6f)) == doctest::Approx(66.6f).epsilon(0.001));
}

TEST_CASE("m1 contract: default configs match the design numbers") {
	const bf3::host::ClientSessionConfig c;
	CHECK(c.interp_min_ms == 75.0f); // plan §2.4's 75-100 ms band (docs §9.4)
	CHECK(c.interp_max_ms == 150.0f);
	CHECK(c.error_decay_ms == 100.0f);
	CHECK(c.target_buffer_ticks == 2);
	CHECK(c.target_hold_s > 0.0f);
	CHECK(c.clock_time_constant_s >= 1.0f);
	CHECK(c.extrapolate_max_ms == 250.0f);
	CHECK(c.max_clock_adjust == doctest::Approx(0.05f));
	// E2's input-edge allowance is the redundancy plus the largest command buffer (docs §1 E2, rest_check.h).
	const bf3::host::RestCheckOptions rest;
	CHECK(rest.edge_ticks == net::kCommandsPerPacket + c.max_buffer_ticks);
	CHECK(rest.window_s == 10.0);
	CHECK(rest.max_eye_jitter_m == doctest::Approx(1e-4f));
	const bf3::host::InputSample input;
	CHECK(input.life == 0);
	const bf3::host::GameServerConfig g;
	CHECK(g.snapshot_divisor == 2);
	CHECK(g.max_slots <= net::kMaxSlots);
	const sim::MotorTuning t;
	CHECK(t.gravity == doctest::Approx(9.81f));
	CHECK(t.reference_hz == 30.0f);
	const net::NetSimConfig off;
	CHECK_FALSE(off.enabled());
	const sim::SoldierState s;
	CHECK(s.pose == sim::Pose::Stand);
	CHECK(s.health == 100.0f);
}

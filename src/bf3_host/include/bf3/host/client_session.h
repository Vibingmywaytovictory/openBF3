#pragma once
// ClientSession: the M1 client (plan §2.3 client row, §3.5; topics/05 §4.1-§4.2, §7.4-§7.5; docs/design/m1.md
// §9). Godot-free. It owns its own std::thread (a deviation from plan §2.3, which runs prediction on the Godot
// main thread; justified in docs §9.1): the ENet client host, the netsim in both directions, the 60 Hz client
// clock, prediction and replay over the client's own CollisionWorld (built over the SharedLevel's immutable
// static shapes), reconciliation and the interpolation buffer of the other soldiers.
//
// The Godot main thread talks to it through two lock-free-ish handoffs only:
//   set_input()            the latest InputSample (written every frame; the client thread samples it per tick)
//   read_presentation()    the newest PresentationFrame (double buffered; the main thread copies it out)
// It calls no Godot API, so the headless tests run it against a ServerHost on the same process.
//
// Per client tick (60 Hz, client clock running command_buffer_ticks ahead of the server, §7.5):
//   1. service ENet; feed every received datagram through the inbound NetSim
//   2. newest snapshot: if its own state differs from the predicted state at that tick beyond the epsilons
//      (or any discrete field differs), reset to it and replay every unacked command (prediction ring)
//   3. sample the input -> Command (quantised at sampling, so prediction and server see the same values)
//   4. predict: SoldierMotor::step on the client's own CollisionWorld
//   5. send a CommandPacket with the last kCommandsPerPacket commands through the outbound NetSim
//   6. publish a PresentationFrame (prev/curr own state for render interpolation, the decaying visual offset,
//      every other soldier interpolated at render time - interp delay)

#include "bf3/core/result.h"
#include "bf3/host/level_load.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::host {

enum class PoseRequest : uint8_t {
	Keep = 0, // hold the current target pose
	Set = 1, // go to InputSample::pose (BF3 toggles are resolved by the input mapper on the Godot side)
};

// What the Godot side samples from the devices. Axes are already mapped (WASD, gamepad); angles are the camera's
// absolute view angles, so mouse look never waits for the network (docs §11.5).
struct InputSample {
	float move_forward = 0.0f; // -1..1
	float move_right = 0.0f; // -1..1
	float yaw = 0.0f; // radians; 0 looks down +Z, a quarter turn down +X (sim convention, command.h)
	float pitch = 0.0f; // radians, + = up
	uint32_t held_buttons = 0; // sim::buttons:: level bits (sprint, fire, zoom)
	uint32_t jump_presses = 0; // monotonically increasing press counter: an edge is never lost between ticks
	PoseRequest pose_request = PoseRequest::Keep;
	sim::Pose pose = sim::Pose::Stand;
	bool free_camera = false; // F6: the soldier gets a neutral command (it stands still, keeps simulating)
};

struct ClientSessionConfig {
	std::string host = "127.0.0.1";
	uint16_t port = 0;
	std::string name = "player";
	net::NetSimConfig netsim; // applied in both directions at this endpoint (net/netsim.h)
	float interp_min_ms = 50.0f; // interpolation delay clamp (docs §9.4)
	float interp_max_ms = 150.0f;
	float error_decay_ms = 100.0f; // visual error decay after a correction (IW4L predicted_error ERROR_DECAY_MS)
	float snap_below_m = 0.0025f; // a visual offset below this is dropped
	float teleport_above_m = 2.0f; // a correction above this snaps (no smoothing)
	float position_epsilon_m = 1e-4f; // reconcile threshold
	float velocity_epsilon = 1e-3f; // m/s
	uint32_t target_buffer_ticks = 2; // commands arriving this many ticks before the server needs them
	float max_clock_adjust = 0.05f; // the client tick rate varies by at most +-5 %
	float clock_reset_ms = 250.0f; // a clock error beyond this resets the client tick (no slewing)
	uint32_t prediction_ring = 128; // commands and predicted states kept for replay (2.1 s)
	uint32_t connect_timeout_ms = 5000;
	bool request_dev_commands = false; // the session may send Debug requests (tests, --m1-check)
};

// One soldier as the presentation sees it.
struct PresentedSoldier {
	uint8_t slot = 0;
	uint32_t life = 0;
	sim::Vec3 position{}; // feet, world space
	sim::Vec3 velocity{};
	float yaw = 0.0f; // radians
	float pitch = 0.0f;
	sim::Pose pose = sim::Pose::Stand;
	float eye_height = 1.6f; // blended over the pose transition
	sim::MoveMode mode = sim::MoveMode::OnGround;
	uint8_t flags = 0;
};

struct ClientStats {
	bool connected = false;
	bool spawned = false;
	uint8_t slot = 0;
	sim::Tick client_tick = 0;
	sim::Tick last_server_tick = 0;
	double rtt_ms = 0.0; // from the snapshot's sequence echo
	double enet_rtt_ms = 0.0;
	int32_t command_buffer_ticks = 0; // the server's report
	float clock_adjust = 0.0f; // current tick-rate scale - 1
	uint64_t clock_resets = 0;
	uint64_t snapshots_received = 0;
	uint64_t snapshots_lost = 0; // sequence gaps
	uint64_t snapshots_late = 0; // older than the newest applied
	uint64_t commands_sent = 0;
	uint64_t reconciliations = 0; // own state reset + replay
	uint64_t replayed_ticks = 0;
	uint64_t corrections_over_1mm = 0; // |server - predicted| position > 1 mm at a reconciliation
	float max_correction_m = 0.0f;
	uint64_t teleports = 0; // corrections above teleport_above_m
	double interp_delay_ms = 0.0;
	uint64_t interp_extrapolated = 0; // render time past the newest snapshot of an other soldier
	net::NetSimStats netsim_out;
	net::NetSimStats netsim_in;
	std::string last_error;
};

// One frame for the Godot main thread. The own soldier comes as the two latest predicted ticks plus the fraction
// so the camera interpolates between them at the display rate; the visual offset is added on top and decays.
struct PresentationFrame {
	uint64_t serial = 0; // increases with every publish
	bool valid = false;
	sim::Tick tick = 0; // the newer own tick
	double tick_fraction = 0.0; // 0..1 of the way from prev to curr at publish time
	uint64_t publish_ns = 0; // net::monotonic_ns
	sim::SoldierState own_prev;
	sim::SoldierState own_curr;
	sim::Vec3 visual_offset{}; // added to the own position; decays to zero over error_decay_ms
	float eye_height_prev = 1.6f;
	float eye_height_curr = 1.6f;
	std::vector<PresentedSoldier> others;
	uint32_t events = 0; // sim::soldier_events:: of the own soldier since the previous frame (sounds, HUD)
	ClientStats stats;
};

class ClientSession {
public:
	ClientSession();
	~ClientSession(); // stops and joins
	ClientSession(const ClientSession &) = delete;
	ClientSession &operator=(const ClientSession &) = delete;

	// Builds the client CollisionWorld and motor over p_level, connects, sends Hello and Ready, and starts the
	// client thread. Fails without starting a thread.
	core::Status start(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level);
	void stop(); // idempotent; sends Bye, joins
	bool running() const;

	void set_input(const InputSample &p_input); // any thread; the newest wins
	// Copies the newest frame into r_frame when it is newer than r_frame.serial. Returns whether it copied.
	bool read_presentation(PresentationFrame &r_frame) const;
	ClientStats stats() const;

	// Dev commands (request_dev_commands). The report arrives asynchronously: poll take_debug_report.
	void send_debug(const net::DebugRequest &p_request);
	bool take_debug_report(net::DebugReport &r_report);

	// Stops and joins every running ClientSession of the process (extension deinit safety net).
	static size_t stop_all();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::host

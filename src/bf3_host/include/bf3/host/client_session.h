#pragma once
// ClientSession: the M1 client (plan §2.3 client row, §3.5; topics/05 §4.1-§4.2, §7.4-§7.5; docs/design/m1.md
// §9). Godot-free. It owns its own std::thread (a deviation from plan §2.3, which runs prediction on the Godot
// main thread; justified in docs §3 and put to the user in docs §16): the ENet client host, the netsim in both
// directions, the 60 Hz client clock, prediction and replay over the client's own CollisionWorld (built over the
// SharedLevel's immutable static shapes) and reconciliation. The others' interpolation buffer is kept here, but
// the interpolation itself runs per display frame on the main thread (present_others), as plan §2.3 has it.
//
// The Godot main thread talks to it through two handoffs only:
//   set_input()            the latest InputSample (written every frame; the client thread samples it per tick)
//   read_presentation()    the newest PresentationFrame, copied out under a short mutex (the frame is trivially
//                          copyable and fixed size, <= ~20 KiB; a seqlock over heap-owning members would be UB)
// It calls no Godot API, so the headless tests run it against a ServerHost on the same process.
// The thread waits inside ENet's service call with a timeout to its next deadline (client tick or netsim
// release), so a datagram is taken when it arrives, not at the next tick (no half-tick RTT bias on this side), and
// it calls Host::flush() right after each CommandPacket send.
//
// Per client tick C (the tick about to be predicted; 60 Hz, the clock running the target command buffer ahead of
// the server, docs §7.5, §9.1):
//   1. service ENet; feed every received datagram through the inbound NetSim; the own_events of every decoded
//      snapshot (not only the newest) are ORed into the frame's server_events
//   2. newest snapshot (seq_newer): S = its own state, T = server_tick = S.tick (protocol.h invariant)
//   3. clock: slew, or reset to the estimate when the error passes clock_reset_ms. A backward reset discards the
//      ring's entries after the new C - 1 (sim::PredictionRing::discard_after). Then, whenever T >= C (the clock
//      is not ahead of the newest authoritative tick: startup, Spawned before the clock settled, a stall), C is
//      forced forward to max(estimate, T + 1), counted in clock_resets
//   4. reconcile (docs §9.2): if S has a newer life than the ring's (a spawn, whichever message carried it), or
//      the ring has no entry at T (ring_misses), or the entry differs from S beyond the epsilons (any discrete
//      field, reconciliations): sim::catch_up(motor, S, ring, C)
//   5. base: if the ring still has no entry at C - 1 (a forward reset, a stall), sim::catch_up(motor, base, ring,
//      C) with base = the ring's newest entry at or below C - 1, or the newest authoritative state when that is
//      newer (by tick, within the same life). So the prediction always has its base, and the motor's precondition
//      (command tick == state tick + 1) holds after any clock jump; every step checks it first and counts a
//      violation in step_precondition_failures (the tests require 0)
//   6. sample the input -> Command for C (quantised at sampling). While InputSample::life is older than the
//      soldier's life the main thread has not yet adopted the spawn view, so the command is the hold command's
//      content at C (no input, the spawn yaw and pitch, the spawn state's target pose; flags 0 as every client
//      command). Predict ring[C] = step(ring[C - 1].state, cmd), sent
//   7. send a CommandPacket of the last kCommandsPerPacket commands from the send history (only commands this
//      client made and sent, never the catch-up's synthesised repeats) through the outbound NetSim; flush
//   8. publish a PresentationFrame: prev/curr own state for render interpolation, the decaying visual offset, the
//      last authoritative own state, and per other soldier the snapshot samples around the render time
// Before the first own state (Spawned or a snapshot) the client predicts nothing and sends no commands: the
// server holds the new slot on hold_command until the first one arrives (game_server.h).

#include "bf3/core/result.h"
#include "bf3/host/level_load.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/soldier_state.h"
// M2a cl (docs/design/m2a.md §8; as built: docs/design/m2a-cl.md)
#include "bf3/host/client_projectiles.h"
#include "bf3/host/kits.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/weapons_wire.h"

#include <cstdint>
#include <functional>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
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
	// The soldier life whose spawn view the mapper has adopted. On a new life (PresentationFrame::own_curr.life
	// newer than this) the mapper sets its mouse yaw and pitch to own_curr's (the spawn view) and then reports
	// that life here; until it does, the client commands the spawn view (step 6 above), so the first real
	// commands never turn the soldier away from the view the server's hold command used (docs §9.2, §11.6).
	uint32_t life = 0;
	// M2a cl (docs/design/m2a.md §6.2, §8.1; as built: docs/design/m2a-cl.md). fire_presses: a press counter like
	// jump_presses, so a click shorter than a tick still fires (the command of the tick after a change carries kFire).
	// The view sample of the frame the player saw: set_view_sample fills it from the drawn PresentationFrame; without
	// it (view_valid false: headless tests) the client takes its own newest frame's render time at sampling.
	uint32_t fire_presses = 0;
	bool view_valid = false;
	uint8_t view_quality = 0; // sim::ViewQuality of the drawn frame
	uint16_t view_seq = 0; // PresentationFrame::newest_seq of the drawn frame
	double view_render_ticks = 0.0; // the render time present_others drew the frame at (server ticks)
};

struct ClientSessionConfig {
	std::string host = "127.0.0.1";
	uint16_t port = 0;
	std::string name = "player";
	net::NetSimConfig netsim; // applied in both directions at this endpoint (net/netsim.h)
	float interp_min_ms = 75.0f; // interpolation delay clamp: plan §2.4's 75-100 ms band at the low end (docs §9.4)
	float interp_max_ms = 150.0f;
	float extrapolate_max_ms = 250.0f; // past the newest sample (plan §3.5)
	float error_decay_ms = 100.0f; // visual error decay after a correction (IW4L predicted_error ERROR_DECAY_MS)
	float snap_below_m = 0.0025f; // a visual offset below this is dropped
	float teleport_above_m = 2.0f; // a correction above this snaps (no smoothing)
	float position_epsilon_m = 1e-4f; // reconcile threshold
	float velocity_epsilon = 1e-3f; // m/s
	// Time sync (docs §7.5). The target command buffer is target_buffer_ticks + max(0, ceil(2 x MAD - 1)), MAD
	// being the mean absolute deviation of the server's integer buffer reports over the last 64 snapshots, clamped
	// to max_buffer_ticks. The -1 keeps the +-1 flip of an integer reading (phase drift inside the deadband) from
	// raising the target: 2 on loopback, 2-3 under "150/2". A new target is adopted only after it has been the
	// computed value for target_hold_s (no flapping). After a clock reset, buffer reports whose sequence_echo is
	// older than the first command sent after the reset still describe the old clock and are ignored (no MAD
	// sample, no slew, no further reset), so one reset never cascades into a reset per snapshot.
	uint32_t target_buffer_ticks = 2;
	uint32_t max_buffer_ticks = 6;
	float target_hold_s = 2.0f;
	float max_clock_adjust = 0.05f; // the client tick rate varies by at most +-5 %
	float clock_time_constant_s = 1.5f; // proportional slew: rate = error / time constant, clamped (loop delay ~ RTT)
	float clock_deadband_ticks = 0.5f; // no slew while |buffer - target| is below this
	float clock_reset_ms = 250.0f; // a clock error beyond this resets the client tick (no slewing)
	uint32_t prediction_ring = 128; // commands and predicted states kept for replay (2.1 s)
	uint32_t connect_timeout_ms = 5000;
	bool request_dev_commands = false; // the session may send Debug requests (tests, --m1-check)
	bool debug_hash_compare = false; // also compare soldier_state_hash at every snapshot (counts hash_mismatches)
	// Tests only (host.session.unit, T4): the first clock estimate (from MatchInfo) is moved by this many ticks, so
	// a test can start the clock behind the server's spawn tick (a start with T >= C: forced_forward).
	int32_t initial_clock_offset_ticks = 0;
	// M2a (docs/design/m2a.md §5.5, E6; cl): the weapon_mechanics.cfg this client parses when it builds its weapon
	// table on MatchInfo (both ends of a headless test share one SharedLevel, so the cfg is not part of it). The
	// default is the shipped file's project-relative path (sim::weapons::kMechanicsTuningPath); E6's mismatch run
	// points it at a second file.
	std::string mechanics_cfg = "config/weapon_mechanics.cfg";
	// M2a cl (as built: docs/design/m2a-cl.md). What the client builds its weapon table from. Null: the level's own
	// (client_gunplay_inputs of SharedLevel::gunplay, built on MatchInfo) when gunplay_from_level is set and the level
	// has gunplay tables; otherwise no gunplay, and a server whose MatchInfo names a weapon_hash refuses this client
	// ("weapon table mismatch").
	std::shared_ptr<const struct ClientGunplayInputs> gunplay;
	bool gunplay_from_level = true;
	// The own world's time rule clamp (ClientProjectiles); -1 = MatchInfo::max_rewind_ticks. E12's control run keeps
	// the client at 21 while its server uses 0.
	int32_t own_test_max_rewind_ticks = -1;
	bool record_predicted = false; // keep the first predictions' ShotEvents and timed events (take_predicted; E1, E5)
	bool send_kit_select = false; // send kit_select with Ready (the kit and team of the first spawn)
	net::KitSelect kit_select;
};

// One soldier as the presentation sees it.
struct PresentedSoldier {
	uint8_t slot = 0;
	uint8_t generation = 0; // life & 0xFF (net::OtherSoldier)
	sim::Vec3 position{}; // feet, world space
	sim::Vec3 velocity{};
	float yaw = 0.0f; // radians
	float pitch = 0.0f;
	sim::Pose pose = sim::Pose::Stand;
	float eye_height = 1.6f; // blended over the pose transition
	sim::MoveMode mode = sim::MoveMode::OnGround;
	uint8_t flags = 0;
	bool extrapolated = false; // render time was past the newest sample
	// M2a (docs/design/m2a.md §6.1; cl fills them in present_others): the stance being entered and the transition
	// progress, switched together with `pose` at the newer sample; the capsules (sim::weapons::TargetPose) and tp's
	// stance blend read them.
	sim::Pose target_pose = sim::Pose::Stand;
	float transition = 0.0f; // 0..1 (net::OtherSoldier::transition / 255)
};

// Trivially copyable (a fixed char buffer instead of a string): it travels inside PresentationFrame.
struct ClientStats {
	bool connected = false;
	bool spawned = false;
	uint8_t slot = 0;
	sim::Tick client_tick = 0;
	sim::Tick last_server_tick = 0;
	double rtt_ms = 0.0; // snapshot sequence echo minus the server's echo hold (smoothed)
	double rtt_min_ms = 0.0; // minimum over the last 2 s (the server services ENet once per tick: up to +1 tick)
	double enet_rtt_ms = 0.0; // ~0 on loopback: the netsim sits above ENet
	int32_t command_buffer_ticks = 0; // the server's report
	uint32_t target_buffer_ticks = 0; // the current target (docs §7.5)
	float clock_adjust = 0.0f; // current tick-rate scale - 1
	uint64_t clock_resets = 0;
	uint64_t motor_hash_mismatch = 0; // MatchInfo::motor_hash differed from ours (M1: logged only)
	uint64_t snapshots_received = 0;
	uint64_t snapshots_lost = 0; // sequence gaps
	uint64_t snapshots_late = 0; // older than the newest applied
	uint64_t commands_sent = 0;
	uint64_t reconciliations = 0; // own state differed from the ring's prediction: reset + replay
	sim::Tick last_reconcile_tick = 0; // snapshot tick T of the newest reconciliation (E2's rest judge, rest_check.h)
	uint64_t last_reconcile_ns = 0; // when it ran (net::monotonic_ns)
	uint64_t ring_misses = 0; // no predicted state for the snapshot's tick (stall, clock reset): catch-up, no compare
	uint64_t spawns = 0; // Spawned states taken (catch-up from the spawn tick)
	uint64_t replayed_ticks = 0; // steps of every catch-up (reconciliations, ring misses, spawns, base fills)
	uint64_t base_fills = 0; // catch-ups because the ring had no entry at C - 1 (forward reset, stall)
	uint64_t forced_forward = 0; // clock forced to T + 1 because T >= C (also counted in clock_resets)
	uint64_t step_precondition_failures = 0; // predicted steps whose command tick != base tick + 1 (must stay 0)
	uint64_t corrections_over_1mm = 0; // |server - predicted| position > 1 mm at a reconciliation
	float max_correction_m = 0.0f;
	uint64_t teleports = 0; // corrections above teleport_above_m
	uint64_t hash_mismatches = 0; // debug_hash_compare: equal within the epsilons but a different hash
	double interp_delay_ms = 0.0;
	net::NetSimStats netsim_out;
	net::NetSimStats netsim_in;
	std::array<char, 128> last_error{}; // NUL-terminated
	// M2a cl (docs/design/m2a.md §6.3, §8; as built: docs/design/m2a-cl.md).
	bool refused = false; // MatchInfo refused (weapon or kit table mismatch): disconnected before Ready, nothing predicted
	bool gunplay = false; // the own weapons are predicted (the server's weapon_hash matched)
	uint64_t weapon_hash_mismatch = 0;
	uint64_t kit_hash_mismatch = 0;
	uint64_t weapon_reconciliations = 0; // predicted weapons at T differed from the Snapshot's wire view: rebase + replay
	uint64_t weapon_ring_misses = 0; // no predicted weapons at T: rebased from the wire view (no compare)
	uint64_t health_rebases = 0; // R2: only the health differed at T: the ring's health moved by the difference, no replay
	uint64_t predicted_shots = 0; // ShotEvents of first predictions
	uint64_t predicted_timed = 0; // timed weapon events of first predictions
	uint64_t no_view_shots = 0; // own shots whose command had no stored view sample (a repeat, a view_seq outside 32)
	uint64_t snapshot_others_received = 0;
	uint64_t remote_shots = 0; // RemoteShot events (shots of other slots)
	uint64_t combat_hits = 0, combat_deaths = 0, combat_detonations = 0;
	uint64_t bad_gunplay_datagrams = 0; // ShotEvents / CombatEvents / SnapshotOthers that failed to decode
	uint64_t bad_weapon_wires = 0; // a Snapshot's own weapons block refused by weapons_from_wire (not adopted)
	uint64_t bad_gunplay_events = 0; // decoded shots / hits / deaths / detonations naming a slot or def out of range (dropped)
	uint64_t late_sample = 0; // arrivals with a seq <= the view_seq of a firing command already sent (§6.3)
	uint64_t generation_reorder = 0; // a reordered old-generation record reset a drawn track
	uint64_t others_ahead = 0; // published frames drawing a SnapshotOthers record newer than newest_seq's tick
	uint64_t track_timeout = 0; // drawn tracks dropped by the wall-clock timeout
	uint64_t events_overflow = 0; // PresentationEvents pushes refused (ring full; E14 gates 0)
	sim::WeaponLaneStats lane;
	ClientProjectilesStats projectiles;
};
static_assert(std::is_trivially_copyable_v<ClientStats>);

// The snapshot samples of one other soldier around the render time (docs §9.4): the newest sample at or before
// the render time at publish, then up to two newer ones, so the main thread finds a bracketing pair for any frame
// until the next publish.
inline constexpr size_t kOtherSamples = 3;
using OtherSample = net::TimedOther; // R1 (docs/design/m2a.md §6.1; cl): the record type lc moved to bf3_net
struct OtherTrack {
	uint8_t slot = 0;
	uint8_t count = 0; // samples used, oldest first
	std::array<OtherSample, kOtherSamples> samples{};
	sim::Tick track_newest_tick = 0; // M2a cl: the newest sample of the client's whole track (window_extrapolated)
};

// One frame for the Godot main thread. The own soldier comes as the two latest predicted ticks plus the fraction
// so the camera interpolates between them at the display rate; the visual offset is added on top and decays.
// Trivially copyable and fixed size; read_presentation copies the used part of `others` only.
struct PresentationFrame {
	uint64_t serial = 0; // increases with every publish
	bool valid = false;
	sim::Tick tick = 0; // the newer own tick
	double tick_fraction = 0.0; // 0..1 of the way from prev to curr at publish time
	uint64_t publish_ns = 0; // net::monotonic_ns
	// The client clock's rate (ticks per ns, including the slew): the main thread advances tick_fraction by
	// (now - publish_ns) * own_ticks_per_ns, clamped to 1 (present_own_fraction), so the own camera moves at the
	// display rate between 60 Hz publishes.
	double own_ticks_per_ns = double(sim::kSimHz) * 1e-9;
	sim::SoldierState own_prev;
	sim::SoldierState own_curr;
	sim::Command own_command; // the command own_curr was predicted with (the HUD; E2's rest judge reads idleness)
	sim::Vec3 visual_offset{}; // added to the own position; decays to zero over error_decay_ms
	float eye_height_prev = 1.6f;
	float eye_height_curr = 1.6f;
	// The last authoritative own state from a snapshot, for the debug HUD only. E1 through the full path is
	// measured on the per-tick Trace reports (net::DebugKind::Trace, docs §7.7), not on this 30 Hz state.
	bool server_own_valid = false;
	sim::SoldierState server_own;
	// Snapshot::own_events of every snapshot decoded since the previous frame, ORed (a snapshot superseded in the
	// same client tick still contributes; a lost one does not: HUD only, docs §9.1).
	uint16_t server_events = 0;
	uint32_t events = 0; // predicted sim::soldier_events:: of the own soldier since the previous frame
	// The others' clock: render time in server ticks at publish_ns and its rate in ticks per nanosecond, so the
	// render time at frame time t is render_ticks_at_publish + (t - publish_ns) * ticks_per_ns. It is built on the
	// received server time (newest snapshot's server_tick + the time since it arrived, smoothed by a slow filter
	// on the arrival offsets), not on the client's ahead-running command clock: render = received - delay
	// (docs §9.4), so with d >= 2 snapshot intervals a sample newer than the render time normally exists.
	double render_ticks_at_publish = 0.0;
	double ticks_per_ns = double(sim::kSimHz) * 1e-9;
	float extrapolate_max_ms = 250.0f;
	// Eye height per pose (MotorParams::poses[].eye_height), so present_others blends a remote soldier's eye height
	// over its transition progress without the motor (added in T4).
	std::array<float, sim::kPoseCount> pose_eye_heights{ 1.6f, 1.0f, 0.3f };
	uint32_t other_count = 0;
	std::array<OtherTrack, net::kMaxSlots> others{};
	ClientStats stats;
	// M2a (docs/design/m2a.md §6.2, §8.3; cl fills it in publish): the newest Snapshot seq received when this frame was
	// published (M1's newest_snapshot at that moment; a newer SnapshotOthers does not count, §7.2). The main thread
	// puts its low 8 bits into the InputSample with the frame's render time, and the client thread writes them into
	// the command as Command::view_seq8.
	uint16_t newest_seq = 0;
	// M2a cl (as built: docs/design/m2a-cl.md): the own weapons of the two latest predicted ticks for
	// present_weapon (presentation_weapon.h), the match seed (effect seeds) and the server tick of newest_seq.
	bool own_weapons_valid = false;
	net::WeaponsWire own_weapons_prev; // weapons_wire_view(predicted weapons after tick - 1, tick - 1)
	net::WeaponsWire own_weapons_curr; // weapons_wire_view(predicted weapons after tick, tick)
	uint64_t match_seed = 0;
	sim::Tick newest_tick = 0;
	uint8_t own_kit = 0;
	uint8_t own_team = 0;
};
static_assert(std::is_trivially_copyable_v<PresentationFrame>);

// Main thread, every display frame: the others at render time p_now_ns (Hermite on the samples' positions and
// velocities, shortest-arc yaw/pitch, pose and mode from the newer sample; past the newest sample extrapolated
// for at most extrapolate_max_ms, then held). Writes at most r_out.size() soldiers and returns the count; adds
// the extrapolated ones to *r_extrapolated when given. A pure function of the frame (tests call it directly).
size_t present_others(const PresentationFrame &p_frame, uint64_t p_now_ns, std::span<PresentedSoldier> r_out, uint64_t *r_extrapolated = nullptr);
// Main thread, every display frame: the own soldier's interpolation fraction between own_prev and own_curr at
// p_now_ns: tick_fraction + (p_now_ns - publish_ns) * own_ticks_per_ns, clamped to [0, 1].
double present_own_fraction(const PresentationFrame &p_frame, uint64_t p_now_ns);

class ClientSession {
public:
	ClientSession();
	~ClientSession(); // stops and joins
	ClientSession(const ClientSession &) = delete;
	ClientSession &operator=(const ClientSession &) = delete;

	// Builds the client CollisionWorld and motor over p_level, connects, sends Hello and Ready, and starts the
	// client thread. Fails without starting a thread. M1 sends Ready as soon as the SharedLevel exists, so the
	// soldier spawns (and its 2 s protection runs) while Phase C still uploads; harmless with fall damage as the
	// only damage. M3 moves Ready to LoadStage::Done (plan §2.7: Loading progress before Ready), docs §7.8.
	core::Status start(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level);
	void stop(); // idempotent; sends Bye, joins
	bool running() const;

	void set_input(const InputSample &p_input); // any thread; the newest wins
	// Copies the newest frame into r_frame when it is newer than r_frame.serial, under a short mutex (the client
	// thread builds the next frame outside it and copies it in). Returns whether it copied.
	bool read_presentation(PresentationFrame &r_frame) const;
	ClientStats stats() const;

	// Dev commands (request_dev_commands). The report arrives asynchronously: poll take_debug_report.
	void send_debug(const net::DebugRequest &p_request);
	bool take_debug_report(net::DebugReport &r_report);

	// Test hooks (host.session.unit, T4; docs §14): disturb the running client the way a scheduler hiccup or a
	// mis-set clock would, so the tests exercise the recovery rules through the real thread. debug_shift_clock moves
	// the command clock (and the tick about to be predicted) by p_ticks without counting a reset: the clock rules
	// must then notice the wrong buffer and reset exactly once. debug_stall makes the client thread sleep p_ms once
	// (no ENet service, no ticks), as a stalled thread would. Any thread; applied at the next loop of the client.
	void debug_shift_clock(int32_t p_ticks);
	void debug_stall(uint32_t p_ms);

	// M2a cl (docs/design/m2a.md §8.3; as built: docs/design/m2a-cl.md). The events ring (drain it from one thread,
	// the main thread), the first predictions' shots and timed events (record_predicted) and the own world's E12 log,
	// each moved out since the previous call. Any thread.
	PresentationEvents &events();
	void take_predicted(std::vector<struct PredictedShot> &r_shots, std::vector<struct PredictedTimed> &r_timed);
	void take_own_test_samples(std::vector<OwnTestSample> &r_samples);

	// Stops and joins every running ClientSession of the process (extension deinit safety net).
	static size_t stop_all();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// ---- M2a cl (docs/design/m2a.md §6.1-§6.3, §8; as built: docs/design/m2a-cl.md) -----------------------------------

// What the client builds its weapon table from on MatchInfo: by default the SharedLevel's gunplay tables
// (client_gunplay_inputs below, the same defs, materials, capsule pose table and kits the server's Gunplay uses), or
// what the caller supplies (ClientSessionConfig::gunplay). The client's table is a copy of `table` with the mechanics
// from mechanics_cfg (load_mechanics; null keeps the table's) and the match options of MatchInfo::weapon_flags,
// rehashed; it predicts only when weapon_table_hash(that) ^ capsules->hash equals MatchInfo::weapon_hash and kits.hash
// equals MatchInfo::kit_hash. A table's defs do not depend on the mechanics (make_weapon_table), so the copy equals the
// server's make_match_table of the same cfg and flags bit for bit.
struct ClientGunplayInputs {
	std::shared_ptr<const sim::weapons::WeaponTable> table;
	std::shared_ptr<const core::gameplay::CapsulePoseTable> capsules;
	KitTable kits;
	net::ShotQuantization shot_frame; // the level's frame for shot origins and combat positions (static world bounds min)
	// Reads and parses the cfg at a path (sv: read the file, sim::weapons::parse_mechanics_tuning). An error refuses
	// the match like a mismatch.
	std::function<core::Result<sim::weapons::Mechanics>(const std::string &p_path)> load_mechanics;
};

// The weapon table a client builds from p_inputs for MatchInfo's p_weapon_flags (also what a test server builds).
core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> build_client_weapon_table(const ClientGunplayInputs &p_inputs,
		const std::string &p_mechanics_cfg, uint8_t p_weapon_flags);
// MatchInfo::weapon_hash of a table and a capsule table: weapon_table_hash(table) ^ capsules.hash.
uint64_t client_weapon_hash(const sim::weapons::WeaponTable &p_table, const core::gameplay::CapsulePoseTable &p_capsules);
// The inputs from a level's gunplay tables (SharedLevel::gunplay, gunplay.h): make_match_table with wp's default
// mechanics (replaced per match by load_mechanics = load_mechanics_cfg), the capsule pose table, the kits and the
// level's shot frame (shot_frame of the static world). Fails without gunplay tables.
core::Result<std::shared_ptr<const ClientGunplayInputs>> client_gunplay_inputs(const SharedLevel &p_level);

struct PredictedShot {
	sim::Tick tick = 0;
	sim::weapons::ShotEvent shot;
};
struct PredictedTimed {
	sim::Tick tick = 0;
	sim::weapons::TimedWeaponEvent event;
};

// present_others with the drawn-frame residual of §6.1: a soldier extrapolated past the window's newest sample while
// the client's track held a newer one (a main-thread stall longer than a sample interval) counts window_extrapolated.
struct PresentOthersCounts {
	uint64_t extrapolated = 0;
	uint64_t window_extrapolated = 0;
	uint64_t held = 0; // before the oldest sample or past the extrapolation limit
};
size_t present_others(const PresentationFrame &p_frame, uint64_t p_now_ns, std::span<PresentedSoldier> r_out, PresentOthersCounts &r_counts);
// The render time present_others draws at p_now_ns (render_ticks_at_publish advanced by ticks_per_ns).
double present_render_ticks(const PresentationFrame &p_frame, uint64_t p_now_ns);
// The view quality of the frame drawn at p_render_ticks: None for a frame never published, Held when a drawn soldier
// is past the 250 ms extrapolation, Extrapolated when one is past its newest sample, else Interpolated.
sim::ViewQuality present_view_quality(const PresentationFrame &p_frame, double p_render_ticks);
// Main thread, every frame the input is sampled for: puts the drawn frame's view sample into r_input.
void set_view_sample(InputSample &r_input, const PresentationFrame &p_frame, uint64_t p_now_ns);

} // namespace bf3::host

#pragma once
// The level session's wave-3 state (G7, W3.0 contract, docs/design/m2a-w3.md; owner gx). Bf3LevelSession holds one
// M2aSession through m2a_ and calls the m2a_* hooks (level_session.h's M2a w3 block) from one tagged line each in
// level_session.cpp. gx fills the struct (Bf3Viewmodel, Bf3Hud, Bf3GameAudio, Bf3GameFx, Bf3GameCrowd, M2aCheck, the
// parsed args) and the hook bodies (level_session_m2a.cpp).
//
// Hook order in one session (loader thread: no Godot API):
//   main    _ready -> parse_args -> m2a_parse_args
//   loader  run_loader: ... m2a_load_options(shared options) -> load_shared_level -> S1 bot level ->
//           m2a_load_assets(db, cache, threads) -> start_session: ... m2a_server_config(game) -> ServerHost::start ->
//           client_config(...) -> m2a_client_config(cc) -> ClientSession::start
//   main    _process: read_presentation -> mapper -> set_input -> m2a_presented (every frame from session start:
//           drains the PresentationEvents ring so it never overflows during the load, E14) ... after Done:
//           cameras -> draw_others -> update_hud -> S1 -> m2a_frame (true: the M2a check handled the frame, M1's
//           checks and shots are skipped)
//           _unhandled_input: mapper -> m2a_input
//   main    shutdown: loader joined -> m2a_shutdown -> client and server stop
// restart_client calls client_config, so m2a_client_config runs again for the new client.
//
// M2a gx (docs/design/m2a-gx.md). The session is M2a when the scene's session node carries the metadata
// "openbf3_m2a" (res://scenes/m2a_metro.tscn) or the command line has --m2a-check / --m2a-firefight; otherwise every
// hook returns at once and M1, S1 and S3 run exactly as before. M2aGame is the part the check (m2a_check.h) reads:
// a namespace-scope struct, because M2aSession is a private member type of Bf3LevelSession.
//
// User args of an M2a session (after "--"):
//   --kit=<0..3> / --team=<1|2>   the first spawn's kit (assault, engineer, support, recon) and team (default 0, 1);
//                                  K / T in game pick the next spawn's (sent as a KitSelect once spawned)
//   --respawn=<s>                  respawn time after death (GameServerConfig::respawn_ticks; default 5 s in the
//                                  playable scene, 1 s for the checks)
//   --bots=<n>                     S1's walker bots (armed, not firing; default 24 in the playable scene, 0 for the
//                                  checks; at most 32: sv's kMaxLagSlots rule, m2a-w3.md §4)
//   --m2a-check=<cases>            m2a_check.h; --m2a-shots=<dir> where the check saves its screenshots
//   --m2a-play-seconds=<s>         the play case's length (default 300)
//   --m2a-firefight                the firefight case (waits for bt)

#include "level_session.h"
#include "m2a_check.h"
#include "m2a_game.h"

#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/firefight_brain.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/sim/collision_world.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <godot_cpp/variant/vector3.hpp>

namespace godot {
class CanvasLayer;
class Label;
class StandardMaterial3D;
} // namespace godot

namespace bf3::godot_side {

class Bf3Hud;
class Bf3Localisation;
class Bf3UiAssets;
class Bf3Viewmodel;
class Bf3GameAudio;
class Bf3GameFx;
class Bf3GameCrowd;

// The server's gunplay output for the checks (GunplayObserver: called on the server thread; taken on the main
// thread under a mutex). Records only the slot set with watch_slot (the player) plus every damage event, each list
// bounded at kMaxRecords (the excess is counted, never stored).
class M2aServerRecorder : public bf3::host::GunplayObserver {
public:
	static constexpr size_t kMaxRecords = 1u << 16;
	struct Shot {
		uint32_t tick = 0;
		bf3::sim::weapons::ShotEvent shot;
	};
	struct Timed {
		uint32_t tick = 0;
		bf3::sim::weapons::TimedWeaponEvent event;
	};
	struct Damage {
		uint32_t tick = 0;
		bf3::sim::weapons::DamageEvent event;
		bool applied = false;
		float health_after = 0;
	};
	void watch_slot(int p_slot) { slot_.store(p_slot, std::memory_order_release); }
	void weapon_tick(uint8_t p_slot, const bf3::sim::Command &p_command, const bf3::sim::weapons::SoldierWeaponsState &p_after,
			const bf3::sim::weapons::WeaponTickOutput &p_out) override;
	void damage(uint32_t p_tick, const bf3::sim::weapons::DamageEvent &p_event, bool p_applied, float p_health_after) override;
	// The watched slot's detonations at the server's exact times (E11's M67 fuse: the wire's DetonationWire time is
	// quantized to 1/65536 tick, protocol.h).
	struct Detonation {
		uint32_t tick = 0;
		bf3::sim::weapons::DetonationEvent event;
	};
	void projectiles(uint32_t p_tick, const bf3::sim::weapons::ProjectileStepOutput &p_out, std::span<const bf3::sim::weapons::Projectile> p_live) override;
	// Moves everything recorded since the last call out (appends).
	void take(std::vector<Shot> &r_shots, std::vector<Timed> &r_timed, std::vector<Damage> &r_damage);
	void take_detonations(std::vector<Detonation> &r_detonations);
	uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
	std::atomic<int> slot_{ -1 };
	std::atomic<uint64_t> dropped_{ 0 };
	std::mutex mutex_;
	std::vector<Shot> shots_;
	std::vector<Timed> timed_;
	std::vector<Damage> damage_;
	std::vector<Detonation> detonations_;
};

struct M2bGame; // M2b W2.0 (gx): level_session_m2b.h

// What the M2a session keeps between frames, and what the check reads (main thread unless marked).
struct M2aGame {
	// ---- args (m2a_parse_args) ----
	bool enabled = false;
	bool playable = false; // the scene's metadata (not a check run)
	uint8_t kit = 0; // the first spawn's kit (0..3)
	uint8_t team = 1; // and team (1 US, 2 RU)
	double respawn_s = 5.0;
	std::vector<M2aCase> cases;
	M2aCheckOptions check_options;
	bool checks() const { return !cases.empty(); }
	bool wants_case(M2aCase p_case) const;

	// ---- loader results (written by the loader before session_ready_; read on the main thread after) ----
	std::shared_ptr<const bf3::host::ClientGunplayInputs> gunplay;
	bf3::host::HudStatic hud_static;
	std::string hud_static_error;
	// The viewmodel loader thread's results (written before M2aSession::vm_done, read on the main thread after).
	std::array<std::unique_ptr<bf3::render::ViewmodelSet>, 2> vm_sets; // US, RU
	std::vector<bf3::render::ViewmodelKit> vm_kits;
	std::string vm_error; // "" when both sets loaded
	double vm_seconds = 0;
	bf3::anim::ViewmodelTuning vm_tuning;
	std::string vm_tuning_source;
	// def id -> the team set's item index, from the kit table's unlocks (PresentedWeapon::def -> viewmodel item).
	std::array<std::unordered_map<uint16_t, int>, 2> def_items;
	std::unique_ptr<bf3::sim::CollisionWorld> world; // the checks' range lane (null otherwise)
	std::shared_ptr<M2aServerRecorder> recorder; // the checks' server observer (null otherwise)

	// ---- Godot side (built once, on the first frame after Done) ----
	bool built = false;
	Bf3Localisation *localisation = nullptr;
	Bf3UiAssets *ui_assets = nullptr;
	Bf3Hud *hud = nullptr;
	bool hud_names_applied = false;
	std::array<Bf3Viewmodel *, 2> vm{ nullptr, nullptr };
	godot::CanvasLayer *layer = nullptr;
	godot::Label *score_label = nullptr;
	godot::Label *death_label = nullptr;
	godot::Label *kit_label = nullptr;
	std::array<godot::Ref<godot::StandardMaterial3D>, 3> tints; // unknown, friend, enemy
	int shown_team = 0;
	float base_sensitivity = 0;

	// ---- per frame ----
	std::vector<bf3::host::PresentEvent> events; // drained this frame (m2a_presented)
	bf3::host::PresentedWeapon weapon; // present_weapon at this frame's time
	bool alive = false; // spawned and alive (own_curr)
	bool spawned = false; // ClientStats::spawned
	uint8_t own_slot = 0xFF;
	uint8_t own_team = 0;
	uint32_t own_life = 0;
	bf3::host::HudFeedState hud_state;
	bf3::core::ui::HudModel hud_model{};
	std::vector<bf3::anim::VmCue> cues;
	uint64_t vm_shot_cues = 0; // Shot cues fed to the viewmodel
	int vm_item = -1; // the item in hand (of the own team's set)
	uint64_t vm_unmapped = 0; // frames with a weapon whose def has no item
	double prev_yaw = 0, prev_pitch = 0;
	bool prev_view_valid = false;

	// ---- score (Death events: kOwnShooter / kOwnVictim, the killer's team from W3.0's team field) ----
	uint32_t kills = 0, deaths = 0;
	std::array<uint32_t, 3> team_kills{}; // [team]: kills by that team (0: unknown)
	uint64_t deaths_seen = 0, hits_seen = 0, own_shots_seen = 0, remote_shots_seen = 0;
	std::array<uint8_t, 256> slot_team{}; // the last team seen per slot (0 unknown)
	// The death screen.
	bool dead = false;
	uint8_t killed_by = 0xFF; // killer slot (0xFF: unknown, fall damage)
	bool killed_by_self = false;
	uint64_t death_ns = 0;
	uint32_t respawns = 0;
	uint32_t deaths_by_enemy = 0; // own deaths with another soldier as the killer

	// ---- kit / team for the next spawn (K / T) ----
	uint8_t next_kit = 0, next_team = 1;
	bool kit_pending = false; // a KitSelect to send once spawned
	uint64_t kit_selects_sent = 0;

	// ---- the checks ----
	bool record_predicted = false;
	std::vector<bf3::host::PredictedShot> predicted_shots; // first predictions since the check last cleared them
	std::vector<bf3::host::PredictedTimed> predicted_timed;
	std::vector<M2aServerRecorder::Shot> server_shots;
	std::vector<M2aServerRecorder::Timed> server_timed;
	std::vector<M2aServerRecorder::Damage> server_damage;
	std::vector<M2aServerRecorder::Detonation> server_detonations; // the own slot's (bounded like server_damage)
	uint64_t events_drained = 0;
	uint64_t frames = 0;

	// ---- the wave-3 parts (built only once their track has landed: OPENBF3_M2A_HAVE_*) ----
	std::unique_ptr<Bf3GameFx, void (*)(Bf3GameFx *)> fx{ nullptr, nullptr };
	std::unique_ptr<Bf3GameAudio, void (*)(Bf3GameAudio *)> audio{ nullptr, nullptr };
	std::unique_ptr<Bf3GameCrowd, void (*)(Bf3GameCrowd *)> crowd{ nullptr, nullptr };
	bool crowd_built = false;
	std::vector<std::string> part_notes; // the parts' load lines (loader thread), printed on the first frame
	// The firefight bots (bt: GameServerConfig::firefight; Free in the playable scene and the play case, Pairs for
	// --m2a-firefight). null: none.
	std::shared_ptr<bf3::host::FirefightStats> firefight_stats;
	uint32_t firefight_bots = 0;
	bool firefight_pairs = false;
	// E9's FX-off windows (the firefight case): the effects hidden and their step skipped.
	bool fx_off = false;
	// The listener / culling camera of the last frame (M2aFrame::camera_velocity from the difference).
	bool camera_valid = false;
	godot::Vector3 camera_prev;
	// The M2a main-thread parts of this frame, wall milliseconds (E14: viewmodel + HUD + crowd + FX + audio).
	double ms_vm = 0, ms_hud = 0, ms_crowd = 0, ms_fx = 0, ms_audio = 0, ms_total = 0;
	float fp_fov_applied = -1.0f; // the FX 1p projection last set from the viewmodel
	uint64_t own_muzzles_fx = 0, own_muzzles_standin = 0; // own 1p muzzles from fx3's own_muzzle / G6's transform
	// E7 (the audio case): the listener held at a point of the check's (the own soldier then sounds in 3p, as the free
	// camera's: M2aFrame::free_camera), without the free camera's neutral command, so the soldier keeps firing.
	bool listener_override = false;
	godot::Vector3 listener_position, listener_forward{ 0, 0, -1 };

	Bf3Viewmodel *own_viewmodel() const { return own_team == 2 ? vm[1] : (own_team == 1 ? vm[0] : nullptr); }
	// The viewmodel item of a def for a team (-1: none).
	int item_of(uint8_t p_team, uint16_t p_def) const;

	// M2b W2.0 (docs/design/m2b.md §2.6 #18; gx): the M2b session's state (level_session_m2b.h); null outside an M2b
	// session (the scene's "openbf3_m2b" metadata, --m2b-check). gx makes it with its own deleter in its [args] line.
	std::unique_ptr<M2bGame, void (*)(M2bGame *)> m2b{ nullptr, nullptr }; // M2b W2.0 (gx)
};

struct Bf3LevelSession::M2aSession : M2aGame {
	// The viewmodel sets load on their own thread from m2a_load_options on (open_viewmodel_sources is the slow part,
	// 23-80 s on Metro without a warm cache), beside the shapes, the gunplay tables and the bot level; m2a_load_assets
	// joins it, so the session starts (and Done, E16's first controllable frame) with the kit viewmodels loaded
	// (m2a.md E16). The main thread builds the two Bf3Viewmodels on the first frame after Done (vm_built).
	std::thread vm_loader;
	std::atomic<bool> vm_done{ false };
	bool vm_built = false; // the build was attempted (main thread)
	bf3::render::LoadProgress vm_progress;
	// M2a w3 integration (E16): Bf3GameCrowd::load_body (soldier assets, stance and move clips) on its own thread
	// beside load_shared_level, from m2a_load_options; m2a_load_assets joins it before the crowd's load(). The status
	// is written before the join.
	std::thread crowd_loader;
	std::string crowd_body_error;
	double crowd_body_seconds = 0;
	std::unique_ptr<M2aCheck> check;
	bool finished = false;
};

} // namespace bf3::godot_side

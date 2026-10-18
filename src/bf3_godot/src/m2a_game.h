#pragma once
// What the level session hands the wave-3 game parts (W3.0 contract, docs/design/m2a-w3.md; owner gx). Godot-free, so
// the m2a.contract3 test compiles it and the tracks' headless tests can build the same inputs.
//
// Loader thread (Bf3LevelSession::run_loader, gx's m2a_load_assets hook, after the SharedLevel and the bot level):
//   Bf3GameFx::load    first: effect library, impact table, fire effect table, atlas pixels; it names the impact
//                      sounds (fx::impact_sound_names) for
//   Bf3GameAudio::load the wave bank, the kit items' programs (load_weapon_audio) and the impact sounds
//                      (ImpactSoundCache over Bf3GameFx's names);
//   Bf3GameCrowd::load the two teams' 3p soldier assets and every kit item's 3p weapon (CrowdWeapon), crowd clips.
// Main thread, once the session is done (gx's m2a_frame hook): build() each (Godot nodes and resources), then per
// display frame, in this order:
//   1. drain the PresentationEvents ring once (the parts share the span: M2aFrame::events);
//   2. present_others + present_other_weapons at the frame's render time (M2aFrame::others / other_weapons);
//   3. Bf3GameCrowd::step -> its muzzles(); the own 1p muzzle (viewmodel_muzzle_transform, G6) first, then the
//      crowd's, into M2aFrame::muzzles;
//   4. Bf3GameFx::step (triggers, world step, draw, decals) -> its sound_cues();
//   5. Bf3GameAudio::step (listener, router, impacts with Bf3GameFx's cues of this frame).
// Every part must accept a frame with no events, no others and no muzzles, and must do nothing before build().
#include "bf3/fx/fx_types.h"
#include "bf3/fx/triggers.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/level_load.h"
#include "bf3/host/presentation_events.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace bf3::core {
class Cache;
} // namespace bf3::core

namespace bf3::godot_side {

// The loader inputs of the three load() steps. The database and the cache are valid during load() only (run_loader
// drops them after Phase B); a part keeps what it needs (shared pointers, compiled data).
struct M2aLoadInputs {
	const host::LevelDatabase *db = nullptr; // install, level mount, world
	std::shared_ptr<const host::SharedLevel> shared; // loaded with SharedLoadOptions::gunplay (GunplayLevel defs, kits)
	core::Cache *cache = nullptr; // may be null (--no-cache)
	unsigned threads = 0; // cores the scene lanes leave free
	const std::atomic<bool> *stop = nullptr; // the session is shutting down: return early (an error is fine)
	host::LoadProgress *progress = nullptr;
	uint8_t own_team = 1; // --team: the viewmodel set loaded first (both are loaded)
};

// One display frame as the parts see it (main thread; every span valid for the step calls of this frame).
struct M2aFrame {
	const host::PresentationFrame *frame = nullptr;
	host::PresentClock clock; // host::present_clock(frame, now_ns)
	uint64_t now_ns = 0; // net::monotonic_ns
	double dt = 0.0; // the display frame's delta, seconds
	std::span<const host::PresentEvent> events;
	std::span<const host::PresentedSoldier> others;
	std::span<const host::PresentedOtherWeapon> other_weapons; // index for index with others
	std::span<const fx::MuzzleSample> muzzles; // the own 1p muzzle (if any) first, then the crowd's
	// The active camera (1p or free) in world space, Frostbite axes (+Y up, forward = view direction): FX culling and
	// sorting, the audio listener.
	fx::Transform camera;
	fx::Vec3 camera_velocity;
	bool free_camera = false; // F6: the viewmodel and the 1p effects are hidden, the listener moves with the camera
	bool own_alive = false; // the own soldier is spawned and alive (own death hides the 1p parts)
	uint8_t own_slot = 0xFF;
	uint8_t own_team = 0; // 0 unknown, 1, 2
};

} // namespace bf3::godot_side

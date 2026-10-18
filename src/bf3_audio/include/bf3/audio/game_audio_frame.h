#pragma once
// One display frame of the game's audio without Godot (owner au3; docs/design/m2a-au3.md "Frame order"): what
// Bf3GameAudio::step does after it measured the output latency, so the offline E7 rehearsal and the 64-shooter run
// (audio_router_tests.cpp) drive exactly the game's order:
//   set_own_slot / set_own_first_person / set_own_trigger (own_weapons_curr's active slot kTriggerDown)
//   -> set_listener (router and impacts) -> set_others_newest (present_other_weapons at newest_tick, the horizon the
//   end of that tick) -> on_events -> on_others -> impacts -> effect sounds -> update.
// The caller set the clock's latency and bias, and set_now(p_in.clock), before.
// Also the Godot-free halves of Bf3GameAudio::load (load_game_audio_assets) and ::audition (start_audition), so the
// offline runs load and audition exactly as the game does.
#include "bf3/audio/game_audio_check.h"
#include "bf3/audio/impact_audio.h"
#include "bf3/audio/kit_audio.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/install.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/presentation_events.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::level {
class LoadedLevel;
}
namespace bf3::host {
struct GunplayLevel;
}

namespace bf3::audio {

struct GameAudioInputs {
	const host::PresentationFrame *frame = nullptr; // null: no own trigger and no newest records this frame
	uint64_t now_ns = 0;
	std::span<const host::PresentEvent> events;
	std::span<const host::PresentedSoldier> others;
	std::span<const host::PresentedOtherWeapon> other_weapons; // index for index with others
	std::span<const MuzzlePoint> muzzles; // index for index with others (shorter: the rest use the stand-in)
	ListenerState listener;
	uint8_t own_slot = 0xFF;
	bool own_first_person = true; // alive and the 1p camera
};

class GameAudioDriver {
public:
	void step(WeaponAudioRouter &r_router, ImpactAudio *r_impacts, SimMixerClock &r_clock, const GameAudioInputs &p_in,
			std::span<const ImpactSoundRequest> p_impacts = {}, std::span<const EffectSoundRequest> p_effects = {});

private:
	std::array<host::PresentedOtherWeapon, net::kMaxSlots> newest_{};
};

// E7 item 4: the superbundles an audition patch the level's mount lacks is compiled from (the M1A2 is not on Metro's
// mount; its SoundPatchAsset and its waves are in MP_001's rush bundle [verified: bf3dump find @all-mp m1a2_abrams]).
// A comma list or "@all-mp" (expand_superbundles); mounted lazily on the loader thread, only when an item needs it.
inline constexpr const char *kAuditionSuperbundles = "Win32/Levels/MP_001/MP_001";

struct GameAudioLoadOptions {
	// Audition patches the level's mount lacks: compiled from these superbundles, with a bank of their own over the
	// mount of the superbundle that holds the patch. Empty: not loaded (a failing E7 row: the check reports the step
	// as skipped and fails it).
	std::string audition_superbundles = kAuditionSuperbundles;
};

// One audition patch: its program and the bank its waves are resident in (the level bank, or its own).
struct GameAuditionAsset {
	std::shared_ptr<const core::audio::SoundProgram> program;
	std::shared_ptr<const core::audio::WaveBank> bank;
	std::string source; // "level" or the superbundle it was compiled from
};

// Everything the game's audio reads at load: the kit items' table (strict, E7 item 1), the impact / effect sound
// cache, the E7 audition patches (from the level's mount, else from GameAudioLoadOptions::audition_superbundles;
// one that loads from neither is reported and becomes a failing check row) and one bank over the level's waves.
// Loader thread; read-only afterwards.
struct GameAudioAssets {
	KitAudioSources kits;
	std::shared_ptr<const WeaponAudioTable> table;
	std::shared_ptr<const ImpactSoundCache> impacts;
	std::map<std::string, GameAuditionAsset, std::less<>> auditions; // by patch
	std::shared_ptr<const core::audio::WaveBank> bank;
	std::vector<std::string> report; // kit items, impact errors, auditions, the bank line
	std::vector<std::string> audition_failures; // patches that loaded from no source (their check steps fail)
	uint32_t programs = 0, program_failures = 0;
	size_t waves_missing = 0; // waves the level programs name that the level's mount does not hold
	size_t audition_waves_missing = 0; // the same for the auditions compiled from audition_superbundles
};
core::Result<GameAudioAssets> load_game_audio_assets(const std::shared_ptr<const core::Install> &p_install, const core::level::LoadedLevel &p_level,
		const host::GunplayLevel &p_gunplay, std::span<const std::string> p_impact_sounds, unsigned p_threads,
		const std::atomic<bool> *p_stop = nullptr, const GameAudioLoadOptions &p_options = {});
// compile_patch strict, else with allow_unimplemented (auditions: an unimplemented node is silent, not a refusal).
core::Result<std::shared_ptr<const core::audio::SoundProgram>> compile_patch_lenient(const core::EbxResolver &p_db, std::string_view p_patch);

// E7 item 4: one client-local audition one-shot of p_program at p_position facing the listener (third person), at
// p_frame (the clock's earliest frame); the check sees it through before_start when it is in a step. false: refused.
bool start_audition(OneShotPool &r_pool, MixerCore &r_mixer, std::shared_ptr<const core::audio::SoundProgram> p_program,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, const ListenerState &p_listener, const Vec3 &p_position,
		uint64_t p_seed, uint64_t p_frame, GameAudioCheck *p_check);

} // namespace bf3::audio

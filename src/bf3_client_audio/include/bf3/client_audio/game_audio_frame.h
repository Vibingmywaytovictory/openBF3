#pragma once

#include "bf3/audio/game_audio_check.h"
#include "bf3/audio/impact_audio.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/client_audio/kit_audio.h"
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
	const host::PresentationFrame *frame = nullptr;
	uint64_t now_ns = 0;
	std::span<const host::PresentEvent> events;
	std::span<const host::PresentedSoldier> others;
	std::span<const host::PresentedOtherWeapon> other_weapons;
	std::span<const MuzzlePoint> muzzles;
	ListenerState listener;
	uint8_t own_slot = 0xFF;
	bool own_first_person = true;
	std::span<const KitAudioVariant> sound_variants;
};

class GameAudioDriver {
public:
	void step(WeaponAudioRouter &r_router, ImpactAudio *r_impacts, SimMixerClock &r_clock, const GameAudioInputs &p_in,
			std::span<const ImpactSoundRequest> p_impacts = {}, std::span<const EffectSoundRequest> p_effects = {});

private:
	std::array<host::PresentedOtherWeapon, net::kMaxSlots> newest_{};
	host::OwnWeaponsPresenter own_weapons_;
};

inline constexpr const char *kAuditionSuperbundles = "Win32/Levels/MP_001/MP_001";

struct GameAudioLoadOptions {

	std::string audition_superbundles = kAuditionSuperbundles;
};

struct GameAuditionAsset {
	std::shared_ptr<const core::audio::SoundProgram> program;
	std::shared_ptr<const core::audio::WaveBank> bank;
	std::string source;
};

struct GameAudioAssets {
	KitAudioSources kits;
	std::shared_ptr<const WeaponAudioTable> table;
	std::shared_ptr<const ImpactSoundCache> impacts;
	std::map<std::string, GameAuditionAsset, std::less<>> auditions;
	std::shared_ptr<const core::audio::WaveBank> bank;
	std::vector<std::string> report;
	std::vector<std::string> audition_failures;
	uint32_t programs = 0, program_failures = 0;
	size_t waves_missing = 0;
	size_t audition_waves_missing = 0;
};
core::Result<GameAudioAssets> load_game_audio_assets(const std::shared_ptr<const core::Install> &p_install, const core::level::LoadedLevel &p_level,
		const host::GunplayLevel &p_gunplay, std::span<const std::string> p_impact_sounds, unsigned p_threads,
		const std::atomic<bool> *p_stop = nullptr, const GameAudioLoadOptions &p_options = {});

core::Result<std::shared_ptr<const core::audio::SoundProgram>> compile_patch_lenient(const core::EbxResolver &p_db, std::string_view p_patch);

bool start_audition(OneShotPool &r_pool, MixerCore &r_mixer, std::shared_ptr<const core::audio::SoundProgram> p_program,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, const ListenerState &p_listener, const Vec3 &p_position,
		uint64_t p_seed, uint64_t p_frame, GameAudioCheck *p_check);

}

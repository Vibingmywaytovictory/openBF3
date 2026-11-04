#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/footstep_driver.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_sound.h"
#include "bf3/core/gameplay/soldier_sound.h"
#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "bf3/host/presentation_events.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/weapons/surface_materials.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace bf3::audio {

struct SoldierAudioAssets {
	core::gameplay::SoldierSoundDef def;
	core::gameplay::MaterialSounds materials;
	FootstepParams params;
	std::shared_ptr<const core::audio::SoundProgram> footsteps;
	std::shared_ptr<const core::audio::SoundProgram> own_hit;
	std::shared_ptr<const core::audio::SoundProgram> movement;
};

core::Result<SoldierAudioAssets> load_soldier_audio(const core::EbxResolver &p_db,
		const sim::weapons::SurfaceMaterialTable &p_surfaces, std::vector<std::string> &r_report);

class FootstepGroundProbe {
public:
	static core::Result<std::shared_ptr<FootstepGroundProbe>> create(
			std::shared_ptr<const sim::StaticWorldShapes> p_shapes,
			std::shared_ptr<const sim::weapons::SurfaceMaterialTable> p_surfaces,
			core::gameplay::MaterialSounds p_materials);

	std::optional<float> sound_id(const Vec3 &p_feet) const;

private:
	std::unique_ptr<sim::CollisionWorld> world_;
	std::shared_ptr<const sim::weapons::SurfaceMaterialTable> surfaces_;
	core::gameplay::MaterialSounds materials_;
};

struct OwnStepTags {
	std::span<const int32_t> tags;
	bool animated = false;
};

void footstep_soldiers(const host::PresentationFrame *p_frame, std::span<const host::PresentedSoldier> p_others,
		uint8_t p_own_slot, bool p_own_first_person, const OwnStepTags &p_own_tags,
		std::vector<FootstepSoldier> &r_out);

std::optional<Vec3> own_hit_forward(const host::PresentEvent &p_hit, std::span<const host::PresentedSoldier> p_others);

struct OwnHitStats {
	uint64_t hits = 0;
	uint64_t starts = 0;
	uint64_t refused = 0;
};
static_assert(std::is_trivially_copyable_v<OwnHitStats>);

class OwnHitAudio {
public:
	OwnHitAudio(MixerCore &r_mixer, std::shared_ptr<const core::audio::SoundProgram> p_program,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, OneShotPool &r_pool);

	void on_events(std::span<const host::PresentEvent> p_events, std::span<const host::PresentedSoldier> p_others,
			const ListenerState &p_listener, uint64_t p_frame);
	const OwnHitStats &stats() const { return stats_; }

private:
	MixerCore &mixer_;
	std::shared_ptr<const core::audio::SoundProgram> program_;
	std::shared_ptr<const core::audio::WaveBank> bank_;
	AudioTuning tuning_;
	OneShotPool &pool_;
	OwnHitStats stats_;
	std::vector<uint64_t> heard_;
};

}

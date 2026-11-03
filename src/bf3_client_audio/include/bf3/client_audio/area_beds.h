#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/gameplay/sound_area.h"
#include "bf3/core/ids.h"
#include "bf3/core/install.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/sound_area_natives.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::level {
class LoadedLevel;
}

namespace bf3::audio {

inline constexpr uint32_t kHashBedAmplitude = 698564572u;
inline constexpr double kBedRetrySeconds = 1.0;

struct AreaBedSound {
	core::Guid sound;
	std::shared_ptr<const core::audio::SoundProgram> program;
	float amplitude = 1.0f;
};

struct AreaBedAssets {
	std::vector<AreaBedSound> sounds;
	std::shared_ptr<const core::audio::WaveBank> bank;
	size_t areas = 0;
	size_t areas_with_sound = 0;
	size_t failures = 0;
	size_t waves_missing = 0;
	std::vector<std::string> errors;
};

core::Result<std::shared_ptr<const AreaBedAssets>> load_area_beds(const core::Install &p_install,
		const core::level::LoadedLevel &p_level, const core::gameplay::SoundAreaData &p_areas, unsigned p_threads);

struct AreaBed {
	uint32_t area = 0;
	uint32_t sound = UINT32_MAX;
	InstanceHandle handle = kNoInstance;
	float amplitude = 0.0f;
	uint64_t retry_frame = 0;
};

struct AreaBedsStats {
	uint64_t steps = 0;
	uint64_t starts = 0;
	uint64_t stops = 0;
	uint64_t updates = 0;
	uint64_t ended = 0;
	uint64_t push_failures = 0;
	uint64_t create_failures = 0;
	uint32_t sounding = 0;
	uint32_t peak_sounding = 0;
};

class AreaBeds {
public:
	void set_assets(std::shared_ptr<const AreaBedAssets> p_assets);
	void step(MixerCore &r_mixer, const sim::logic::SoundAreaNatives *p_areas, const AudioTuning &p_tuning,
			const ListenerState &p_listener);
	void on_cut(const MixerCutEvent &p_event);
	void stop_all(MixerCore &r_mixer);
	std::span<const AreaBed> beds() const { return beds_; }
	const AreaBedsStats &stats() const { return stats_; }
	const AreaBedAssets *assets() const { return assets_.get(); }

private:
	void bind(const sim::logic::SoundAreaNatives &p_areas);
	bool start(MixerCore &r_mixer, AreaBed &r_bed, const AudioTuning &p_tuning, const ListenerState &p_listener);
	void stop(MixerCore &r_mixer, AreaBed &r_bed);

	std::shared_ptr<const AreaBedAssets> assets_;
	const sim::logic::SoundAreaNatives *bound_ = nullptr;
	std::vector<AreaBed> beds_;
	AreaBedsStats stats_;
};

}

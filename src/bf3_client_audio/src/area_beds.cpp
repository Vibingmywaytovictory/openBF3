#include "bf3/client_audio/area_beds.h"

#include "bf3/audio/audio_load.h"
#include "bf3/core/audio/sound_patch_config.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/level/level.h"
#include "bf3/core/util/splitmix64.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace ca = core::audio;

namespace {

core::Result<std::shared_ptr<const ca::SoundProgram>> bed_program(
		const core::EbxResolver &p_db, const core::Guid &p_sound) {
	const core::EbxPartition *part = p_db.partition(p_sound);
	if (part == nullptr) {
		return core::make_error("area bed " + p_sound.to_string() + ": not on the level");
	}
	core::gameplay::WorkBudget budget;
	if (part->primary().is_a("SoundPatchAsset")) {
		core::Result<ca::SoundProgram> prog = ca::compile_sound_patch(p_db, *part, ca::CompileOptions{}, budget);
		if (!prog) {
			return prog.error();
		}
		return std::shared_ptr<const ca::SoundProgram>(std::make_shared<ca::SoundProgram>(std::move(*prog)));
	}
	core::Result<ca::SoundPatchConfiguration> config = ca::read_sound_patch_configuration(p_db, *part);
	if (!config) {
		return config.error();
	}
	const core::EbxPartition *patch = p_db.partition(config->patch);
	if (patch == nullptr) {
		return core::make_error(config->name + ": its patch is not on the level");
	}
	core::Result<ca::SoundProgram> prog = ca::compile_sound_patch(p_db, *patch, ca::CompileOptions{}, budget);
	if (!prog) {
		return core::make_error(config->name + ": " + prog.error().message);
	}
	core::Result<ca::SoundProgram> configured = ca::configure_sound_program(std::move(*prog), *config);
	if (!configured) {
		return configured.error();
	}
	return std::shared_ptr<const ca::SoundProgram>(std::make_shared<ca::SoundProgram>(std::move(*configured)));
}

float amplitude_input(const ca::SoundProgram &p_program) {
	for (const ca::SoundInput &in : p_program.inputs) {
		if (in.name_hash == kHashBedAmplitude && !in.event) {
			return in.default_value;
		}
	}
	return 1.0f;
}

}

core::Result<std::shared_ptr<const AreaBedAssets>> load_area_beds(const core::Install &p_install,
		const core::level::LoadedLevel &p_level, const core::gameplay::SoundAreaData &p_areas, unsigned p_threads) {
	auto out = std::make_shared<AreaBedAssets>();
	const core::EbxResolver &db = p_level.db();
	out->areas = p_areas.areas.size();
	std::vector<std::shared_ptr<const ca::SoundProgram>> programs;
	std::vector<core::Guid> tried;
	for (const core::gameplay::AreaDef &a : p_areas.areas) {
		if (a.sound.is_zero()) {
			continue;
		}
		++out->areas_with_sound;
		if (std::find(tried.begin(), tried.end(), a.sound) != tried.end()) {
			continue;
		}
		tried.push_back(a.sound);
		core::Result<std::shared_ptr<const ca::SoundProgram>> prog = bed_program(db, a.sound);
		if (!prog) {
			++out->failures;
			out->errors.push_back(prog.error().message);
			continue;
		}
		out->sounds.push_back(AreaBedSound{ a.sound, *prog, amplitude_input(**prog) });
		programs.push_back(*prog);
	}
	core::Result<std::vector<ca::SoundWaveInfo>> waves = read_program_waves(db, programs, &out->waves_missing);
	if (!waves) {
		return waves.error();
	}
	ca::WaveBankOptions bo;
	bo.threads = p_threads;
	core::Result<std::shared_ptr<const ca::WaveBank>> bank =
			ca::WaveBank::load(p_install, p_level.mount(), std::move(*waves), bo);
	if (!bank) {
		return bank.error();
	}
	out->bank = *bank;
	return std::shared_ptr<const AreaBedAssets>(std::move(out));
}

void AreaBeds::set_assets(std::shared_ptr<const AreaBedAssets> p_assets) {
	assets_ = std::move(p_assets);
	bound_ = nullptr;
	beds_.clear();
}

void AreaBeds::bind(const sim::logic::SoundAreaNatives &p_areas) {
	bound_ = &p_areas;
	beds_.clear();
	const core::gameplay::SoundAreaData &data = p_areas.data();
	for (uint32_t i = 0; i < data.areas.size(); ++i) {
		if (data.areas[i].sound.is_zero()) {
			continue;
		}
		for (uint32_t s = 0; s < assets_->sounds.size(); ++s) {
			if (assets_->sounds[s].sound == data.areas[i].sound) {
				beds_.push_back(AreaBed{ i, s });
				break;
			}
		}
	}
}

bool AreaBeds::start(MixerCore &r_mixer, AreaBed &r_bed, const AudioTuning &p_tuning, const ListenerState &p_listener) {
	const AreaBedSound &sound = assets_->sounds[r_bed.sound];
	const uint64_t seed = core::splitmix64(uint64_t(bound_->data().areas[r_bed.area].node) ^ (stats_.starts << 32));
	core::Result<std::unique_ptr<PatchInstance>> inst =
			PatchInstance::create(sound.program, assets_->bank, p_tuning, seed, kMixRate);
	if (!inst) {
		++stats_.create_failures;
		return false;
	}
	EmitterState emitter;
	emitter.position = p_listener.position;
	(*inst)->set_geometry(emitter, p_listener);
	(*inst)->set_parameter(kHashBedAmplitude, r_bed.amplitude);
	const InstanceHandle handle = r_mixer.next_handle();
	MixerCommand start;
	start.type = MixerCommandType::Start;
	start.handle = handle;
	start.instance = inst->get();
	if (!r_mixer.push(start)) {
		++stats_.push_failures;
		return false;
	}
	(void)inst->release();
	if (sound.program->start_event != 0) {
		MixerCommand ev;
		ev.type = MixerCommandType::PostEvent;
		ev.handle = handle;
		ev.name_hash = sound.program->start_event;
		if (!r_mixer.push(ev)) {
			++stats_.push_failures;
		}
	}
	r_bed.handle = handle;
	++stats_.starts;
	return true;
}

void AreaBeds::stop(MixerCore &r_mixer, AreaBed &r_bed) {
	MixerCommand c;
	c.type = MixerCommandType::Stop;
	c.handle = r_bed.handle;
	if (!r_mixer.push(c)) {
		++stats_.push_failures;
		return;
	}
	r_bed.handle = kNoInstance;
	++stats_.stops;
}

void AreaBeds::step(MixerCore &r_mixer, const sim::logic::SoundAreaNatives *p_areas, const AudioTuning &p_tuning,
		const ListenerState &p_listener) {
	++stats_.steps;
	if (assets_ == nullptr || p_areas == nullptr || !p_areas->listening()) {
		return;
	}
	if (bound_ != p_areas) {
		stop_all(r_mixer);
		bind(*p_areas);
	}
	const std::span<const sim::logic::SoundAreaValue> values = p_areas->areas();
	const uint64_t now = r_mixer.clock();
	uint32_t sounding = 0;
	for (AreaBed &bed : beds_) {
		const sim::logic::SoundAreaValue &v = values[bed.area];
		const float proximity = v.published && std::isfinite(v.proximity) ? std::max(0.0f, v.proximity) : 0.0f;
		const float amplitude = proximity * assets_->sounds[bed.sound].amplitude;
		if (proximity <= 0.0f) {
			if (bed.handle != kNoInstance) {
				stop(r_mixer, bed);
			}
			bed.amplitude = 0.0f;
			continue;
		}
		if (bed.handle == kNoInstance) {
			bed.amplitude = amplitude;
			if (now >= bed.retry_frame && !start(r_mixer, bed, p_tuning, p_listener)) {
				bed.retry_frame = now + uint64_t(kBedRetrySeconds * double(kMixRate));
			}
		} else if (amplitude != bed.amplitude) {
			MixerCommand c;
			c.type = MixerCommandType::SetParameter;
			c.handle = bed.handle;
			c.name_hash = kHashBedAmplitude;
			c.value = amplitude;
			if (r_mixer.push(c)) {
				bed.amplitude = amplitude;
				++stats_.updates;
			} else {
				++stats_.push_failures;
			}
		}
		sounding += bed.handle != kNoInstance ? 1u : 0u;
	}
	stats_.sounding = sounding;
	stats_.peak_sounding = std::max(stats_.peak_sounding, sounding);
}

void AreaBeds::on_cut(const MixerCutEvent &p_event) {
	const MixerCutKind k = p_event.kind;
	if (k != MixerCutKind::Finish && k != MixerCutKind::Refused && k != MixerCutKind::Cull && k != MixerCutKind::Kill &&
			k != MixerCutKind::Drop) {
		return;
	}
	for (AreaBed &bed : beds_) {
		if (bed.handle != kNoInstance && bed.handle == p_event.handle) {
			bed.handle = kNoInstance;
			bed.retry_frame = p_event.frame + uint64_t(kBedRetrySeconds * double(kMixRate));
			++stats_.ended;
		}
	}
}

void AreaBeds::stop_all(MixerCore &r_mixer) {
	for (AreaBed &bed : beds_) {
		if (bed.handle != kNoInstance) {
			stop(r_mixer, bed);
		}
	}
}

}

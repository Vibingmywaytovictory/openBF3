#include "bf3/client_audio/area_wave_observer.h"

#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_sound.h"

#include <algorithm>

namespace bf3::audio {

core::Result<std::vector<std::string>> observe_area_waves(std::shared_ptr<const core::audio::SoundProgram> p_program,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, float p_area_type,
		uint32_t p_event) {
	if (p_program == nullptr || p_bank == nullptr) {
		return core::make_error("area observer: no program or bank");
	}
	const std::shared_ptr<const core::audio::SoundProgram> program = p_program;
	const std::shared_ptr<const core::audio::WaveBank> bank = p_bank;
	core::Result<std::unique_ptr<PatchInstance>> made =
			PatchInstance::create(std::move(p_program), std::move(p_bank), p_tuning, 1, kMixRate);
	if (!made) {
		return made.error();
	}
	PatchInstance &inst = **made;
	SamplerTrace trace;
	trace.entries.resize(4096);
	inst.set_sampler_trace(&trace);
	inst.set_mixer_value(kHashAreaType, p_area_type);
	inst.set_parameter(kHashIsFirstPerson, 1.0f);
	EmitterState emitter;
	emitter.position = Vec3{ 0.0f, 0.0f, 1.0f };
	inst.set_geometry(emitter, ListenerState{});
	std::vector<float> storage(size_t(core::audio::MasterBus::Unknown) * kMaxBusChannels * kBlockFrames, 0.0f);
	BusBuffers buses;
	for (size_t b = 0; b < size_t(core::audio::MasterBus::Unknown); ++b) {
		for (size_t c = 0; c < kMaxBusChannels; ++c) {
			buses.channels[b][c] = storage.data() + (b * kMaxBusChannels + c) * kBlockFrames;
		}
	}
	inst.post_event(p_event, 0);
	for (uint32_t block = 0; block < kAreaObserverBlocks; ++block) {
		std::fill(storage.begin(), storage.end(), 0.0f);
		buses.frames = kBlockFrames;
		buses.first_frame = uint64_t(block) * kBlockFrames;
		inst.process_block(buses);
	}
	std::vector<std::string> names;
	for (uint32_t i = 0; i < trace.count; ++i) {
		const SamplerTraceEntry &e = trace.entries[i];
		if (e.kind != SamplerTraceKind::Trigger && e.kind != SamplerTraceKind::Step) {
			continue;
		}
		for (const core::audio::SoundWaveRef &w : program->waves) {
			const std::optional<core::audio::WaveId> id = bank->find(w.partition);
			if (id && *id == e.wave && std::find(names.begin(), names.end(), w.name) == names.end()) {
				names.push_back(w.name);
			}
		}
	}
	return names;
}

}

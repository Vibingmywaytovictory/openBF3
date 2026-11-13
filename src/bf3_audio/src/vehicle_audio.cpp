#include "bf3/audio/vehicle_audio.h"

#include "bf3/core/gameplay/vehicle_sound.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/splitmix64.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace vsp = core::gameplay::vehicle_sound_port;

namespace {

struct ParamDef {
	int32_t hash = 0;
	float step = 0.0f;
};

constexpr size_t kMaxStopping = 64;

constexpr std::array<ParamDef, kVehicleSoundParams> kParams = { {
		{ vsp::kThrottle, 0.01f },
		{ vsp::kRpm, 10.0f },
		{ vsp::kGear, 0.5f },
		{ vsp::kLoad, 0.01f },
		{ vsp::kVelocity, 0.05f },
		{ vsp::kAngularVelocity, 0.01f },
		{ vsp::kRelativeVelocity, 0.05f },
		{ vsp::kSlip, 0.02f },
		{ vsp::kTilt, 0.005f },
		{ vsp::kContact, 0.01f },
		{ vsp::kBoost, 0.5f },
		{ vsp::kSpringCompression, 0.01f },
		{ vsp::kInteriorCam, 0.5f },
		{ vsp::kLocalPlayerInVehicle, 0.5f },
		{ vsp::kHealth, 0.001f },
		{ vsp::kDisabledDamageThreshold, 0.001f },
		{ vsp::kInDriverPos, 0.5f },
} };

}

uint32_t vehicle_sound_param_hash(VehicleSoundParam p_param) {
	return uint32_t(kParams[size_t(p_param)].hash);
}

float vehicle_sound_param_step(VehicleSoundParam p_param) {
	return kParams[size_t(p_param)].step;
}

VehicleAudio::VehicleAudio(std::vector<std::shared_ptr<const core::audio::SoundProgram>> p_programs,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning) :
		programs_(std::move(p_programs)), bank_(std::move(p_bank)), tuning_(p_tuning) {
	connected_.resize(programs_.size());
	for (size_t p = 0; p < programs_.size(); ++p) {
		for (size_t i = 0; i < kVehicleSoundParams; ++i) {
			connected_[p][i] =
					programs_[p] != nullptr && programs_[p]->input_register(uint32_t(kParams[i].hash)).has_value();
		}
	}
}

bool VehicleAudio::push(
		MixerCore &r_mixer, MixerCommandType p_type, InstanceHandle p_handle, uint32_t p_hash, float p_value) {
	MixerCommand c;
	c.type = p_type;
	c.handle = p_handle;
	c.name_hash = p_hash;
	c.value = p_value;
	if (!r_mixer.push(c)) {
		++stats_.push_failures;
		return false;
	}
	return true;
}

bool VehicleAudio::start(MixerCore &r_mixer, const VehicleSoundSource &p_source, const ListenerState &p_listener) {
	const std::shared_ptr<const core::audio::SoundProgram> &program = programs_[p_source.program];
	core::Result<std::unique_ptr<PatchInstance>> inst = PatchInstance::create(program, bank_, tuning_,
			core::splitmix64((uint64_t(p_source.key) << 16) ^ p_source.program ^ (stats_.starts << 40)), kMixRate);
	if (!inst) {
		++stats_.create_failures;
		return false;
	}
	(*inst)->set_geometry(p_source.emitter, p_listener);
	Voice v;
	v.key = p_source.key;
	v.program = p_source.program;
	for (size_t i = 0; i < kVehicleSoundParams; ++i) {
		if (connected_[p_source.program][i]) {
			(*inst)->set_parameter(uint32_t(kParams[i].hash), p_source.params[i]);
		}
		v.sent[i] = p_source.params[i];
	}
	v.handle = r_mixer.next_handle();
	MixerCommand c;
	c.type = MixerCommandType::Start;
	c.handle = v.handle;
	c.instance = inst->get();
	if (!r_mixer.push(c)) {
		++stats_.push_failures;
		return false;
	}
	(void)inst->release();
	if (program->start_event != 0) {
		(void)push(r_mixer, MixerCommandType::PostEvent, v.handle, program->start_event, 0.0f);
	}
	v.seen = true;
	voices_.push_back(v);
	++stats_.starts;
	return true;
}

void VehicleAudio::stop(MixerCore &r_mixer, Voice &r_voice) {
	if (r_voice.handle == kNoInstance) {
		return;
	}
	MixerCommand c;
	c.type = MixerCommandType::Stop;
	c.handle = r_voice.handle;
	if (!r_mixer.push(c)) {
		++stats_.push_failures;
		return;
	}
	if (stopping_.size() >= kMaxStopping) {
		stopping_.erase(stopping_.begin());
	}
	stopping_.push_back(r_voice.handle);
	r_voice.handle = kNoInstance;
	++stats_.stops;
}

void VehicleAudio::step(
		MixerCore &r_mixer, std::span<const VehicleSoundSource> p_sources, const ListenerState &p_listener) {
	for (Voice &v : voices_) {
		v.seen = false;
	}
	for (const VehicleSoundSource &s : p_sources) {
		if (s.program >= programs_.size() || !programs_[s.program] || !core::finite(s.emitter.position)) {
			continue;
		}
		auto it = std::find_if(voices_.begin(), voices_.end(),
				[&](const Voice &v) { return v.handle != kNoInstance && v.key == s.key && v.program == s.program; });
		if (!s.running && it == voices_.end()) {
			continue;
		}
		stats_.peak_velocity = std::max(stats_.peak_velocity, s.params[size_t(VehicleSoundParam::Velocity)]);
		stats_.peak_throttle = std::max(stats_.peak_throttle, std::fabs(s.params[size_t(VehicleSoundParam::Throttle)]));
		stats_.peak_rpm = std::max(stats_.peak_rpm, s.params[size_t(VehicleSoundParam::Rpm)]);
		if (s.params[size_t(VehicleSoundParam::InteriorCam)] > 0.5f) {
			++stats_.interior_frames;
		}
		if (it == voices_.end()) {
			(void)start(r_mixer, s, p_listener);
			continue;
		}
		it->seen = s.running;
		MixerCommand em;
		em.type = MixerCommandType::SetEmitter;
		em.handle = it->handle;
		em.emitter = s.emitter;
		if (!r_mixer.push(em)) {
			++stats_.push_failures;
		}
		for (size_t i = 0; i < kVehicleSoundParams; ++i) {
			if (!connected_[s.program][i] || std::fabs(s.params[i] - it->sent[i]) < kParams[i].step) {
				continue;
			}
			if (push(r_mixer, MixerCommandType::SetParameter, it->handle, uint32_t(kParams[i].hash), s.params[i])) {
				it->sent[i] = s.params[i];
				++stats_.updates;
			}
		}
	}
	for (Voice &v : voices_) {
		if (!v.seen) {
			stop(r_mixer, v);
		}
	}
	std::erase_if(voices_, [](const Voice &v) { return v.handle == kNoInstance; });
	stats_.live = uint32_t(voices_.size());
	stats_.peak_live = std::max(stats_.peak_live, stats_.live);
}

void VehicleAudio::on_cut(const MixerCutEvent &p_event) {
	const MixerCutKind k = p_event.kind;
	if (k != MixerCutKind::Finish && k != MixerCutKind::Refused && k != MixerCutKind::Cull && k != MixerCutKind::Kill &&
			k != MixerCutKind::Drop) {
		return;
	}
	for (Voice &v : voices_) {
		if (v.handle != kNoInstance && v.handle == p_event.handle) {
			v.handle = kNoInstance;
			++stats_.ended;
		}
	}
	std::erase_if(voices_, [](const Voice &v) { return v.handle == kNoInstance; });
	if (const auto it = std::find(stopping_.begin(), stopping_.end(), p_event.handle); it != stopping_.end()) {
		stopping_.erase(it);
		++stats_.ended;
	}
}

void VehicleAudio::stop_all(MixerCore &r_mixer) {
	for (Voice &v : voices_) {
		stop(r_mixer, v);
	}
	std::erase_if(voices_, [](const Voice &v) { return v.handle == kNoInstance; });
	stats_.live = 0;
}

InstanceHandle VehicleAudio::handle_of(uint32_t p_key, uint16_t p_program) const {
	for (const Voice &v : voices_) {
		if (v.key == p_key && v.program == p_program) {
			return v.handle;
		}
	}
	return kNoInstance;
}

}

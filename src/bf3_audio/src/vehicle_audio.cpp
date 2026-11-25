#include "bf3/audio/vehicle_audio.h"

#include "bf3/core/gameplay/vehicle_sound.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/splitmix64.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace vsp = core::gameplay::vehicle_sound_port;

namespace {

constexpr size_t kMaxStopping = 64;

constexpr std::array<int32_t, kVehicleSoundParams> kParams = { {
		vsp::kThrottle,
		vsp::kRpm,
		vsp::kGear,
		vsp::kLoad,
		vsp::kVelocity,
		vsp::kAngularVelocity,
		vsp::kRelativeVelocity,
		vsp::kSlip,
		vsp::kTilt,
		vsp::kContact,
		vsp::kBoost,
		vsp::kSpringCompression,
		vsp::kInteriorCam,
		vsp::kLocalPlayerInVehicle,
		vsp::kHealth,
		vsp::kDisabledDamageThreshold,
		vsp::kInDriverPos,
		vsp::kRoll,
		vsp::kGForce,
} };
}

uint32_t vehicle_sound_param_hash(VehicleSoundParam p_param) {
	return uint32_t(kParams[size_t(p_param)]);
}

VehicleAudio::VehicleAudio(std::vector<std::shared_ptr<const core::audio::SoundProgram>> p_programs,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning) :
		programs_(std::move(p_programs)), bank_(std::move(p_bank)), tuning_(p_tuning) {
	connected_.resize(programs_.size());
	for (size_t p = 0; p < programs_.size(); ++p) {
		for (size_t i = 0; i < kVehicleSoundParams; ++i) {
			connected_[p][i] =
					programs_[p] != nullptr && programs_[p]->input_register(uint32_t(kParams[i])).has_value();
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
			(*inst)->set_parameter(uint32_t(kParams[i]), p_source.params[i]);
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
	v.seen = true;
	++stats_.starts;
	(void)post(r_mixer, v, true);
	voices_.push_back(v);
	return true;
}

bool VehicleAudio::post(MixerCore &r_mixer, Voice &r_voice, bool p_start) {
	const core::audio::SoundProgram &program = *programs_[r_voice.program];
	const uint32_t event = p_start ? program.start_event : program.stop_event;
	r_voice.running = p_start;
	if (event == 0 || !push(r_mixer, MixerCommandType::PostEvent, r_voice.handle, event, 0.0f)) {
		return false;
	}
	++(p_start ? stats_.start_events : stats_.stops);
	return true;
}

void VehicleAudio::update(MixerCore &r_mixer, Voice &r_voice, const VehicleSoundSource &p_source) {
	MixerCommand em;
	em.type = MixerCommandType::SetEmitter;
	em.handle = r_voice.handle;
	em.emitter = p_source.emitter;
	if (!r_mixer.push(em)) {
		++stats_.push_failures;
	}
	for (size_t i = 0; i < kVehicleSoundParams; ++i) {
		if (!connected_[p_source.program][i] || p_source.params[i] == r_voice.sent[i]) {
			continue;
		}
		if (push(r_mixer, MixerCommandType::SetParameter, r_voice.handle, uint32_t(kParams[i]), p_source.params[i])) {
			r_voice.sent[i] = p_source.params[i];
			++stats_.updates;
		}
	}
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
	stats_.stops += r_voice.running ? 1u : 0u;
}

void VehicleAudio::step(
		MixerCore &r_mixer, std::span<const VehicleSoundSource> p_sources, const ListenerState &p_listener) {
	for (Voice &v : voices_) {
		v.seen = false;
	}
	for (Activation &a : activations_) {
		a.seen = false;
	}
	for (const VehicleSoundSource &s : p_sources) {
		if (s.program >= programs_.size() || !programs_[s.program] || !core::finite(s.emitter.position)) {
			continue;
		}
		bool activated = s.running;
		if (s.kind == VehicleSoundKind::Engine) {
			auto a = std::find_if(
					activations_.begin(), activations_.end(), [&](const Activation &p_a) { return p_a.key == s.key; });
			if (a == activations_.end()) {
				activations_.push_back(Activation{ s.key, false, false, false });
				a = activations_.end() - 1;
			}
			activated = s.running && (!a->running || a->lost);
			a->running = s.running;
			a->seen = true;
			a->lost = false;
		}
		auto it = std::find_if(voices_.begin(), voices_.end(),
				[&](const Voice &v) { return v.handle != kNoInstance && v.key == s.key && v.program == s.program; });
		const float rpm = s.params[size_t(VehicleSoundParam::Rpm)];
		if (it == voices_.end()) {
			const bool spin_up = s.kind == VehicleSoundKind::Spin && rpm >= s.previous_rpm && rpm >= s.start_rpm;
			if ((s.kind == VehicleSoundKind::Engine && activated) || spin_up) {
				const bool started = start(r_mixer, s, p_listener);
				mark_lost(s.key, s.kind == VehicleSoundKind::Engine && !started);
			}
			continue;
		}
		it->seen = true;
		stats_.peak_velocity = std::max(stats_.peak_velocity, s.params[size_t(VehicleSoundParam::Velocity)]);
		stats_.peak_throttle = std::max(stats_.peak_throttle, std::fabs(s.params[size_t(VehicleSoundParam::Throttle)]));
		stats_.peak_rpm = std::max(stats_.peak_rpm, rpm);
		if (s.params[size_t(VehicleSoundParam::InteriorCam)] > 0.5f) {
			++stats_.interior_frames;
		}
		update(r_mixer, *it, s);
		if (s.kind == VehicleSoundKind::Engine && s.running != it->running) {
			(void)post(r_mixer, *it, s.running);
		} else if (s.kind == VehicleSoundKind::Spin && it->running && rpm <= s.previous_rpm && rpm < s.stop_rpm) {
			(void)post(r_mixer, *it, false);
		}
	}
	for (Voice &v : voices_) {
		if (!v.seen) {
			stop(r_mixer, v);
		}
	}
	std::erase_if(voices_, [](const Voice &v) { return v.handle == kNoInstance; });
	std::erase_if(activations_, [](const Activation &a) { return !a.seen; });
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
			mark_lost(v.key, k != MixerCutKind::Finish);
		}
	}
	std::erase_if(voices_, [](const Voice &v) { return v.handle == kNoInstance; });
	if (const auto it = std::find(stopping_.begin(), stopping_.end(), p_event.handle); it != stopping_.end()) {
		stopping_.erase(it);
		++stats_.ended;
	}
}

void VehicleAudio::mark_lost(uint32_t p_key, bool p_lost) {
	for (Activation &a : activations_) {
		if (a.key == p_key) {
			a.lost = p_lost;
		}
	}
}

void VehicleAudio::stop_all(MixerCore &r_mixer) {
	for (Voice &v : voices_) {
		stop(r_mixer, v);
	}
	std::erase_if(voices_, [](const Voice &v) { return v.handle == kNoInstance; });
	activations_.clear();
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

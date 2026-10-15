// The weapon seam (weapon_sound.h, design §6.7): binding a compiled weapon patch's inputs and turning trigger state
// into frame-stamped mixer commands.

#include "bf3/audio/weapon_sound.h"

#include "bf3/core/audio/sound_ports.h"

#include <cmath>

namespace bf3::audio {

namespace ca = core::audio;
namespace cp = core::audio::port;

namespace {

// The game input that feeds register p_reg: the input itself, or the AudioGraphParameter/Event a ReceiveEntry copies
// into it.
const ca::SoundInput *input_behind(const ca::SoundProgram &p, uint16_t p_reg, bool p_event) {
	if (p_reg == ca::kNoRegister) {
		return nullptr;
	}
	for (const ca::SoundInput &in : p.inputs) {
		if (in.reg == p_reg && in.event == p_event) {
			return &in;
		}
	}
	for (const ca::SoundNode &n : p.nodes) {
		if (n.type != ca::SoundNodeType::Receive) {
			continue;
		}
		for (uint16_t c = 0; c < n.children_count; ++c) {
			const ca::SoundNode &e = p.child_nodes[p.children[n.children_begin + c]];
			if (p.ports[e.ports_begin + cp::ReceiveOut].reg == p_reg) {
				const uint16_t source = p.ports[e.ports_begin + cp::ReceiveSource].reg;
				for (const ca::SoundInput &in : p.inputs) {
					if (in.reg == source && in.event == p_event) {
						return &in;
					}
				}
			}
		}
	}
	return nullptr;
}

} // namespace

core::Result<WeaponSoundBinding> bind_weapon_sound(std::shared_ptr<const ca::SoundProgram> p_program, double p_shot_interval_seconds, uint32_t p_mix_rate) {
	if (!p_program) {
		return core::make_error("bind_weapon_sound: no program");
	}
	const ca::SoundProgram &p = *p_program;
	if (p.start_event == 0) {
		return core::make_error("bind_weapon_sound: " + p.name + " has no DefaultStartEvent");
	}
	if (!(p_shot_interval_seconds > 0.0) || !std::isfinite(p_shot_interval_seconds) || p_mix_rate == 0) {
		return core::make_error("bind_weapon_sound: bad shot interval or rate");
	}
	WeaponSoundBinding b;
	b.start_event = p.start_event;
	b.stop_event = p.stop_event;
	b.shot_interval_seconds = p_shot_interval_seconds;
	b.mix_rate = p_mix_rate;
	for (const ca::SoundNode &n : p.nodes) {
		if (n.type != ca::SoundNodeType::Sampler) {
			continue;
		}
		if (!b.step_event) {
			if (const ca::SoundInput *in = input_behind(p, p.ports[n.ports_begin + cp::SmpStep].reg, true)) {
				b.step_event = in->name_hash;
			}
		}
		if (!b.enable_step_parameter) {
			if (const ca::SoundInput *in = input_behind(p, p.ports[n.ports_begin + cp::SmpEnableStep].reg, false)) {
				b.enable_step_parameter = in->name_hash;
			}
		}
	}
	b.program = std::move(p_program);
	return b;
}

WeaponSoundDriver::WeaponSoundDriver(const WeaponSoundBinding &p_binding, WeaponFireMode p_mode, InstanceHandle p_instance) :
		binding_(p_binding), mode_(p_mode), instance_(p_instance) {}

void WeaponSoundDriver::prepare(PatchInstance &r_instance, bool p_first_person) const {
	r_instance.set_parameter(kHashIsFirstPerson, p_first_person ? 1.0f : 0.0f);
	if (binding_.enable_step_parameter) {
		r_instance.set_parameter(*binding_.enable_step_parameter, mode_ == WeaponFireMode::Single ? 0.0f : 1.0f);
	}
}

uint64_t WeaponSoundDriver::round_frame(uint32_t p_round) const {
	return first_frame_ + uint64_t(std::llround(double(p_round) * binding_.shot_interval_seconds * double(binding_.mix_rate)));
}

namespace {

MixerCommand event_command(InstanceHandle p_handle, uint32_t p_hash, uint64_t p_frame) {
	MixerCommand c;
	c.type = MixerCommandType::PostEvent;
	c.handle = p_handle;
	c.name_hash = p_hash;
	c.at_frame = p_frame;
	return c;
}

} // namespace

bool WeaponSoundDriver::trigger_down(MixerCore &r_mixer, uint64_t p_first_frame) {
	first_frame_ = p_first_frame;
	held_ = true;
	return r_mixer.push(event_command(instance_, binding_.start_event, p_first_frame));
}

bool WeaponSoundDriver::round_fired(MixerCore &r_mixer, uint32_t p_round) {
	if (p_round == 0) {
		return true;
	}
	const uint32_t hash = (mode_ == WeaponFireMode::Single || !binding_.step_event) ? binding_.start_event : *binding_.step_event;
	return r_mixer.push(event_command(instance_, hash, round_frame(p_round)));
}

bool WeaponSoundDriver::trigger_up(MixerCore &r_mixer, uint64_t p_frame) {
	held_ = false;
	if (binding_.stop_event == 0) {
		return true;
	}
	return r_mixer.push(event_command(instance_, binding_.stop_event, p_frame));
}

} // namespace bf3::audio

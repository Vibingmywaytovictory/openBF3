#include "bf3/client_audio/area_feed.h"

#include "bf3/audio/weapon_sound.h"

namespace bf3::audio {

void AreaFeed::step(MixerCore &r_mixer, const sim::logic::SoundAreaNatives *p_areas) {
	++stats_.steps;
	if (p_areas == nullptr || !p_areas->listening()) {
		return;
	}
	const float area_type = p_areas->area_type();
	if (stats_.posted && area_type == stats_.area_type) {
		return;
	}
	MixerCommand c;
	c.type = MixerCommandType::SetMixerValue;
	c.name_hash = kHashAreaType;
	c.value = area_type;
	if (!r_mixer.push(c)) {
		++stats_.push_failures;
		return;
	}
	++stats_.posts;
	stats_.posted = true;
	stats_.area_type = area_type;
}

void IrReverbFeed::step(MixerCore &r_mixer, const sim::logic::IrReverbNatives *p_reverbs, const ImpulseBank *p_bank) {
	++stats_.steps;
	if (p_reverbs != bound_ || p_bank != bound_bank_) {
		bound_ = p_reverbs;
		bound_bank_ = p_bank;
		impulse_of_.clear();
		stats_.unmatched = 0;
		if (p_reverbs != nullptr) {
			for (const sim::logic::IrReverbDef &z : p_reverbs->zones()) {
				const std::optional<uint32_t> i = p_bank != nullptr ? p_bank->find(z.impulse) : std::nullopt;
				impulse_of_.push_back(i.value_or(kNoImpulse));
				stats_.unmatched += i ? 0 : 1;
			}
		}
		stats_.zones = impulse_of_.size();
	}
	zones_.clear();
	if (p_reverbs != nullptr && p_reverbs->updated()) {
		const std::span<const sim::logic::IrReverbValue> values = p_reverbs->values();
		for (size_t i = 0; i < values.size() && i < impulse_of_.size(); ++i) {
			zones_.push_back(IrZone{ impulse_of_[i], values[i].amplitude });
		}
	}
	const std::array<IrSlot, kIrSlots> slots = controller_.choose(zones_);
	for (uint32_t s = 0; s < kIrSlots; ++s) {
		if (sent_[s] && slots[s] == posted_[s]) {
			continue;
		}
		MixerCommand c;
		c.type = MixerCommandType::SetReverbSlot;
		c.name_hash = s;
		c.impulse = slots[s].impulse;
		c.value = slots[s].amplitude;
		if (!r_mixer.push(c)) {
			++stats_.push_failures;
			continue;
		}
		++stats_.posts;
		posted_[s] = slots[s];
		sent_[s] = true;
	}
	stats_.slots = slots;
}

}

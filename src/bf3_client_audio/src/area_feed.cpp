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

}

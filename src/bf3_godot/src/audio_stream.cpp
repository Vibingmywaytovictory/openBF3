// Bf3AudioStream / Bf3AudioPlayback (audio_stream.h).

#include "audio_stream.h"

#include <cstring>

namespace bf3::godot_side {

void Bf3AudioPlayback::_start(double) {
	playing_ = true;
}

void Bf3AudioPlayback::_stop() {
	playing_ = false;
}

bool Bf3AudioPlayback::_is_playing() const {
	return playing_;
}

int32_t Bf3AudioPlayback::_mix(godot::AudioFrame *p_buffer, float, int32_t p_frames) {
	static_assert(sizeof(godot::AudioFrame) == 2 * sizeof(float));
	if (p_frames <= 0) {
		return 0;
	}
	// Interface stage: silence. The A1 wave replaces this with mixer_->mix(out, p_frames) once MixerCore exists
	// (design §6.5); the signature and the always-return-p_frames rule are fixed here.
	float *out = reinterpret_cast<float *>(p_buffer);
	std::memset(out, 0, size_t(p_frames) * 2 * sizeof(float));
	return p_frames;
}

godot::Ref<godot::AudioStreamPlayback> Bf3AudioStream::_instantiate_playback() const {
	godot::Ref<Bf3AudioPlayback> playback;
	playback.instantiate();
	playback->set_mixer(mixer_);
	return playback;
}

} // namespace bf3::godot_side

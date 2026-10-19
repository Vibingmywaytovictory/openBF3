// Bf3AudioStream / Bf3AudioPlayback (audio_stream.h).

#include "audio_stream.h"

#include <cstring>
#if defined(_WIN32) // PT1 snd
#define WIN32_LEAN_AND_MEAN // PT1 snd
#define NOMINMAX // PT1 snd
#include <windows.h> // PT1 snd
#endif // PT1 snd

namespace bf3::godot_side {

namespace { // PT1 snd
std::atomic<int> g_pt1_priority{ kPt1NoPriority }; // PT1 snd
void pt1_note_audio_thread() { // PT1 snd
#if defined(_WIN32) // PT1 snd
	if (g_pt1_priority.load(std::memory_order_relaxed) == kPt1NoPriority) { // PT1 snd
		g_pt1_priority.store(GetThreadPriority(GetCurrentThread()), std::memory_order_relaxed); // PT1 snd
	} // PT1 snd
#endif // PT1 snd
} // PT1 snd
} // namespace PT1 snd

void Bf3AudioPlayback::_start(double) {
	playing_.store(true, std::memory_order_release);
}

void Bf3AudioPlayback::_stop() {
	playing_.store(false, std::memory_order_release);
}

bool Bf3AudioPlayback::_is_playing() const {
	return playing_.load(std::memory_order_acquire);
}

int32_t Bf3AudioPlayback::_mix(godot::AudioFrame *p_buffer, float, int32_t p_frames) {
	static_assert(sizeof(godot::AudioFrame) == 2 * sizeof(float));
	if (p_frames <= 0) {
		return 0;
	}
	// AudioFrame {left, right} is interleaved stereo: the mixer writes into Godot's buffer directly. Always the whole
	// buffer (returning fewer frames makes Godot fade the stream out and end it).
	float *out = reinterpret_cast<float *>(p_buffer);
	pt1_note_audio_thread(); // PT1 snd
	if (mixer_ == nullptr || !playing_.load(std::memory_order_acquire)) {
		std::memset(out, 0, size_t(p_frames) * 2 * sizeof(float));
		return p_frames;
	}
	mixer_->mix(out, uint32_t(p_frames));
	return p_frames;
}

godot::Ref<godot::AudioStreamPlayback> Bf3AudioStream::_instantiate_playback() const {
	godot::Ref<Bf3AudioPlayback> playback;
	playback.instantiate();
	playback->set_mixer(mixer_);
	return playback;
}

// PT1 snd (docs/design/pt1-snd.md)
int pt1_audio_thread_priority() {
	return g_pt1_priority.load(std::memory_order_relaxed);
}

} // namespace bf3::godot_side

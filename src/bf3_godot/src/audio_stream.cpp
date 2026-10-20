#include "audio_stream.h"

#include <atomic>
#include <cstring>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bf3::godot_side {

namespace {
std::atomic<int> g_pt1_priority{ kPt1NoPriority };
std::atomic<int> g_pt1_request{ kPt1NoPriority };
std::atomic<int> g_pt1_set{ kPt1NoPriority };
void pt1_note_audio_thread() {
#if defined(_WIN32)
	if (g_pt1_priority.load(std::memory_order_relaxed) == kPt1NoPriority) {
		g_pt1_priority.store(GetThreadPriority(GetCurrentThread()), std::memory_order_relaxed);
	}
	const int want = g_pt1_request.exchange(kPt1NoPriority, std::memory_order_relaxed);
	if (want != kPt1NoPriority) {
		const bool ok = SetThreadPriority(GetCurrentThread(), want) != 0;
		g_pt1_set.store(ok ? GetThreadPriority(GetCurrentThread()) : kPt1NoPriority, std::memory_order_relaxed);
	}
#endif
}
}

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

	float *out = reinterpret_cast<float *>(p_buffer);
	pt1_note_audio_thread();
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

int pt1_audio_thread_priority() {
	return g_pt1_priority.load(std::memory_order_relaxed);
}

void pt1_request_audio_thread_priority(int p_priority) {

	const bool valid = p_priority == -15 || p_priority == 15 || (p_priority >= -2 && p_priority <= 2);
	g_pt1_request.store(valid ? p_priority : kPt1NoPriority, std::memory_order_relaxed);
}

int pt1_audio_thread_priority_set() {
	return g_pt1_set.load(std::memory_order_relaxed);
}

}

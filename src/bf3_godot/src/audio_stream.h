#pragma once

#include "bf3/audio/mixer_core.h"

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>

#include <atomic>
#include <memory>

namespace bf3::godot_side {

class Bf3AudioPlayback : public godot::AudioStreamPlayback {
	GDCLASS(Bf3AudioPlayback, godot::AudioStreamPlayback)

public:
	void set_mixer(std::shared_ptr<bf3::audio::MixerCore> p_mixer) { mixer_ = std::move(p_mixer); }

	void _start(double p_from_pos) override;
	void _stop() override;
	bool _is_playing() const override;
	int32_t _mix(godot::AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) override;

protected:
	static void _bind_methods() {}

private:
	std::shared_ptr<bf3::audio::MixerCore> mixer_;
	std::atomic<bool> playing_{ false };
};

class Bf3AudioStream : public godot::AudioStream {
	GDCLASS(Bf3AudioStream, godot::AudioStream)

public:
	void set_mixer(std::shared_ptr<bf3::audio::MixerCore> p_mixer) { mixer_ = std::move(p_mixer); }
	godot::Ref<godot::AudioStreamPlayback> _instantiate_playback() const override;
	godot::String _get_stream_name() const override { return "openBF3 mixer"; }
	bool _is_monophonic() const override { return true; }

protected:
	static void _bind_methods() {}

private:
	std::shared_ptr<bf3::audio::MixerCore> mixer_;
};

inline constexpr int kPt1NoPriority = -1000;
int pt1_audio_thread_priority();

void pt1_request_audio_thread_priority(int p_priority);
int pt1_audio_thread_priority_set();

}

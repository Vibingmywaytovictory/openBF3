#pragma once
// Bf3AudioStream / Bf3AudioPlayback: the Godot end of the M2a audio mixer (docs/design/m2a-audio.md §6.5, §7). An
// AudioStream resource whose playback pulls interleaved stereo float blocks from a bf3::audio::MixerCore on Godot's
// audio thread. One AudioStreamPlayer (bus "Master") plays it for the whole session; every game sound is a voice
// inside the mixer core, not a Godot node (plan §3.10: Godot's AudioServer gives at most 6 buses per playback and no
// convolution, so the mix happens in bf3_audio).
//
// _mix(AudioFrame *buffer, float rate_scale, int frames): AudioFrame is {float left, right}, the layout of
// interleaved stereo, so MixerCore::mix writes into the buffer directly. It always returns `frames` (returning fewer
// makes Godot fade the stream out and end it). rate_scale is ignored: the project runs the driver at 48 kHz
// (project.godot [audio] driver/mix_rate=48000, the AudioSystemAsset SampleRate), so the stream's mix rate equals
// the server's and Godot does not resample. The call size is not assumed: MixerCore serves any frame count from
// its own 512-frame grid.
//
// Lifetime: the playback holds a shared_ptr to the MixerCore but never the last one; Bf3LevelSession owns the
// mixer, stops the player, waits one AudioServer lock()/unlock() and then releases it on the game thread
// (mixer_core.h), so nothing is freed on Godot's audio thread.
//
// Registered in register_types.cpp (M2a audio block); Bf3AudioDemo (audio_demo.h) creates them. set_mixer on a
// playback that Godot's audio thread may be using is called only under AudioServer::lock().

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
	std::atomic<bool> playing_{ false }; // written by _start/_stop (main thread), read by _mix (audio thread)
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

// PT1 snd (docs/design/pt1-snd.md): the Win32 priority of the thread Godot calls _mix on (GetThreadPriority, read once
// at the first _mix), for the audio health line; kPt1NoPriority before the first call or off Windows.
inline constexpr int kPt1NoPriority = -1000;
int pt1_audio_thread_priority();
// PT1 snd [prio]: a priority to set on that thread at the next _mix (SetThreadPriority; kPt1NoPriority leaves it).
// Bf3GameAudio asks for THREAD_PRIORITY_HIGHEST (2): in windowed rounds blocks of up to 107 ms were measured beside
// the main thread's hitches while the mixer's own work stayed under 4 ms (docs/design/pt1-snd.md §3.4).
// pt1_audio_thread_priority_set() is the priority after the change (kPt1NoPriority when none was asked or it failed).
void pt1_request_audio_thread_priority(int p_priority);
int pt1_audio_thread_priority_set();

} // namespace bf3::godot_side

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
// Registered in register_types.cpp by the A1 wave (the classes compile now and _mix renders silence; nothing
// creates them yet).

#include "bf3/audio/mixer_core.h"

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>

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
	bool playing_ = false;
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

} // namespace bf3::godot_side

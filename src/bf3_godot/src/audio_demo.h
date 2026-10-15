#pragma once
// Bf3AudioDemo: the M2a audio demo and gate G8 (docs/design/m2a-audio.md §1, §10.3, §10.4). Scene:
// res://scenes/audio_m16.tscn.
//
// A loader thread reads the install (Metro TDM, the M16A3 patch compiled strictly, a WaveBank of exactly the waves it
// names); the main thread then creates the MixerCore, a Bf3AudioStream on it and one AudioStreamPlayer (Master bus),
// waits for Godot's audio thread to start mixing, and runs the M16A3 check script (audio_check.h, the script G7
// renders offline) through a CheckDriver pumped once per frame, from MixerCore::earliest_frame(). A WavRecorder tap
// on the mixer records its output from frame 0.
//
// User args (after "--"):
//   --audio-check            gate G8: once the script is done and 31 s are recorded from its start, the player stops
//                            and a worker thread runs analyse_check on the recording (rows (a)-(d)) plus the Godot
//                            rows: recording >= 30.0 s; short_blocks = late_blocks = late_commands = ring_overflows =
//                            starved_voices = 0; the server mix rate is 48000; an offline replay of the same script
//                            (run_check_offline at the same start frame) equals the recording sample for sample.
//                            max_block_ns and max_call_gap_ns are printed. Prints the PASS/FAIL table,
//                            install.unchanged and "AUDIO CHECK: PASS|FAIL (n/m)", then quits 0/1
//   --audio-record=<wav>     save the recording [start, start + 31 s) as float WAV, and the core and noise taps beside it
//                            (<stem>_core.wav, <stem>_noise.wav); through the install's PathGuard
//   --quit-after=<s>         quit (as a failure in check mode) when not done after s seconds
//   --bf3-install=<dir>      install override, as for GameSession
// Without --audio-check the script plays once and the demo stays open.

#include "audio_stream.h"

#include "bf3/audio/audio_check.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/wav_io.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"

#include <godot_cpp/classes/node.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class AudioStreamPlayer;
class Label;
} // namespace godot

namespace bf3::godot_side {

class Bf3AudioDemo : public godot::Node {
	GDCLASS(Bf3AudioDemo, godot::Node)

public:
	Bf3AudioDemo();
	~Bf3AudioDemo() override;

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods() {}

private:
	enum class Phase : uint8_t { Loading, WaitAudio, Running, Analysing, Done };

	void start_audio();
	void start_analysis();
	void finish(int p_code);
	void join_threads();
	void stop_player();

	bf3::core::InstallReader install_;
	bf3::core::InstallSnapshot install_before_;
	bf3::audio::AudioTuning tuning_;
	bf3::audio::CheckScript script_;
	bool check_mode_ = false;
	std::string record_path_;
	double quit_after_ = 0.0;
	double elapsed_ = 0.0;
	Phase phase_ = Phase::Loading;

	// Loader thread results (read by the main thread once loaded_ is set).
	std::thread loader_;
	std::atomic<bool> loaded_{ false };
	std::string load_error_;
	double load_seconds_ = 0.0;
	std::unique_ptr<bf3::core::level::LoadedLevel> level_;
	std::shared_ptr<const bf3::core::audio::SoundProgram> program_;
	std::shared_ptr<const bf3::core::audio::WaveBank> bank_;

	// Audio (main thread; the mixer is shared with the playback on Godot's audio thread).
	std::shared_ptr<bf3::audio::MixerCore> mixer_;
	std::unique_ptr<bf3::audio::WavRecorder> recorder_;
	std::unique_ptr<bf3::audio::CheckDriver> driver_;
	godot::Ref<Bf3AudioStream> stream_;
	godot::AudioStreamPlayer *player_ = nullptr;
	godot::Label *label_ = nullptr;
	uint64_t start_frame_ = 0;
	uint64_t record_frames_ = 0;
	double server_mix_rate_ = 0.0;

	// Analysis thread results.
	std::thread analyser_;
	std::atomic<bool> analysed_{ false };
	bf3::audio::CheckReport report_;
	std::vector<std::string> notes_;
};

} // namespace bf3::godot_side

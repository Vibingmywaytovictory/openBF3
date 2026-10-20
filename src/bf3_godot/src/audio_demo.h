#pragma once

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
}

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

	std::thread loader_;
	std::atomic<bool> loaded_{ false };
	std::string load_error_;
	double load_seconds_ = 0.0;
	std::unique_ptr<bf3::core::level::LoadedLevel> level_;
	std::shared_ptr<const bf3::core::audio::SoundProgram> program_;
	std::shared_ptr<const bf3::core::audio::WaveBank> bank_;

	std::shared_ptr<bf3::audio::MixerCore> mixer_;
	std::unique_ptr<bf3::audio::WavRecorder> recorder_;
	std::unique_ptr<bf3::audio::CheckDriver> driver_;
	godot::Ref<Bf3AudioStream> stream_;
	godot::AudioStreamPlayer *player_ = nullptr;
	godot::Label *label_ = nullptr;
	uint64_t start_frame_ = 0;
	uint64_t record_frames_ = 0;
	double server_mix_rate_ = 0.0;

	std::thread analyser_;
	std::atomic<bool> analysed_{ false };
	bf3::audio::CheckReport report_;
	std::vector<std::string> notes_;
};

}

// Bf3AudioDemo (audio_demo.h).

#include "audio_demo.h"

#include "godot_util.h"

#include "bf3/audio/audio_load.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace godot;
namespace ba = bf3::audio;
namespace ca = bf3::core::audio;

namespace bf3::godot_side {

namespace {

constexpr double kRecordSeconds = 31.0;

void log_line(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[audio] " + p_line));
}

std::filesystem::path with_suffix(const std::filesystem::path &p_path, const std::string &p_suffix) {
	std::filesystem::path out = p_path;
	out.replace_filename(p_path.stem().string() + p_suffix + p_path.extension().string());
	return out;
}

ba::CheckRow row(const std::string &p_name, double p_value, const std::string &p_rule, bool p_pass) {
	return ba::CheckRow{ p_name, p_value, p_rule, p_pass };
}

} // namespace

Bf3AudioDemo::Bf3AudioDemo() = default;

Bf3AudioDemo::~Bf3AudioDemo() {
	stop_player();
	join_threads();
}

void Bf3AudioDemo::join_threads() {
	if (loader_.joinable()) {
		loader_.join();
	}
	if (analyser_.joinable()) {
		analyser_.join();
	}
}

void Bf3AudioDemo::stop_player() {
	if (player_ != nullptr) {
		Ref<AudioStreamPlayback> playback = player_->get_stream_playback();
		player_->stop();
		player_ = nullptr;
		// Under the AudioServer lock Godot's audio thread is outside _mix: the playback drops its mixer reference here,
		// so the mixer (and the instances it owns) is released on the main thread, and the tap is ours again.
		AudioServer::get_singleton()->lock();
		if (Bf3AudioPlayback *ours = Object::cast_to<Bf3AudioPlayback>(playback.ptr())) {
			ours->set_mixer(nullptr);
		}
		AudioServer::get_singleton()->unlock();
	}
}

void Bf3AudioDemo::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	bf3::core::InstallLocateOptions opts;
	opts.cmdline_path = user_arg_value("bf3-install");
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps->has_setting("openbf3/install/path")) {
		opts.setting_path = to_std(String(ps->get_setting("openbf3/install/path")));
	}
	install_.locate(opts);
	if (install_.is_mounted()) {
		install_before_ = install_.snapshot();
	}
	log_line("engine " + engine_summary());
	log_line("install " + install_.describe());
	check_mode_ = has_user_arg("audio-check");
	record_path_ = user_arg_value("audio-record");
	const std::string quit_after = user_arg_value("quit-after");
	quit_after_ = quit_after.empty() ? 0.0 : std::atof(quit_after.c_str());

	label_ = memnew(Label);
	label_->set_name("Status");
	label_->set_position(Vector2(12, 8));
	add_child(label_);

	// The tuning file of the project (16 keys, audio.contract checks it); a missing or bad file fails the run.
	const String cfg = FileAccess::get_file_as_string("res://config/audio_tuning.cfg");
	bf3::core::Result<ba::AudioTuning> tuning = ba::parse_audio_tuning(to_std(cfg));
	if (!tuning) {
		load_error_ = "res://config/audio_tuning.cfg: " + tuning.error().message;
		loaded_ = true;
	} else {
		tuning_ = *tuning;
	}
	script_ = ba::m16_check_script();

	if (!loaded_ && !install_.is_mounted()) {
		load_error_ = "no usable install: " + install_.describe();
		loaded_ = true;
	}
	if (!loaded_) {
		// Heavy work off the main thread (plan §2.3); no Godot API on this thread.
		loader_ = std::thread([this]() {
			const auto t0 = std::chrono::steady_clock::now();
			bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(install_);
			if (!install) {
				load_error_ = install.error().message;
				loaded_ = true;
				return;
			}
			bf3::core::Result<std::unique_ptr<bf3::core::level::LoadedLevel>> lvl = bf3::core::level::load_level(*install, "MP_Subway", "TeamDeathMatch0");
			if (!lvl) {
				load_error_ = "load_level: " + lvl.error().message;
				loaded_ = true;
				return;
			}
			bf3::core::Result<std::shared_ptr<const ca::SoundProgram>> prog = ba::compile_patch((*lvl)->db(), ba::kM16Patch);
			if (!prog) {
				load_error_ = "compile: " + prog.error().message;
				loaded_ = true;
				return;
			}
			size_t missing = 0;
			std::shared_ptr<const ca::SoundProgram> p = *prog;
			bf3::core::Result<std::vector<ca::SoundWaveInfo>> waves =
					ba::read_program_waves((*lvl)->db(), std::span<const std::shared_ptr<const ca::SoundProgram>>(&p, 1), &missing);
			if (!waves || missing != 0) {
				load_error_ = waves ? std::to_string(missing) + " waves of the M16A3 are not in the level" : waves.error().message;
				loaded_ = true;
				return;
			}
			bf3::core::Result<std::shared_ptr<const ca::WaveBank>> bank = ca::WaveBank::load(**install, (*lvl)->mount(), std::move(*waves), ca::WaveBankOptions{});
			if (!bank) {
				load_error_ = "bank: " + bank.error().message;
				loaded_ = true;
				return;
			}
			level_ = std::move(*lvl);
			program_ = p;
			bank_ = *bank;
			load_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			loaded_ = true;
		});
	}
	set_process(true);
}

void Bf3AudioDemo::start_audio() {
	ba::MixerConfig config;
	config.schedule_lead_blocks = uint32_t(tuning_.schedule_lead_blocks);
	mixer_ = std::make_shared<ba::MixerCore>(config);
	recorder_ = std::make_unique<ba::WavRecorder>(config.mix_rate, kRecordSeconds + 60.0);
	mixer_->set_tap(recorder_.get()); // before the audio thread sees the mixer
	stream_.instantiate();
	stream_->set_mixer(mixer_);
	player_ = memnew(AudioStreamPlayer);
	player_->set_name("Bf3Mixer");
	player_->set_bus("Master");
	player_->set_stream(stream_);
	add_child(player_);
	player_->play();
	server_mix_rate_ = AudioServer::get_singleton()->get_mix_rate();
	char line[256];
	std::snprintf(line, sizeof(line), "loaded in %.2f s: %s, %zu voices; %u resident waves; server mix rate %.0f Hz", load_seconds_, program_->name.c_str(),
			program_->voices.size(), unsigned(bank_->stats().resident_waves), server_mix_rate_);
	log_line(line);
	phase_ = Phase::WaitAudio;
}

void Bf3AudioDemo::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || phase_ == Phase::Done) {
		return;
	}
	elapsed_ += p_delta;
	if (quit_after_ > 0.0 && elapsed_ > quit_after_ && phase_ != Phase::Analysing) {
		char line[128];
		std::snprintf(line, sizeof(line), "timeout: not done after %.1f s (phase %d)", elapsed_, int(phase_));
		log_line(line);
		if (check_mode_) {
			std::printf("AUDIO CHECK: FAIL (timeout)\n");
			std::fflush(stdout);
		}
		finish(check_mode_ ? 1 : 0);
		return;
	}
	switch (phase_) {
		case Phase::Loading:
			if (loaded_) {
				if (loader_.joinable()) {
					loader_.join();
				}
				if (!load_error_.empty()) {
					log_line("load failed: " + load_error_);
					std::printf("AUDIO CHECK: FAIL (%s)\n", load_error_.c_str());
					std::fflush(stdout);
					finish(1);
					return;
				}
				start_audio();
			} else if (label_ != nullptr) {
				label_->set_text("openBF3 audio: loading the M16A3 patch and its waves...");
			}
			break;
		case Phase::WaitAudio:
			if (mixer_->clock() > 0) {
				start_frame_ = mixer_->earliest_frame();
				bf3::core::Result<std::unique_ptr<ba::CheckDriver>> d = ba::CheckDriver::create(script_, program_, bank_, tuning_, start_frame_, ba::kMixRate);
				if (!d) {
					log_line("check driver: " + d.error().message);
					std::printf("AUDIO CHECK: FAIL (driver)\n");
					std::fflush(stdout);
					finish(1);
					return;
				}
				driver_ = std::move(*d);
				record_frames_ = std::max<uint64_t>(driver_->total_frames(), uint64_t(kRecordSeconds * ba::kMixRate));
				log_line("script starts at frame " + std::to_string(start_frame_) + "; " + std::to_string(driver_->bursts().size()) + " bursts over " +
						std::to_string(double(driver_->total_frames()) / ba::kMixRate) + " s");
				phase_ = Phase::Running;
			}
			break;
		case Phase::Running: {
			const bf3::core::Status s = driver_->pump(*mixer_);
			if (!s) {
				log_line("pump: " + s.error().message);
			}
			mixer_->reclaim();
			std::array<ba::MixerFeedback, 64> fb;
			while (mixer_->poll_feedback(fb) == fb.size()) {
			}
			const uint64_t clock = mixer_->clock();
			if (label_ != nullptr) {
				char text[256];
				std::snprintf(text, sizeof(text), "openBF3 audio: M16A3 check script %.1f / %.1f s | active %u | late commands %llu",
						clock > start_frame_ ? double(clock - start_frame_) / ba::kMixRate : 0.0, double(record_frames_) / ba::kMixRate,
						mixer_->stats().active_instances.load(), static_cast<unsigned long long>(mixer_->stats().late_commands.load()));
				label_->set_text(text);
			}
			if (driver_->done(*mixer_) && clock >= start_frame_ + record_frames_ + 2 * ba::kBlockFrames) {
				if (check_mode_ || !record_path_.empty()) {
					stop_player();
					start_analysis();
				} else {
					log_line("script done");
					phase_ = Phase::Done;
				}
			}
			break;
		}
		case Phase::Analysing:
			if (analysed_) {
				if (analyser_.joinable()) {
					analyser_.join();
				}
				for (const std::string &n : notes_) {
					log_line(n);
				}
				if (!check_mode_) {
					phase_ = Phase::Done;
					break;
				}
				const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(install_before_, install_.snapshot());
				std::printf("install.unchanged %s%s\n", diff.clean() ? "yes" : "no ", diff.clean() ? "" : diff.summary().c_str());
				report_.rows.push_back(row("install.unchanged", diff.clean() ? 1.0 : 0.0, "Install::snapshot identical before and after", diff.clean()));
				std::printf("%s", report_.table().c_str());
				size_t failed = 0;
				for (const ba::CheckRow &r : report_.rows) {
					failed += r.pass ? 0 : 1;
				}
				std::printf("AUDIO CHECK: %s (%zu/%zu)\n", failed == 0 ? "PASS" : "FAIL", report_.rows.size() - failed, report_.rows.size());
				std::fflush(stdout);
				finish(failed == 0 ? 0 : 1);
			}
			break;
		case Phase::Done:
			break;
	}
}

void Bf3AudioDemo::start_analysis() {
	phase_ = Phase::Analysing;
	if (label_ != nullptr) {
		label_->set_text("openBF3 audio: analysing the recording...");
	}
	// The audio thread is out of _mix (stop_player): the recorder, the driver's taps and the mixer stats are stable.
	analyser_ = std::thread([this]() {
		const ba::MixerStats &st = mixer_->stats();
		const std::span<const float> rec = recorder_->samples();
		const uint64_t end = start_frame_ + record_frames_;
		const uint64_t analysed_end = start_frame_ + driver_->total_frames();
		if (rec.size() < size_t(end) * 2) {
			report_.rows.push_back(row("godot.recording", double(rec.size() / 2) / ba::kMixRate, "the recording covers the script", false));
			analysed_ = true;
			return;
		}
		const std::span<const float> mix = rec.subspan(size_t(start_frame_) * 2, size_t(analysed_end - start_frame_) * 2);
		report_ = ba::analyse_check(script_, driver_->bursts(), mix, driver_->core_tap().samples(), driver_->noise_tap().samples(), driver_->far_noise_hpf_hz(),
				ba::kMixRate, tuning_);
		char rule[256];
		const double recorded_s = double(record_frames_) / ba::kMixRate;
		report_.rows.push_back(row("godot.recording_s", recorded_s, ">= 30.0 s from the script start", recorded_s >= 30.0));
		std::snprintf(rule, sizeof(rule), "== 48000 (project [audio] driver/mix_rate)");
		report_.rows.push_back(row("godot.mix_rate", server_mix_rate_, rule, server_mix_rate_ == 48000.0));
		const std::pair<const char *, uint64_t> counters[] = { { "counters.short_blocks", st.short_blocks.load() },
			{ "counters.late_blocks", st.late_blocks.load() }, { "counters.late_commands", st.late_commands.load() },
			{ "counters.ring_overflows", st.ring_overflows.load() }, { "counters.starved_voices", st.starved_voices.load() } };
		for (const auto &[name, value] : counters) {
			report_.rows.push_back(row(name, double(value), "== 0", value == 0));
		}
		const uint64_t dropped = driver_->core_tap().dropped_frames() + driver_->noise_tap().dropped_frames() + recorder_->dropped_frames();
		report_.rows.push_back(row("counters.dropped_tap_record", double(dropped), "== 0", dropped == 0));
		std::snprintf(rule, sizeof(rule), "reported: max_block_ns %llu (block length 10,666,667 ns), max_call_gap_ns %llu, %llu blocks",
				static_cast<unsigned long long>(st.max_block_ns.load()), static_cast<unsigned long long>(st.max_call_gap_ns.load()),
				static_cast<unsigned long long>(st.blocks.load()));
		notes_.push_back(rule);
		report_.rows.push_back(row("timing.max_block_ms", double(st.max_block_ns.load()) / 1e6, rule, true));

		// The offline replay of the same script from the same start frame.
		ba::OfflineCheckOptions options;
		options.start_frame = start_frame_;
		const auto t0 = std::chrono::steady_clock::now();
		bf3::core::Result<ba::OfflineCheck> replay = ba::run_check_offline(script_, program_, bank_, tuning_, options);
		const double replay_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (!replay) {
			report_.rows.push_back(row("replay.equal", 0.0, "offline replay: " + replay.error().message, false));
		} else {
			float worst = 0.0f;
			size_t differing = 0;
			const size_t n = std::min(replay->mix.size(), mix.size());
			for (size_t i = 0; i < n; ++i) {
				const float d = std::fabs(replay->mix[i] - mix[i]);
				worst = std::max(worst, d);
				differing += d != 0.0f ? 1 : 0;
			}
			std::snprintf(rule, sizeof(rule), "== 0 (max |difference| over %zu samples, %zu differ; %s; replay %.2f s)", n, differing,
					replay->mix.size() == mix.size() ? "same length" : "LENGTHS DIFFER", replay_s);
			report_.rows.push_back(row("replay.max_abs_diff", double(worst), rule, worst == 0.0f && differing == 0 && replay->mix.size() == mix.size()));
		}

		// The recording and the taps, when asked for.
		if (!record_path_.empty()) {
			const std::filesystem::path path(std::u8string(record_path_.begin(), record_path_.end()));
			const bf3::core::PathGuard guard = install_.write_guard();
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			const std::span<const float> recorded = rec.subspan(size_t(start_frame_) * 2, size_t(record_frames_) * 2);
			const bf3::core::Status a = ba::write_wav(guard, path, recorded, 2, ba::kMixRate);
			const bf3::core::Status b = ba::write_wav(guard, with_suffix(path, "_core"), driver_->core_tap().samples(), 1, ba::kMixRate);
			const bf3::core::Status c = ba::write_wav(guard, with_suffix(path, "_noise"), driver_->noise_tap().samples(), 1, ba::kMixRate);
			const bool ok = a.ok() && b.ok() && c.ok();
			report_.rows.push_back(row("wavs.written", ok ? 3.0 : 0.0, "== 3 (" + record_path_ + ")", ok));
			if (!ok) {
				notes_.push_back("wav: " + (!a.ok() ? a.error().message : !b.ok() ? b.error().message : c.error().message));
			}
		}
		analysed_ = true;
	});
}

void Bf3AudioDemo::finish(int p_code) {
	phase_ = Phase::Done;
	stop_player();
	join_threads();
	driver_.reset();
	if (mixer_ != nullptr) {
		mixer_->set_tap(nullptr);
	}
	stream_.unref();
	mixer_.reset();
	get_tree()->quit(p_code);
}

} // namespace bf3::godot_side

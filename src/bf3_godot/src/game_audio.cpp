#include "game_audio.h"

#include "audio_stream.h"
#include "godot_util.h"

#include "bf3/audio/audio_load.h"
#include "bf3/audio/voice_cull.h"
#include "bf3/client_audio/game_audio_frame.h"
#include "bf3/client_audio/kit_audio.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/level/level.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <chrono>
#include "bf3/audio/wav_io.h"
#include "bf3/core/install.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <filesystem>
#include <fstream>

using namespace godot;
namespace ba = bf3::audio;
namespace ca = bf3::core::audio;
namespace sw = bf3::sim::weapons;

namespace bf3::godot_side {

namespace {

constexpr int64_t kBiasMarginFrames = 2 * int64_t(ba::kBlockFrames) + 800;
constexpr uint32_t kCommandCapacity = 8192;

class TapForwarder final : public ba::MixTap {
public:
	void on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t p_first_frame) override {
		if (ba::MixTap *t = target_.load(std::memory_order_acquire)) {
			t->on_block(p_interleaved_stereo, p_frames, p_first_frame);
		}
		if (ba::MixTap *t = pt1_capture_.load(std::memory_order_acquire)) {
			t->on_block(p_interleaved_stereo, p_frames, p_first_frame);
		}
	}
	void set(ba::MixTap *p_tap) {
		target_.store(p_tap, std::memory_order_release);
	}
	void pt1_set_capture(ba::MixTap *p_tap) {
		pt1_capture_.store(p_tap, std::memory_order_release);
	}

private:
	std::atomic<ba::MixTap *> target_{ nullptr };
	std::atomic<ba::MixTap *> pt1_capture_{ nullptr };
};
}

template <class Impl>
ba::MixerA3Assets load_a3(const core::EbxResolver &p_db, Impl &r_m);
struct Pt1Capture;
void pt1_delete_capture(Pt1Capture *p_capture);
template <class Impl>
void pt1_capture_begin(Impl &r_m);
template <class Impl>
void pt1_capture_step(Impl &r_m);
template <class Impl>
void pt1_capture_end(Impl &r_m);

struct Bf3GameAudio::Impl {

	const host::LevelDatabase *db = nullptr;
	std::shared_ptr<const ba::WeaponAudioTable> table;
	ba::KitAudioSources kits;
	std::shared_ptr<const ba::ImpactSoundCache> impacts_cache;
	std::map<std::string, ba::GameAuditionAsset, std::less<>> auditions;
	ba::GameAudioLoadOptions load_options;
	std::shared_ptr<const ca::WaveBank> bank;
	std::vector<std::string> report;
	uint32_t programs = 0, program_failures = 0;
	ba::MixerA3Assets a3;
	std::atomic<bool> loaded{ false };

	ba::AudioTuning tuning;
	std::shared_ptr<ba::MixerCore> mixer;
	std::unique_ptr<ba::OneShotPool> pool;
	std::unique_ptr<ba::SimMixerClock> clock;
	std::unique_ptr<ba::WeaponAudioRouter> router;
	std::unique_ptr<ba::ImpactAudio> impacts;
	TapForwarder tap;
	Ref<Bf3AudioStream> stream;
	AudioStreamPlayer *player = nullptr;
	bool built = false;
	ba::GameAudioCheck *check = nullptr;

	std::vector<ba::MuzzlePoint> muzzles;
	std::vector<ba::ImpactSoundRequest> impact_requests;
	ba::GameAudioDriver driver;
	ba::ListenerState listener;
	uint64_t auditions_started = 0;
	int64_t lead_frames = 2 * int64_t(ba::kBlockFrames);
	std::unique_ptr<Pt1Capture, void (*)(Pt1Capture *)> pt1{ nullptr, &pt1_delete_capture };
	core::PathGuard pt1_guard;
};

Bf3GameAudio::Bf3GameAudio() :
		impl_(std::make_unique<Impl>()) {}

Bf3GameAudio::~Bf3GameAudio() {
	shutdown();
}

core::Status Bf3GameAudio::load(const M2aLoadInputs &p_in, std::span<const std::string> p_impact_sounds) {
	Impl &m = *impl_;
	if (p_in.db == nullptr || !p_in.db->level || !p_in.db->install) {
		return core::make_error("Bf3GameAudio::load: no level database");
	}
	if (!p_in.shared || !p_in.shared->gunplay) {
		return core::make_error("Bf3GameAudio::load: the shared level has no gunplay tables (SharedLoadOptions::gunplay)");
	}
	m.db = p_in.db;
	m.pt1_guard = p_in.db->install->reader().write_guard();
	core::Result<ba::GameAudioAssets> a = ba::load_game_audio_assets(p_in.db->install, *p_in.db->level, *p_in.shared->gunplay, p_impact_sounds, p_in.threads, p_in.stop,
			m.load_options);
	if (!a) {
		return a.error();
	}
	m.kits = std::move(a->kits);
	m.table = std::move(a->table);
	m.impacts_cache = std::move(a->impacts);
	m.auditions = std::move(a->auditions);
	m.bank = std::move(a->bank);
	m.report = std::move(a->report);
	m.programs = a->programs;
	m.program_failures = a->program_failures;
	m.a3 = load_a3(p_in.db->level->db(), m);
	m.loaded.store(true, std::memory_order_release);
	return core::Status::success();
}

void Bf3GameAudio::set_load_options(const audio::GameAudioLoadOptions &p_options) {
	impl_->load_options = p_options;
}

bool Bf3GameAudio::loaded() const {
	return impl_->loaded.load(std::memory_order_acquire);
}

bool Bf3GameAudio::build(godot::Node *p_parent, std::string *r_error) {
	Impl &m = *impl_;
	const auto fail = [&](const std::string &p_why) {
		if (r_error != nullptr) {
			*r_error = p_why;
		}
		return false;
	};
	if (m.built) {
		return true;
	}
	if (!loaded() || p_parent == nullptr) {
		return fail("Bf3GameAudio::build: not loaded or no parent");
	}
	const String cfg = FileAccess::get_file_as_string("res://config/audio_tuning.cfg");
	core::Result<ba::AudioTuning> tuning = ba::parse_audio_tuning(to_std(cfg));
	if (!tuning) {
		return fail("res://config/audio_tuning.cfg: " + tuning.error().message);
	}
	m.tuning = *tuning;
	ba::MixerConfig mc;
	mc.max_instances = ba::kGameMixerInstances;
	mc.command_capacity = kCommandCapacity;
	mc.schedule_lead_blocks = uint32_t(std::max(1.0f, m.tuning.schedule_lead_blocks));
	mc.max_voices = ba::kGameMixerVoices;
	ba::apply_mixer_a3(m.a3, mc);
	if (const std::string fade = user_arg_value("audio-cut-fade"); !fade.empty()) {
		mc.pt1_cut_fade_frames = uint32_t(std::clamp(std::atoi(fade.c_str()), 0, 4800));
	}
	if (user_arg_value("audio-cap-hold") == "0") {
		mc.pt1_cap_hold_db = 0.0f;
		mc.pt1_voice_hold_blocks = 0;
		mc.pt1_resume_margin = 0;
	}
	if (user_arg_value("audio-limit") == "0") {
		mc.pt1_limit_ceiling = 0.0f;
	}
	m.lead_frames = int64_t(mc.schedule_lead_blocks) * int64_t(ba::kBlockFrames);
	m.mixer = std::make_shared<ba::MixerCore>(mc);
	m.mixer->set_tap(&m.tap);
	m.pool = std::make_unique<ba::OneShotPool>(ba::kMaxOneShotVoices);
	ba::SimMixerClockConfig cc;
	cc.bias_frames = m.lead_frames + kBiasMarginFrames;
	m.clock = std::make_unique<ba::SimMixerClock>(*m.mixer, cc);
	m.router = std::make_unique<ba::WeaponAudioRouter>(*m.mixer, m.table, m.bank, m.tuning, *m.pool);
	m.impacts = std::make_unique<ba::ImpactAudio>(*m.mixer, m.impacts_cache, m.bank, m.tuning, *m.pool);
	m.stream.instantiate();
	m.stream->set_mixer(m.mixer);
	m.player = memnew(AudioStreamPlayer);
	m.player->set_name("Bf3GameMixer");
	m.player->set_bus("Master");
	m.player->set_stream(m.stream);
	p_parent->add_child(m.player);
	pt1_capture_begin(m);
	m.player->play();
	m.built = true;
	return true;
}

bool Bf3GameAudio::built() const {
	return impl_->built;
}

void Bf3GameAudio::step(const M2aFrame &p_frame, std::span<const fx::ImpactSoundCue> p_impact_cues) {
	step(p_frame, p_impact_cues, {});
}

void Bf3GameAudio::step(const M2aFrame &p_frame, std::span<const fx::ImpactSoundCue> p_impact_cues, std::span<const audio::EffectSoundRequest> p_effect_sounds) {
	Impl &m = *impl_;
	if (!m.built) {
		return;
	}

	const double latency_s = AudioServer::get_singleton()->get_output_latency();
	const uint64_t latency = std::isfinite(latency_s) && latency_s > 0.0 && latency_s < 2.0 ? uint64_t(latency_s * double(ba::kMixRate)) : 0;
	m.clock->set_output_latency_frames(latency);
	m.clock->set_bias_frames(int64_t(latency) + m.lead_frames + kBiasMarginFrames);
	m.clock->set_now(p_frame.clock);

	ba::GameAudioInputs in;
	in.frame = p_frame.frame;
	in.now_ns = p_frame.now_ns;
	in.events = p_frame.events;
	in.others = p_frame.others;
	in.other_weapons = p_frame.other_weapons;
	in.listener.position = p_frame.camera.trans;
	in.listener.forward = p_frame.camera.forward;
	in.listener.up = p_frame.camera.up;
	in.listener.velocity = p_frame.camera_velocity;
	in.own_slot = p_frame.own_slot;
	in.own_first_person = p_frame.own_alive && !p_frame.free_camera;
	m.listener = in.listener;

	m.muzzles.assign(p_frame.others.size(), ba::MuzzlePoint{});
	for (size_t i = 0; i < p_frame.others.size(); ++i) {
		for (const fx::MuzzleSample &s : p_frame.muzzles) {
			if (s.valid && !s.first_person && s.slot == p_frame.others[i].slot) {
				m.muzzles[i].valid = true;
				m.muzzles[i].position = s.transform.trans;
				m.muzzles[i].forward = s.transform.up;
				break;
			}
		}
	}
	in.muzzles = m.muzzles;
	m.impact_requests.clear();
	for (const fx::ImpactSoundCue &c : p_impact_cues) {
		ba::ImpactSoundRequest r;
		r.patch = c.patch;
		r.position = c.position;
		r.normal = c.normal;
		r.seed = c.seed;
		r.time = c.time;
		r.timeline = c.timeline;
		m.impact_requests.push_back(r);
	}
	m.driver.step(*m.router, m.impacts.get(), *m.clock, in, m.impact_requests, p_effect_sounds);
	pt1_capture_step(m);
}

bool Bf3GameAudio::audition(std::string_view p_patch, const fx::Vec3 &p_position, std::string *r_error) {
	Impl &m = *impl_;
	const auto fail = [&](const std::string &p_why) {
		if (r_error != nullptr) {
			*r_error = p_why;
		}

		if (m.check != nullptr && m.check->current_step() >= 0) {
			m.check->skip_step(uint32_t(m.check->current_step()), p_why);
		}
		return false;
	};
	if (!m.built) {
		return fail("Bf3GameAudio::audition: not built");
	}
	std::shared_ptr<const ca::SoundProgram> program;
	std::shared_ptr<const ca::WaveBank> bank = m.bank;
	if (auto it = m.auditions.find(p_patch); it != m.auditions.end()) {
		program = it->second.program;
		bank = it->second.bank ? it->second.bank : m.bank;
	} else if (m.impacts_cache && (program = m.impacts_cache->find(p_patch))) {
	} else {
		if (m.db == nullptr || !m.db->level) {
			return fail("Bf3GameAudio::audition: no level database");
		}
		core::Result<std::shared_ptr<const ca::SoundProgram>> p = ba::compile_patch_lenient(m.db->level->db(), p_patch);
		if (!p) {
			++m.program_failures;
			return fail(std::string(p_patch) + ": " + p.error().message);
		}
		program = *p;

		m.auditions.emplace(std::string(p_patch), ba::GameAuditionAsset{ program, m.bank, "level (lazy)" });
	}
	const uint64_t seed = ba::sound_seed(0xA0D1u, uint32_t(m.auditions_started));
	if (!ba::start_audition(*m.pool, *m.mixer, program, bank, m.tuning, m.listener, p_position, seed,
				m.clock->earliest_frame(), m.check)) {
		return fail("Bf3GameAudio::audition: the one-shot pool refused");
	}
	++m.auditions_started;
	return true;
}

void Bf3GameAudio::set_check(audio::GameAudioCheck *p_check) {
	Impl &m = *impl_;
	m.check = p_check;
	if (m.router) {
		m.router->set_observer(p_check);
	}

	if (m.built) {
		AudioServer::get_singleton()->lock();
		m.tap.set(p_check);
		AudioServer::get_singleton()->unlock();
	} else {
		m.tap.set(p_check);
	}
}

audio::MixerCore *Bf3GameAudio::mixer() const {
	return impl_->mixer.get();
}

GameAudioStats Bf3GameAudio::stats() const {
	const Impl &m = *impl_;
	GameAudioStats s;
	s.programs = m.programs;
	s.program_failures = m.program_failures;
	if (!m.built) {
		return s;
	}
	s.router = m.router->stats();
	s.impacts = m.impacts->stats();
	const ba::MixerStats &ms = m.mixer->stats();
	s.mixer_frames = ms.frames.load(std::memory_order_relaxed);
	s.short_blocks = ms.short_blocks.load(std::memory_order_relaxed);
	s.late_blocks = ms.late_blocks.load(std::memory_order_relaxed);
	s.late_commands = ms.late_commands.load(std::memory_order_relaxed);
	s.ring_overflows = ms.ring_overflows.load(std::memory_order_relaxed);
	s.peak_instances = ms.peak_instances.load(std::memory_order_relaxed);
	s.starved_voices = ms.starved_voices.load(std::memory_order_relaxed);
	s.feedback_drops = ms.feedback_drops.load(std::memory_order_relaxed);
	s.max_block_ns = ms.max_block_ns.load(std::memory_order_relaxed);
	s.clock_late = m.clock->late();
	s.output_latency_frames = m.clock->output_latency_frames();
	s.bias_frames = m.clock->bias_frames();
	s.one_shots_live = m.pool->live();
	s.one_shots_peak = m.pool->peak();
	s.one_shots_stolen = m.pool->stolen();
	s.one_shots_failed = m.pool->failed();
	s.auditions = m.auditions_started;
	return s;
}

std::vector<std::string> Bf3GameAudio::load_report() const {
	return impl_->report;
}

void Bf3GameAudio::shutdown() {
	Impl &m = *impl_;
	if (m.player != nullptr) {
		Ref<AudioStreamPlayback> playback = m.player->get_stream_playback();
		m.player->stop();

		AudioServer::get_singleton()->lock();
		if (Bf3AudioPlayback *ours = Object::cast_to<Bf3AudioPlayback>(playback.ptr())) {
			ours->set_mixer(nullptr);
		}
		m.tap.set(nullptr);
		m.tap.pt1_set_capture(nullptr);
		AudioServer::get_singleton()->unlock();
		pt1_capture_end(m);
		if (Node *parent = m.player->get_parent()) {
			parent->remove_child(m.player);
		}
		m.player->queue_free();
		m.player = nullptr;
	}
	if (m.stream.is_valid()) {
		m.stream->set_mixer(nullptr);
		m.stream.unref();
	}
	m.router.reset();
	m.impacts.reset();
	m.clock.reset();
	m.mixer.reset();
	m.pool.reset();
	m.check = nullptr;
	m.built = false;
}

template <class Impl>
ba::MixerA3Assets load_a3(const core::EbxResolver &p_db, Impl &r_m) {
	std::vector<std::shared_ptr<const ca::SoundProgram>> programs;
	if (r_m.table) {
		for (const ba::WeaponAudioDef &d : r_m.table->defs) {
			programs.push_back(d.program);
			programs.push_back(d.explosion);
		}
	}
	if (r_m.impacts_cache) {
		const std::vector<std::shared_ptr<const ca::SoundProgram>> more = r_m.impacts_cache->programs();
		programs.insert(programs.end(), more.begin(), more.end());
	}
	for (const auto &a : r_m.auditions) {
		programs.push_back(a.second.program);
	}
	ba::MixerA3Assets a3 = ba::load_mixer_a3(p_db, programs);
	char line[200];
	std::snprintf(line, sizeof(line), "audio A3: HDR window %s, %zu scopes, %zu of %zu programs scoped", a3.hdr ? "from the data" : "absent (fixed window)",
			a3.scopes ? a3.scopes->scopes.size() : size_t(0), a3.scoped_patches, programs.size());
	r_m.report.push_back(line);
	if (!a3.notes.empty()) {
		r_m.report.push_back(a3.notes);
	}
	return a3;
}

audio::MixerProfile game_audio_profile(const Bf3GameAudio &p_audio) {
	const audio::MixerCore *mixer = p_audio.mixer();
	return mixer != nullptr ? audio::read_mixer_profile(mixer->stats()) : audio::MixerProfile{};
}

std::string game_audio_profile_text(const Bf3GameAudio &p_audio) {
	return p_audio.mixer() != nullptr ? audio::format_mixer_profile(game_audio_profile(p_audio)) : std::string("audio A3: not built");
}

struct Pt1Capture {
	std::filesystem::path dir;
	core::PathGuard guard;
	std::unique_ptr<ba::WavRecorder> wav;
	std::vector<ba::MixerCutEvent> events;
	std::vector<std::pair<uint64_t, double>> hitches;
	std::chrono::steady_clock::time_point last_step{};
	bool have_step = false;
	uint64_t hitch_count = 0, events_lost = 0;
	double hitch_max_ms = 0.0;
};

void pt1_delete_capture(Pt1Capture *p_capture) {
	delete p_capture;
}

namespace {

constexpr double kPt1HitchMs = 50.0;
constexpr int kPt1AudioThreadPriority = 2;
constexpr size_t kPt1MaxEvents = size_t(1) << 20;

void pt1_drain(ba::MixerCore &r_mixer, Pt1Capture &r_c) {
	std::array<ba::MixerCutEvent, 256> batch;
	for (;;) {
		const size_t n = r_mixer.pt1_poll_cuts(batch);
		for (size_t i = 0; i < n; ++i) {
			if (r_c.events.size() < kPt1MaxEvents) {
				r_c.events.push_back(batch[i]);
			} else {
				++r_c.events_lost;
			}
		}
		if (n < batch.size()) {
			return;
		}
	}
}

}

template <class Impl>
void pt1_capture_begin(Impl &r_m) {
	r_m.pt1.reset(new Pt1Capture());

	const std::string prio = user_arg_value("audio-thread-priority");
	if (prio != "keep") {
		pt1_request_audio_thread_priority(prio.empty() ? kPt1AudioThreadPriority : std::atoi(prio.c_str()));
	}
	const std::string dir = user_arg_value("audio-capture");
	if (dir.empty() || r_m.mixer == nullptr) {
		return;
	}
	double seconds = 150.0;
	if (const std::string s = user_arg_value("audio-capture-seconds"); !s.empty()) {
		const double v = std::atof(s.c_str());
		seconds = std::isfinite(v) ? std::clamp(v, 1.0, 1800.0) : seconds;
	}
	r_m.pt1->dir = std::filesystem::path(dir);
	r_m.pt1->guard = r_m.pt1_guard;
	r_m.pt1->wav = std::make_unique<ba::WavRecorder>(ba::kMixRate, seconds);
	r_m.pt1->events.reserve(size_t(1) << 16);
	r_m.mixer->pt1_record_cuts(true);
	r_m.tap.pt1_set_capture(r_m.pt1->wav.get());
}

template <class Impl>
void pt1_capture_step(Impl &r_m) {
	Pt1Capture *c = r_m.pt1.get();
	if (c == nullptr || r_m.mixer == nullptr) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (c->have_step) {
		const double ms = std::chrono::duration<double, std::milli>(now - c->last_step).count();
		if (ms > kPt1HitchMs) {
			++c->hitch_count;
			c->hitch_max_ms = std::max(c->hitch_max_ms, ms);
			if (!c->dir.empty() && c->hitches.size() < kPt1MaxEvents) {
				c->hitches.emplace_back(r_m.mixer->clock(), ms);
			}
		}
	}
	c->last_step = now;
	c->have_step = true;
	if (!c->dir.empty()) {
		pt1_drain(*r_m.mixer, *c);
	}
}

template <class Impl>
void pt1_capture_end(Impl &r_m) {
	Pt1Capture *c = r_m.pt1.get();
	if (c == nullptr || r_m.mixer == nullptr) {
		return;
	}
	const ba::MixerStats &ms = r_m.mixer->stats();
	char line[600];
	const int prio = pt1_audio_thread_priority();
	const int prio_set = pt1_audio_thread_priority_set();
	std::snprintf(line, sizeof(line),
			"; mixer: %llu blocks, %llu late, %llu late commands, %llu starved, %llu decode errors, %llu ring overflows, max block %.3f ms, max call gap %.1f ms, output latency %llu frames, audio thread priority %s%d -> %s%d; main "
			"thread hitches > %.0f ms: %llu (max %.0f ms)",
			(unsigned long long)ms.blocks.load(), (unsigned long long)ms.late_blocks.load(), (unsigned long long)ms.late_commands.load(), (unsigned long long)ms.starved_voices.load(), (unsigned long long)ms.decode_errors.load(),
			(unsigned long long)ms.ring_overflows.load(), double(ms.max_block_ns.load()) / 1e6,
			double(ms.max_call_gap_ns.load()) / 1e6, (unsigned long long)(r_m.clock ? r_m.clock->output_latency_frames() : 0),
			prio == kPt1NoPriority ? "unknown " : "", prio == kPt1NoPriority ? 0 : prio, prio_set == kPt1NoPriority ? "unchanged " : "",
			prio_set == kPt1NoPriority ? 0 : prio_set, kPt1HitchMs, (unsigned long long)c->hitch_count, c->hitch_max_ms);
	UtilityFunctions::print(String::utf8(("[pt1] audio health: " + ba::format_mixer_cuts(r_m.mixer->pt1_cut_stats()) + line).c_str()));
	if (r_m.router) {
		const ba::WeaponAudioStats &rs = r_m.router->stats();
		UtilityFunctions::print(String::utf8(("[pt1] weapon foley: " + std::to_string(rs.reloads) + " reloads, " + std::to_string(rs.foley) + " foley one-shots, " +
				std::to_string(rs.foley_releases) + " releases, " + std::to_string(rs.foley_out_of_reach) + " out of reach, " + std::to_string(rs.one_shots) + " pool one-shots").c_str()));
	}
	if (c->dir.empty()) {
		return;
	}
	pt1_drain(*r_m.mixer, *c);
	const core::PathGuard &guard = c->guard;
	std::error_code ec;
	std::filesystem::create_directories(c->dir, ec);
	std::string result = "[pt1] audio capture " + c->dir.string() + ":";
	if (const core::Status st = c->wav->save(guard, c->dir / "mix.wav"); st.ok()) {
		char part[96];
		std::snprintf(part, sizeof(part), " mix.wav %.1f s (%llu frames past the end dropped)", double(c->wav->samples().size()) / 2.0 / double(ba::kMixRate),
				(unsigned long long)c->wav->dropped_frames());
		result += part;
	} else {
		result += " mix.wav ERROR " + st.error().message;
	}
	const auto write_text = [&](const char *p_name, const std::string &p_text) {
		const std::filesystem::path path = c->dir / p_name;
		if (!guard.allows_write(path)) {
			result += std::string(" ") + p_name + " refused by the path guard";
			return;
		}
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		f << p_text;
		result += std::string(" ") + p_name + (f.good() ? "" : " (write failed)");
	};
	std::string csv = "frame,time_s,kind,handle,step,loudness_db,distance_m,program\n";
	for (const ba::MixerCutEvent &e : c->events) {
		char row[160];
		std::snprintf(row, sizeof(row), "%llu,%.5f,%s,%u,%.6g,%.2f,%.2f,", (unsigned long long)e.frame, double(e.frame) / double(ba::kMixRate),
				ba::mixer_cut_kind_name(e.kind), unsigned(e.handle), double(e.step), double(e.loudness_db), double(e.distance_m));
		csv += row;
		csv += e.program;
		csv += '\n';
	}
	write_text("cuts.csv", csv);
	std::string hitches = "frame,time_s,gap_ms\n";
	for (const auto &h : c->hitches) {
		char row[96];
		std::snprintf(row, sizeof(row), "%llu,%.5f,%.2f\n", (unsigned long long)h.first, double(h.first) / double(ba::kMixRate), h.second);
		hitches += row;
	}
	write_text("hitches.csv", hitches);
	char tail[96];
	std::snprintf(tail, sizeof(tail), "; %zu cuts (%llu lost), %zu hitches", c->events.size(), (unsigned long long)c->events_lost, c->hitches.size());
	result += tail;
	UtilityFunctions::print(String::utf8(result.c_str()));
}

}

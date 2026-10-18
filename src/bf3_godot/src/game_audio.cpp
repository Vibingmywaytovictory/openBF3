// Bf3GameAudio (game_audio.h; as built: docs/design/m2a-au3.md "Bf3GameAudio").

#include "game_audio.h"

#include "audio_stream.h"
#include "godot_util.h"

#include "bf3/audio/audio_load.h"
#include "bf3/audio/game_audio_frame.h"
#include "bf3/audio/kit_audio.h"
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

using namespace godot;
namespace ba = bf3::audio;
namespace ca = bf3::core::audio;
namespace sw = bf3::sim::weapons;

namespace bf3::godot_side {

namespace {

// The mapped frame of an event at "now" is the mixer clock + this (plus the measured output latency, which the heard
// frame takes off again): schedule_lead_blocks blocks (the mixer's earliest frame), 2 blocks for Godot's mix chunks
// running ahead between two display frames, and one 60 Hz display frame of 800 frames (an event a frame old).
constexpr int64_t kBiasMarginFrames = 2 * int64_t(ba::kBlockFrames) + 800;
constexpr uint32_t kCommandCapacity = 8192;

// The game mixer's single tap: forwards to the check while one is attached (swapped under the AudioServer lock).
class TapForwarder final : public ba::MixTap {
public:
	void on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t p_first_frame) override {
		if (ba::MixTap *t = target_.load(std::memory_order_acquire)) {
			t->on_block(p_interleaved_stereo, p_frames, p_first_frame);
		}
	}
	void set(ba::MixTap *p_tap) {
		target_.store(p_tap, std::memory_order_release);
	}

private:
	std::atomic<ba::MixTap *> target_{ nullptr };
};

ba::Vec3 vec(const fx::Vec3 &p_v) {
	return { p_v.x, p_v.y, p_v.z };
}

core::Result<std::shared_ptr<const ca::SoundProgram>> compile_lenient(const core::EbxResolver &p_db, std::string_view p_patch) {
	core::Result<std::shared_ptr<const ca::SoundProgram>> strict = ba::compile_patch(p_db, p_patch);
	if (strict) {
		return strict;
	}
	ca::CompileOptions lenient;
	lenient.allow_unimplemented = true;
	return ba::compile_patch(p_db, p_patch, lenient);
}

} // namespace

struct Bf3GameAudio::Impl {
	// Load (loader thread), read-only afterwards.
	const host::LevelDatabase *db = nullptr;
	std::shared_ptr<const ba::WeaponAudioTable> table;
	ba::KitAudioSources kits;
	std::shared_ptr<const ba::ImpactSoundCache> impacts_cache;
	std::map<std::string, std::shared_ptr<const ca::SoundProgram>, std::less<>> auditions; // compiled patches by name
	std::shared_ptr<const ca::WaveBank> bank;
	std::vector<std::string> report;
	uint32_t programs = 0, program_failures = 0;
	std::atomic<bool> loaded{ false };
	// Build (main thread).
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
	// Per frame.
	std::vector<ba::MuzzlePoint> muzzles;
	std::vector<ba::ImpactSoundRequest> impact_requests;
	ba::GameAudioDriver driver;
	ba::ListenerState listener;
	uint64_t auditions_started = 0;
	int64_t lead_frames = 2 * int64_t(ba::kBlockFrames);
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
	const core::EbxDb &db = p_in.db->level->db();
	const auto stopped = [&]() { return p_in.stop != nullptr && p_in.stop->load(std::memory_order_relaxed); };
	m.db = p_in.db;
	// E7 item 1: the kit items' patches, strict, and their binding.
	m.kits = ba::kit_audio_sources(*p_in.shared->gunplay);
	core::Result<ba::WeaponAudioTable> table = ba::load_weapon_audio(db, m.kits.sources, ba::kMixRate);
	if (!table) {
		return table.error();
	}
	auto shared_table = std::make_shared<ba::WeaponAudioTable>(std::move(*table));
	m.report = ba::kit_audio_report(m.kits, *shared_table);
	m.programs += shared_table->compiled;
	m.program_failures += shared_table->failed;
	m.table = shared_table;
	if (stopped()) {
		return core::make_error("Bf3GameAudio::load: stopped");
	}
	// Impact and effect sounds (fx3's names).
	core::Result<std::shared_ptr<const ba::ImpactSoundCache>> cache = ba::ImpactSoundCache::build(db, p_impact_sounds);
	if (!cache) {
		return cache.error();
	}
	m.impacts_cache = *cache;
	m.programs += uint32_t((*cache)->compiled());
	m.program_failures += uint32_t((*cache)->failed());
	for (const std::string &e : (*cache)->errors()) {
		m.report.push_back("impact " + e);
	}
	// E7 item 4: the audition patches, when the level's mount has them (else a reported skip).
	for (const ba::GameAuditionItem &item : ba::game_audition_items()) {
		core::Result<std::shared_ptr<const ca::SoundProgram>> p = compile_lenient(db, item.patch);
		if (p) {
			m.auditions.emplace(item.patch, *p);
			m.report.push_back("audition " + item.name + " [" + item.patch + "]: compiled");
		} else {
			m.report.push_back("audition " + item.name + " [" + item.patch + "]: skipped (" + p.error().message + ")");
		}
	}
	if (stopped()) {
		return core::make_error("Bf3GameAudio::load: stopped");
	}
	// One bank over every program's waves.
	std::vector<std::shared_ptr<const ca::SoundProgram>> all;
	for (const ba::WeaponAudioDef &d : m.table->defs) {
		if (d.program) {
			all.push_back(d.program);
		}
		if (d.explosion) {
			all.push_back(d.explosion);
		}
	}
	for (const std::shared_ptr<const ca::SoundProgram> &p : m.impacts_cache->programs()) {
		all.push_back(p);
	}
	for (const auto &[name, p] : m.auditions) {
		all.push_back(p);
	}
	size_t missing = 0;
	core::Result<std::vector<ca::SoundWaveInfo>> waves = ba::read_program_waves(db, all, &missing);
	if (!waves) {
		return waves.error();
	}
	ca::WaveBankOptions bo;
	bo.threads = p_in.threads;
	core::Result<std::shared_ptr<const ca::WaveBank>> bank = ca::WaveBank::load(*p_in.db->install, p_in.db->level->mount(), std::move(*waves), bo);
	if (!bank) {
		return bank.error();
	}
	m.bank = *bank;
	char line[160];
	std::snprintf(line, sizeof(line), "bank: %zu programs, %u resident waves, %zu waves not in the level", all.size(), unsigned(m.bank->stats().resident_waves),
			missing);
	m.report.push_back(line);
	m.loaded.store(true, std::memory_order_release);
	return core::Status::success();
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
	m.lead_frames = int64_t(mc.schedule_lead_blocks) * int64_t(ba::kBlockFrames);
	m.mixer = std::make_shared<ba::MixerCore>(mc);
	m.mixer->set_tap(&m.tap); // before the audio thread sees the mixer
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
	// The clock: the measured output latency, the bias over it, the frame's two presentation times.
	const double latency_s = AudioServer::get_singleton()->get_output_latency();
	const uint64_t latency = std::isfinite(latency_s) && latency_s > 0.0 && latency_s < 2.0 ? uint64_t(latency_s * double(ba::kMixRate)) : 0;
	m.clock->set_output_latency_frames(latency);
	m.clock->set_bias_frames(int64_t(latency) + m.lead_frames + kBiasMarginFrames);
	m.clock->set_now(p_frame.clock);
	// The listener is the active camera.
	ba::GameAudioInputs in;
	in.frame = p_frame.frame;
	in.now_ns = p_frame.now_ns;
	in.events = p_frame.events;
	in.others = p_frame.others;
	in.other_weapons = p_frame.other_weapons;
	in.listener.position = vec(p_frame.camera.trans);
	in.listener.forward = vec(p_frame.camera.forward);
	in.listener.up = vec(p_frame.camera.up);
	in.listener.velocity = vec(p_frame.camera_velocity);
	in.own_slot = p_frame.own_slot;
	in.own_first_person = p_frame.own_alive && !p_frame.free_camera;
	m.listener = in.listener;
	// Each other's 3p muzzle when the crowd has one (fx's MuzzleSample: +Y of the effect frame is the barrel).
	m.muzzles.assign(p_frame.others.size(), ba::MuzzlePoint{});
	for (size_t i = 0; i < p_frame.others.size(); ++i) {
		for (const fx::MuzzleSample &s : p_frame.muzzles) {
			if (s.valid && !s.first_person && s.slot == p_frame.others[i].slot) {
				m.muzzles[i].valid = true;
				m.muzzles[i].position = vec(s.transform.trans);
				m.muzzles[i].forward = vec(s.transform.up);
				break;
			}
		}
	}
	in.muzzles = m.muzzles;
	m.impact_requests.clear();
	for (const fx::ImpactSoundCue &c : p_impact_cues) {
		ba::ImpactSoundRequest r;
		r.patch = c.patch;
		r.position = vec(c.position);
		r.normal = vec(c.normal);
		r.seed = c.seed;
		r.time = c.time;
		r.timeline = c.timeline;
		m.impact_requests.push_back(r);
	}
	m.driver.step(*m.router, m.impacts.get(), *m.clock, in, m.impact_requests, p_effect_sounds);
}

bool Bf3GameAudio::audition(std::string_view p_patch, const fx::Vec3 &p_position, std::string *r_error) {
	Impl &m = *impl_;
	const auto fail = [&](const std::string &p_why) {
		if (r_error != nullptr) {
			*r_error = p_why;
		}
		return false;
	};
	if (!m.built) {
		return fail("Bf3GameAudio::audition: not built");
	}
	std::shared_ptr<const ca::SoundProgram> program;
	if (auto it = m.auditions.find(p_patch); it != m.auditions.end()) {
		program = it->second;
	} else if (m.impacts_cache && (program = m.impacts_cache->find(p_patch))) {
	} else {
		if (m.db == nullptr || !m.db->level) {
			return fail("Bf3GameAudio::audition: no level database");
		}
		core::Result<std::shared_ptr<const ca::SoundProgram>> p = compile_lenient(m.db->level->db(), p_patch);
		if (!p) {
			++m.program_failures;
			return fail(std::string(p_patch) + ": " + p.error().message);
		}
		program = *p;
		m.auditions.emplace(std::string(p_patch), program); // its waves are not in the bank unless loaded: starved, counted
	}
	ba::EmitterState em;
	em.position = vec(p_position);
	const ba::Vec3 d{ m.listener.position.x - em.position.x, m.listener.position.y - em.position.y, m.listener.position.z - em.position.z };
	const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (len > 0.0f) {
		em.forward = { d.x / len, d.y / len, d.z / len }; // facing the listener (on the cone axis)
	}
	ba::OneShotOptions o;
	o.set_first_person = true;
	o.first_person = false;
	const uint64_t seed = ba::sound_seed(0xA0D1u, uint32_t(m.auditions_started));
	const uint64_t frame = m.clock->earliest_frame();
	ba::GameAudioCheck *check = m.check;
	if (check != nullptr && check->current_step() >= 0) {
		const uint32_t step = uint32_t(check->current_step());
		o.before_start = [check, step, seed, frame](ba::PatchInstance &r_instance, ba::InstanceHandle) { check->on_audition(step, seed, frame, r_instance); };
	}
	if (!m.pool->start(*m.mixer, program, m.bank, m.tuning, seed, em, m.listener, frame, o)) {
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
	// Under the AudioServer lock the audio thread is outside _mix: the tap target swaps between two blocks.
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
		// Under the AudioServer lock Godot's audio thread is outside _mix: the playback drops its mixer reference, so
		// the mixer (and the instances it owns) goes on the main thread, and the tap is ours again.
		AudioServer::get_singleton()->lock();
		if (Bf3AudioPlayback *ours = Object::cast_to<Bf3AudioPlayback>(playback.ptr())) {
			ours->set_mixer(nullptr);
		}
		m.tap.set(nullptr);
		AudioServer::get_singleton()->unlock();
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
	m.router.reset(); // Kill commands for the live drivers (the mixer still exists)
	m.impacts.reset();
	m.clock.reset();
	m.mixer.reset();
	m.pool.reset();
	m.check = nullptr;
	m.built = false;
}

} // namespace bf3::godot_side

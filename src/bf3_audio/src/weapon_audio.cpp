// Weapons in the game's mixer (weapon_audio.h; docs/design/m2a.md §12.3, §8.3, §4.4; as built: docs/design/m2a-au3.md).
// Game thread only. Every PatchInstance is built here (allocation on the game thread) and handed to the mixer by a
// Start command; the mixer gives finished ones back through its feedback and return rings.

#include "bf3/audio/weapon_audio.h"

#include "bf3/audio/audio_load.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace bf3::audio {

namespace ca = core::audio;
namespace sw = sim::weapons;

namespace {

constexpr uint32_t kStopCommandDelayFrames = 2 * kBlockFrames; // the Stop command after the stop event (au's check)

float distance(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a.x - p_b.x, dy = p_a.y - p_b.y, dz = p_a.z - p_b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool finite3(const float *p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
}

bool has_event(const ca::SoundProgram &p_program, uint32_t p_hash) {
	for (const ca::SoundInput &in : p_program.inputs) {
		if (in.event && in.name_hash == p_hash) {
			return true;
		}
	}
	return false;
}

} // namespace

uint32_t one_shot_event_of(const ca::SoundProgram &p_program) {
	if (p_program.start_event != 0) {
		return p_program.start_event;
	}
	if (has_event(p_program, kHashFire)) {
		return kHashFire;
	}
	if (has_event(p_program, kHashMelee)) {
		return kHashMelee;
	}
	return 0;
}

namespace {

bool same_emitter(const EmitterState &p_a, const EmitterState &p_b) {
	const auto eq = [](const Vec3 &a, const Vec3 &b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
	return eq(p_a.position, p_b.position) && eq(p_a.velocity, p_b.velocity) && eq(p_a.forward, p_b.forward);
}

MixerCommand emitter_command(InstanceHandle p_handle, const EmitterState &p_emitter) {
	MixerCommand c;
	c.type = MixerCommandType::SetEmitter;
	c.handle = p_handle;
	c.emitter = p_emitter;
	return c;
}

} // namespace

// ---- SimMixerClock ------------------------------------------------------------------------------------------------

SimMixerClock::SimMixerClock(const MixerCore &p_mixer, const SimMixerClockConfig &p_config) :
		mixer_(&p_mixer), config_(p_config) {
	if (config_.rate == 0) {
		config_.rate = kMixRate;
	}
}

void SimMixerClock::set_output_latency_frames(uint64_t p_frames) {
	latency_frames_ = p_frames;
}

void SimMixerClock::set_now(const host::PresentClock &p_now) {
	now_ = p_now;
}

uint64_t SimMixerClock::heard_frame() const {
	const uint64_t c = mixer_->clock();
	return c > latency_frames_ ? c - latency_frames_ : 0;
}

uint64_t SimMixerClock::frame_of(sw::SimTime p_time, host::PresentTimeline p_timeline) {
	const uint64_t earliest = mixer_->earliest_frame();
	if (p_timeline == host::PresentTimeline::Immediate) {
		return earliest;
	}
	if (!now_.valid(p_timeline)) {
		++late_;
		return earliest;
	}
	// (t - now) in frames, rounded. In doubles: a hostile time far from now must not overflow the difference.
	const double delta = (double(p_time) - double(now_.now(p_timeline))) * double(config_.rate) / double(sw::kTimeUnitsPerSecond);
	if (!std::isfinite(delta)) {
		++late_;
		return earliest;
	}
	const double f = double(heard_frame()) + std::round(delta) + double(config_.bias_frames);
	if (!(f >= double(earliest))) {
		++late_;
		const double behind = double(earliest) - f;
		const uint64_t b = behind >= 1.8e19 ? std::numeric_limits<uint64_t>::max() : uint64_t(behind);
		max_late_frames_ = std::max(max_late_frames_, b);
		return earliest;
	}
	if (f >= 1.8e19) {
		return std::numeric_limits<uint64_t>::max() / 2; // hostile: a time far in the future (never reached)
	}
	return uint64_t(f);
}

uint64_t SimMixerClock::late() const {
	return late_;
}

uint64_t SimMixerClock::max_late_frames() const {
	return max_late_frames_;
}

void SimMixerClock::set_bias_frames(int64_t p_bias) {
	config_.bias_frames = p_bias;
}

int64_t SimMixerClock::bias_frames() const {
	return config_.bias_frames;
}

uint64_t SimMixerClock::output_latency_frames() const {
	return latency_frames_;
}

const host::PresentClock &SimMixerClock::now() const {
	return now_;
}

uint64_t SimMixerClock::earliest_frame() const {
	return mixer_->earliest_frame();
}

uint32_t SimMixerClock::rate() const {
	return config_.rate;
}

// ---- load_weapon_audio --------------------------------------------------------------------------------------------

core::Result<WeaponAudioTable> load_weapon_audio(const core::EbxResolver &p_db, std::span<const WeaponSoundSource> p_defs, uint32_t p_mix_rate) {
	if (p_mix_rate == 0) {
		return core::make_error("load_weapon_audio: mix rate 0");
	}
	WeaponAudioTable table;
	table.defs.resize(p_defs.size());
	// One compile per partition (the two teams share the knife, the M67 and every patch named twice).
	std::unordered_map<std::string, core::Result<std::shared_ptr<const ca::SoundProgram>>> compiled;
	const auto compile = [&](const std::string &p_name) -> const core::Result<std::shared_ptr<const ca::SoundProgram>> & {
		auto it = compiled.find(p_name);
		if (it == compiled.end()) {
			it = compiled.emplace(p_name, compile_patch(p_db, p_name)).first;
		}
		return it->second;
	};
	for (size_t i = 0; i < p_defs.size(); ++i) {
		const WeaponSoundSource &src = p_defs[i];
		WeaponAudioDef &d = table.defs[i];
		d.patch = src.sound;
		d.fire_mode = src.fire_mode;
		d.shot_interval_s = src.shot_interval_s;
		if (src.sound.empty()) {
			d.error = "no sound partition";
			continue;
		}
		const core::Result<std::shared_ptr<const ca::SoundProgram>> &prog = compile(src.sound);
		if (!prog) {
			d.error = "compile: " + prog.error().message;
			++table.failed;
			continue;
		}
		d.program = *prog;
		d.has_reload = has_event(*d.program, kHashReload);
		d.foley = read_weapon_foley(*d.program); // PT1 rld [foley]
		if (!src.explosion_sound.empty()) {
			const core::Result<std::shared_ptr<const ca::SoundProgram>> &ex = compile(src.explosion_sound);
			if (ex) {
				d.explosion = *ex;
			} else {
				d.error = "explosion compile: " + ex.error().message; // the form stays; the detonation is silent
			}
		}
		if (src.one_shot) {
			d.one_shot_event = one_shot_event_of(*d.program);
			if (d.one_shot_event == 0) {
				d.error = "one-shot patch has no start, Fire or Melee event";
				d.program.reset();
				++table.failed;
				continue;
			}
			d.form = WeaponAudioForm::OneShot;
			++table.compiled;
			continue;
		}
		core::Result<WeaponSoundBinding> b = bind_weapon_sound(d.program, src.shot_interval_s, p_mix_rate);
		if (!b) {
			d.error = "bind: " + b.error().message;
			d.program.reset();
			++table.failed;
			continue;
		}
		d.binding = std::move(*b);
		d.form = WeaponAudioForm::Driver;
		++table.compiled;
	}
	return table;
}

// ---- OneShotPool --------------------------------------------------------------------------------------------------

OneShotPool::OneShotPool(uint32_t p_limit) :
		limit_(std::max<uint32_t>(p_limit, 1)) {
	live_.reserve(limit_);
}

bool OneShotPool::start(MixerCore &r_mixer, std::shared_ptr<const ca::SoundProgram> p_program, std::shared_ptr<const ca::WaveBank> p_bank,
		const AudioTuning &p_tuning, uint64_t p_seed, const EmitterState &p_emitter, const ListenerState &p_listener, uint64_t p_frame) {
	return start(r_mixer, std::move(p_program), std::move(p_bank), p_tuning, p_seed, p_emitter, p_listener, p_frame, OneShotOptions{});
}

bool OneShotPool::start(MixerCore &r_mixer, std::shared_ptr<const ca::SoundProgram> p_program, std::shared_ptr<const ca::WaveBank> p_bank,
		const AudioTuning &p_tuning, uint64_t p_seed, const EmitterState &p_emitter, const ListenerState &p_listener, uint64_t p_frame,
		const OneShotOptions &p_options) {
	if (!p_program) {
		++failed_;
		return false;
	}
	if (live_.size() >= limit_) {
		// The oldest is stolen (Kill: no fade, it is the least audible by age).
		auto oldest = std::min_element(live_.begin(), live_.end(), [](const Entry &a, const Entry &b) { return a.start_frame < b.start_frame; });
		MixerCommand kill;
		kill.type = MixerCommandType::Kill;
		kill.handle = oldest->handle;
		if (!r_mixer.push(kill)) {
			++failed_;
			return false;
		}
		live_.erase(oldest);
		++stolen_;
	}
	core::Result<std::unique_ptr<PatchInstance>> inst = PatchInstance::create(p_program, std::move(p_bank), p_tuning, p_seed, p_options.mix_rate);
	if (!inst) {
		++failed_;
		return false;
	}
	PatchInstance &pi = **inst;
	if (p_options.set_first_person) {
		pi.set_parameter(kHashIsFirstPerson, p_options.first_person ? 1.0f : 0.0f);
	}
	pi.set_geometry(p_emitter, p_listener);
	const InstanceHandle handle = r_mixer.next_handle();
	if (p_options.before_start) {
		p_options.before_start(pi, handle);
	}
	MixerCommand start;
	start.type = MixerCommandType::Start;
	start.handle = handle;
	start.at_frame = p_frame;
	start.instance = &pi;
	if (!r_mixer.push(start)) {
		++failed_;
		return false; // the instance stays ours and is destroyed here
	}
	(void)inst->release(); // the mixer owns it now
	const uint32_t event = p_options.event != 0 ? p_options.event : p_program->start_event;
	if (event != 0) {
		MixerCommand ev;
		ev.type = MixerCommandType::PostEvent;
		ev.handle = handle;
		ev.name_hash = event;
		ev.at_frame = p_frame;
		if (!r_mixer.push(ev)) {
			++failed_; // started but silent; update() stops and forgets it
		}
	}
	Entry e;
	e.handle = handle;
	e.start_frame = p_frame;
	e.emitter = p_emitter;
	live_.push_back(e);
	peak_ = std::max<uint32_t>(peak_, uint32_t(live_.size()));
	++started_;
	last_ = handle;
	return true;
}

void OneShotPool::on_finished(InstanceHandle p_handle) {
	for (size_t i = 0; i < live_.size(); ++i) {
		if (live_[i].handle == p_handle) {
			live_.erase(live_.begin() + std::ptrdiff_t(i));
			return;
		}
	}
}

uint32_t OneShotPool::live() const {
	return uint32_t(live_.size());
}

uint32_t OneShotPool::peak() const {
	return peak_;
}

uint64_t OneShotPool::started() const {
	return started_;
}

uint64_t OneShotPool::stolen() const {
	return stolen_;
}

uint64_t OneShotPool::failed() const {
	return failed_;
}

InstanceHandle OneShotPool::last_handle() const {
	return last_;
}

bool OneShotPool::contains(InstanceHandle p_handle) const {
	return std::any_of(live_.begin(), live_.end(), [&](const Entry &e) { return e.handle == p_handle; });
}

uint32_t OneShotPool::resend_emitters(MixerCore &r_mixer) const {
	uint32_t n = 0;
	for (const Entry &e : live_) {
		if (r_mixer.push(emitter_command(e.handle, e.emitter))) {
			++n;
		}
	}
	return n;
}

void OneShotPool::update(MixerCore &r_mixer, uint32_t p_rate) {
	const uint64_t clock = r_mixer.clock();
	const uint64_t stop_after = uint64_t(kOneShotStopAfterSeconds * double(p_rate));
	const uint64_t forget_after = uint64_t(kOneShotForgetSeconds * double(p_rate));
	for (size_t i = 0; i < live_.size();) {
		Entry &e = live_[i];
		if (clock > e.start_frame + forget_after) {
			MixerCommand kill;
			kill.type = MixerCommandType::Kill;
			kill.handle = e.handle;
			if (r_mixer.push(kill)) {
				live_.erase(live_.begin() + std::ptrdiff_t(i));
				++forgotten_;
				continue;
			}
		} else if (!e.stopped && clock > e.start_frame + stop_after) {
			MixerCommand stop;
			stop.type = MixerCommandType::Stop;
			stop.handle = e.handle;
			e.stopped = r_mixer.push(stop);
		}
		++i;
	}
}

bool OneShotPool::stop(MixerCore &r_mixer, InstanceHandle p_handle) {
	for (Entry &e : live_) {
		if (e.handle == p_handle) {
			if (e.stopped) {
				return true;
			}
			MixerCommand stop;
			stop.type = MixerCommandType::Stop;
			stop.handle = p_handle;
			e.stopped = r_mixer.push(stop);
			return e.stopped;
		}
	}
	return false;
}

uint64_t OneShotPool::forgotten() const {
	return forgotten_;
}

// ---- WeaponAudioRouter --------------------------------------------------------------------------------------------

struct WeaponAudioRouter::Impl {
	MixerCore &mixer;
	std::shared_ptr<const WeaponAudioTable> table;
	std::shared_ptr<const ca::WaveBank> bank;
	AudioTuning tuning;
	OneShotPool &pool;
	WeaponAudioConfig config;
	WeaponAudioStats stats;
	WeaponAudioObserver *observer = nullptr;
	uint8_t own_slot = 0xFF;
	bool own_first_person = true;
	bool own_trigger_known = false;
	bool own_trigger_down = false;
	ListenerState listener;
	bool listener_set = false;
	uint32_t own_burst_counter = 0;
	uint64_t frame_counter = 0;
	// set_others_newest: the server time up to which RemoteShots and records are in (valid once set this frame).
	sw::SimTime horizon = 0;
	bool horizon_valid = false;

	// One driver instance in the mixer: a burst while held, its tail after the trigger up.
	enum class OpKind : uint8_t { Down, Round, Up, Stop };
	struct Op {
		OpKind kind = OpKind::Down;
		uint32_t round = 0;
		uint64_t frame = 0;
	};
	struct Voice {
		InstanceHandle handle = kNoInstance;
		uint8_t slot = 0;
		bool own = false;
		uint16_t def = sw::kNoWeapon;
		uint64_t seed = 0;
		double interval_s = 0.075; // the binding's shot interval
		uint32_t rate = kMixRate; // the binding's mix rate
		WeaponSoundDriver driver;
		bool held = true;
		bool single = false;
		host::PresentTimeline timeline = host::PresentTimeline::Render;
		uint64_t first_frame = 0;
		sw::SimTime first_time = 0;
		bool have_id = false;
		uint32_t first_id = 0;
		uint32_t next_round = 1;
		uint32_t last_round = 0;
		sw::SimTime last_round_time = 0; // the newest round's time (event or synthesised)
		sw::SimTime last_evidence = 0; // others: the newest firing sample, burst_seq change or RemoteShot time
		sw::SimTime last_event_time = std::numeric_limits<sw::SimTime>::min(); // others: the newest RemoteShot
		uint64_t stop_frame = 0;
		uint64_t release_clock = 0;
		EmitterState emitter;
		bool emitter_dirty = false;
		float distance = 0.0f;
		std::vector<Op> ops; // not yet accepted by the command ring, in order
		Voice(const WeaponSoundBinding &p_binding, WeaponFireMode p_mode, InstanceHandle p_handle) :
				handle(p_handle),
				interval_s(p_binding.shot_interval_seconds > 0.0 ? p_binding.shot_interval_seconds : 0.075),
				rate(p_binding.mix_rate != 0 ? p_binding.mix_rate : kMixRate),
				driver(p_binding, p_mode, p_handle) {}
		// WeaponSoundDriver::round_frame without needing the trigger down flushed yet.
		uint64_t round_frame(uint32_t p_round) const {
			return first_frame + uint64_t(std::llround(double(p_round) * interval_s * double(rate)));
		}
	};
	std::vector<Voice> voices; // <= config.max_weapon_voices

	struct Track {
		InstanceHandle held = kNoInstance; // the slot's running burst
		bool seen = false;
		bool weapon_seen = false;
		uint8_t generation = 0;
		uint8_t burst_seq = 0;
		uint8_t reload_seq = 0;
		uint32_t last_firing_tick = 0;
		uint16_t def = sw::kNoWeapon;
		uint64_t present_frame = 0;
		EmitterState emitter;
		float distance = 0.0f;
		sw::SimTime last_event_time = std::numeric_limits<sw::SimTime>::min();
		// Bursts started from RemoteShots since the newest record: the guess of the burst_seq sv gave them.
		uint8_t seq_ahead = 0;
		// The newest evidence of the slot's last ended burst: an older firing record never restarts it.
		sw::SimTime ended_evidence = std::numeric_limits<sw::SimTime>::min();
		// The newest record's firing sample (set_others_newest): a shot within two ticks before it.
		sw::SimTime newest_fire = std::numeric_limits<sw::SimTime>::min();
	};
	std::array<Track, 256> tracks{};
	Track own;
	SimMixerClock *clock = nullptr;

	Impl(MixerCore &r_mixer, std::shared_ptr<const WeaponAudioTable> p_table, std::shared_ptr<const ca::WaveBank> p_bank, const AudioTuning &p_tuning,
			OneShotPool &r_pool, const WeaponAudioConfig &p_config) :
			mixer(r_mixer), table(std::move(p_table)), bank(std::move(p_bank)), tuning(p_tuning), pool(r_pool), config(p_config) {
		voices.reserve(std::max<uint32_t>(config.max_weapon_voices, 1));
	}

	const WeaponAudioDef *def_of(uint16_t p_def) const {
		if (!table || p_def >= table->defs.size()) {
			return nullptr;
		}
		return &table->defs[p_def];
	}

	Voice *voice(InstanceHandle p_handle) {
		for (Voice &v : voices) {
			if (v.handle == p_handle) {
				return &v;
			}
		}
		return nullptr;
	}

	sw::SimTime interval_units(const Voice &p_v) const {
		return sw::SimTime(std::llround(p_v.interval_s * double(sw::kTimeUnitsPerSecond)));
	}

	// Pushes the voice's queued ops in order; stops at the first the ring refuses (retried in update()).
	void flush(Voice &r_v) {
		size_t done = 0;
		for (; done < r_v.ops.size(); ++done) {
			const Op &op = r_v.ops[done];
			bool ok = true;
			switch (op.kind) {
				case OpKind::Down:
					ok = r_v.driver.trigger_down(mixer, op.frame);
					break;
				case OpKind::Round:
					ok = r_v.driver.round_fired(mixer, op.round);
					break;
				case OpKind::Up:
					ok = r_v.driver.trigger_up(mixer, op.frame);
					break;
				case OpKind::Stop: {
					MixerCommand c;
					c.type = MixerCommandType::Stop;
					c.handle = r_v.handle;
					c.at_frame = op.frame;
					ok = mixer.push(c);
					break;
				}
			}
			if (!ok) {
				++stats.retried_commands;
				break;
			}
		}
		r_v.ops.erase(r_v.ops.begin(), r_v.ops.begin() + std::ptrdiff_t(done));
	}

	void kill(InstanceHandle p_handle) {
		MixerCommand c;
		c.type = MixerCommandType::Kill;
		c.handle = p_handle;
		(void)mixer.push(c); // a refused Kill leaves the instance to finish on its own
	}

	void erase_voice(InstanceHandle p_handle) {
		for (size_t i = 0; i < voices.size(); ++i) {
			if (voices[i].handle == p_handle) {
				voices.erase(voices.begin() + std::ptrdiff_t(i));
				break;
			}
		}
		for (Track &t : tracks) {
			if (t.held == p_handle) {
				t.held = kNoInstance;
			}
		}
		if (own.held == p_handle) {
			own.held = kNoInstance;
		}
	}

	// A voice for a new burst: under the limit, or by killing the farthest released (tail) one. False: culled.
	bool make_room(float p_distance) {
		if (voices.size() < config.max_weapon_voices) {
			return true;
		}
		Voice *far = nullptr;
		for (Voice &v : voices) {
			if (!v.held && (far == nullptr || v.distance > far->distance)) {
				far = &v;
			}
		}
		(void)p_distance;
		if (far == nullptr) {
			return false; // every voice is a running burst: the new one waits for a trigger up (nearest first applies there)
		}
		const InstanceHandle h = far->handle;
		kill(h);
		erase_voice(h);
		++stats.stolen;
		return true;
	}

	// Starts a burst: one PatchInstance (seed, IsFirstPerson, EnableStep, emitter), Start + trigger down at p_frame.
	Voice *start_burst(Track &r_track, bool p_own, uint8_t p_slot, uint16_t p_def, uint64_t p_seed, uint64_t p_frame, sw::SimTime p_time,
			host::PresentTimeline p_timeline, const EmitterState &p_emitter, float p_distance, bool p_first_person) {
		const WeaponAudioDef *d = def_of(p_def);
		if (d == nullptr || d->form != WeaponAudioForm::Driver || !d->program) {
			++stats.unbound_defs;
			return nullptr;
		}
		if (!(p_distance <= config.max_weapon_distance_m) || !make_room(p_distance)) {
			++stats.culled;
			return nullptr;
		}
		core::Result<std::unique_ptr<PatchInstance>> inst = PatchInstance::create(d->program, bank, tuning, p_seed, d->binding.mix_rate);
		if (!inst) {
			++stats.mixer_refusals; // counted with the refusals: the burst cannot sound
			return nullptr;
		}
		PatchInstance &pi = **inst;
		const InstanceHandle handle = mixer.next_handle();
		voices.emplace_back(d->binding, d->fire_mode, handle);
		Voice &v = voices.back();
		v.slot = p_slot;
		v.own = p_own;
		v.def = p_def;
		v.seed = p_seed;
		v.single = d->fire_mode == WeaponFireMode::Single;
		v.timeline = p_timeline;
		v.first_frame = p_frame;
		v.first_time = p_time;
		v.last_round_time = p_time;
		v.last_evidence = p_time;
		v.emitter = p_emitter;
		v.distance = p_distance;
		v.driver.prepare(pi, p_first_person);
		pi.set_geometry(p_emitter, listener);
		if (observer != nullptr) {
			observer->on_driver_instance(p_slot, p_seed, p_frame, pi);
		}
		MixerCommand start;
		start.type = MixerCommandType::Start;
		start.handle = handle;
		start.at_frame = p_frame;
		start.instance = &pi;
		if (!mixer.push(start)) {
			++stats.mixer_refusals;
			voices.pop_back();
			return nullptr; // the instance is destroyed with `inst`
		}
		(void)inst->release();
		v.ops.push_back(Op{ OpKind::Down, 0, p_frame });
		flush(v);
		r_track.held = handle;
		++stats.bursts;
		if (p_own) {
			++stats.own_bursts;
		}
		stats.peak_weapon_voices = std::max<uint32_t>(stats.peak_weapon_voices, uint32_t(voices.size()));
		return &v;
	}

	void post_round(Voice &r_v, uint32_t p_round, sw::SimTime p_time, bool p_from_event) {
		if (p_round < r_v.next_round || r_v.single || !r_v.held) {
			return;
		}
		// Shot indices are contiguous per soldier: a gap before an event's round is shots whose datagrams were lost.
		// They were fired, so they sound on their grid frames (counted as state rounds); a long gap is not filled.
		if (p_from_event && p_round - r_v.next_round <= 64) {
			for (uint32_t k = r_v.next_round; k < p_round; ++k) {
				r_v.ops.push_back(Op{ OpKind::Round, k, 0 });
				++stats.rounds;
				++stats.state_rounds;
			}
		}
		r_v.next_round = p_round + 1;
		r_v.last_round = p_round;
		r_v.last_round_time = std::max(r_v.last_round_time, p_time);
		r_v.ops.push_back(Op{ OpKind::Round, p_round, 0 });
		flush(r_v);
		++stats.rounds;
		if (p_from_event) {
			++stats.event_rounds;
		} else {
			++stats.state_rounds;
		}
	}

	// Ends a held burst: the stop event half an interval after the last round (not before earliest_frame, and after
	// the last round's frame), then the Stop command.
	void trigger_up(Voice &r_v) {
		if (!r_v.held) {
			return;
		}
		r_v.held = false;
		const uint64_t last = r_v.round_frame(r_v.last_round);
		uint64_t stop = r_v.single ? last + 1 : last + uint64_t(std::llround(0.5 * r_v.interval_s * double(config.mix_rate)));
		stop = std::max(stop, mixer.earliest_frame());
		r_v.stop_frame = stop;
		r_v.release_clock = mixer.clock();
		r_v.ops.push_back(Op{ OpKind::Up, 0, stop });
		r_v.ops.push_back(Op{ OpKind::Stop, 0, stop + kStopCommandDelayFrames });
		flush(r_v);
		++stats.trigger_ups;
		if (observer != nullptr) {
			observer->on_trigger_up(r_v.slot, r_v.seed, stop);
		}
	}

	void end_track(Track &r_track) {
		if (r_track.held != kNoInstance) {
			if (Voice *v = voice(r_track.held)) {
				trigger_up(*v);
			}
			r_track.held = kNoInstance;
		}
	}

	EmitterState own_emitter(const host::PresentEvent &p_e) const {
		EmitterState e;
		if (own_first_person) {
			e.position = listener.position;
			e.forward = listener.forward;
		} else if (finite3(p_e.pos)) {
			e.position = { p_e.pos[0], p_e.pos[1], p_e.pos[2] };
			if (finite3(p_e.dir)) {
				e.forward = { p_e.dir[0], p_e.dir[1], p_e.dir[2] };
			}
		}
		return e;
	}

	EmitterState event_emitter(const host::PresentEvent &p_e) const {
		EmitterState e;
		if (finite3(p_e.pos)) {
			e.position = { p_e.pos[0], p_e.pos[1], p_e.pos[2] };
		}
		if (finite3(p_e.dir)) {
			e.forward = { p_e.dir[0], p_e.dir[1], p_e.dir[2] };
		}
		return e;
	}

	// A one-shot of a def's program (knife swing, M67 throw, reloads, the explosion).
	bool one_shot(const std::shared_ptr<const ca::SoundProgram> &p_program, uint32_t p_event, uint64_t p_seed, const EmitterState &p_emitter,
			uint64_t p_frame, bool p_first_person) {
		if (!p_program) {
			return false;
		}
		OneShotOptions o;
		o.event = p_event;
		o.set_first_person = true;
		o.first_person = p_first_person;
		o.mix_rate = config.mix_rate;
		if (!pool.start(mixer, p_program, bank, tuning, p_seed, p_emitter, listener, p_frame, o)) {
			return false;
		}
		++stats.one_shots;
		return true;
	}

	void own_shot(const host::PresentEvent &p_e, SimMixerClock &r_clock) {
		const WeaponAudioDef *d = def_of(p_e.def);
		if (d == nullptr || d->form == WeaponAudioForm::None) {
			++stats.unbound_defs;
			return;
		}
		const EmitterState em = own_emitter(p_e);
		const uint64_t frame = r_clock.frame_of(p_e.time, host::PresentTimeline::Predicted);
		if (d->form == WeaponAudioForm::OneShot) {
			end_track(own);
			(void)one_shot(d->program, d->one_shot_event, sound_seed(own_slot, own_burst_counter++), em, frame, own_first_person);
			return;
		}
		Voice *v = own.held != kNoInstance ? voice(own.held) : nullptr;
		if (v != nullptr) {
			const sw::SimTime gap = p_e.time - v->last_round_time;
			const double hold = config.own_hold_intervals * v->interval_s * double(sw::kTimeUnitsPerSecond);
			if (v->def != p_e.def || !v->have_id || p_e.id < v->first_id || double(gap) > hold || v->single) {
				trigger_up(*v);
				own.held = kNoInstance;
				v = nullptr;
			}
		}
		if (v == nullptr) {
			v = start_burst(own, true, own_slot, p_e.def, sound_seed(own_slot, own_burst_counter++), frame, p_e.time, host::PresentTimeline::Predicted, em,
					0.0f, own_first_person);
			if (v == nullptr) {
				return;
			}
			v->have_id = true;
			v->first_id = p_e.id;
			++stats.event_rounds;
			if (v->single) {
				trigger_up(*v);
				own.held = kNoInstance;
			}
			return;
		}
		post_round(*v, p_e.id - v->first_id, p_e.time, true);
	}

	sw::SimTime other_hold_units(const Voice &p_v) const {
		return interval_units(p_v) + sw::SimTime(std::llround(config.other_hold_ticks * double(sw::kTimeUnitsPerTick)));
	}

	// A RemoteShot arrives about the render delay before the render clock reaches its time, so it starts the burst on
	// its exact frame (in the future); the records only extend it (and start one when every RemoteShot of it was
	// lost). The seed is sound_seed(slot, burst_seq) with sv's burst_seq for the burst: the newest record's plus one
	// (sv counts a burst when a tick has shots after one without), plus the bursts started since that record.
	void remote_shot(const host::PresentEvent &p_e, SimMixerClock &r_clock) {
		if (p_e.slot == own_slot) {
			return; // cl emits OwnShot for the own soldier
		}
		Track &t = tracks[p_e.slot];
		const WeaponAudioDef *d = def_of(p_e.def);
		if (d == nullptr || d->form == WeaponAudioForm::None) {
			++stats.unbound_defs;
			return;
		}
		EmitterState em = event_emitter(p_e);
		if (t.seen && t.present_frame + 2 >= frame_counter) {
			em = t.emitter; // the presented muzzle (the shot origin is the server's eye position)
		}
		const float dist = distance(em.position, listener.position);
		t.last_event_time = std::max(t.last_event_time, p_e.time);
		const uint64_t frame = r_clock.frame_of(p_e.time, host::PresentTimeline::Render);
		if (d->form == WeaponAudioForm::OneShot) {
			if (dist <= config.max_weapon_distance_m) {
				(void)one_shot(d->program, d->one_shot_event, sound_seed(p_e.slot, p_e.id), em, frame, false);
			}
			return;
		}
		Voice *v = t.held != kNoInstance ? voice(t.held) : nullptr;
		if (v != nullptr && v->def == p_e.def && !v->single && v->have_id && p_e.id > v->first_id) {
			// The same burst when the shot sits on its round grid and follows its last round within the hold.
			const sw::SimTime iu = interval_units(*v);
			const uint32_t k = p_e.id - v->first_id;
			const sw::SimTime on_grid = v->first_time + sw::SimTime(k) * iu;
			const bool grid = iu > 0 && std::llabs(p_e.time - on_grid) <= iu / 2;
			sw::SimTime last = std::max(v->last_round_time, v->last_evidence);
			if (t.newest_fire > v->first_time) {
				last = std::max(last, t.newest_fire); // a lost RemoteShot between: the newest records saw it fire
			}
			const bool close = p_e.time - last <= other_hold_units(*v);
			if (grid && close && k < 100000) {
				v->last_evidence = std::max(v->last_evidence, p_e.time);
				v->last_event_time = std::max(v->last_event_time, p_e.time);
				post_round(*v, k, p_e.time, true);
				return;
			}
		}
		if (v != nullptr) {
			if (v->def == p_e.def && v->have_id && p_e.id <= v->first_id + v->last_round) {
				return; // a duplicate or a reordered old shot of the running burst
			}
			trigger_up(*v);
			t.ended_evidence = std::max(v->last_evidence, v->last_event_time);
			t.held = kNoInstance;
		}
		const uint8_t seq = uint8_t((t.burst_seq + 1u + t.seq_ahead) & 0x0F);
		v = start_burst(t, false, p_e.slot, p_e.def, sound_seed(p_e.slot, seq), frame, p_e.time, host::PresentTimeline::Render, em, dist, false);
		if (v == nullptr) {
			return;
		}
		t.seq_ahead = uint8_t(std::min<uint32_t>(t.seq_ahead + 1u, 15u));
		v->have_id = true;
		v->first_id = p_e.id;
		v->last_event_time = p_e.time;
		++stats.event_rounds;
		if (v->single) {
			trigger_up(*v);
			t.held = kNoInstance;
			// The records show firing up to two ticks after the shot: those samples are this shot, not a lost one.
			t.ended_evidence = std::max(t.ended_evidence, p_e.time + 2 * sw::kTimeUnitsPerTick);
		}
	}

	void on_events(std::span<const host::PresentEvent> p_events, SimMixerClock &r_clock) {
		clock = &r_clock;
		for (const host::PresentEvent &e : p_events) {
			switch (e.kind) {
				case host::PresentKind::OwnShot:
					own_shot(e, r_clock);
					break;
				case host::PresentKind::OwnTimed:
					// PT1 rld [own timed]: replaces the Reload-only one-shot: Reload (+ IsShortReload), its release, Action, Bolt.
					foley_own_timed(e, r_clock);
					break;
				case host::PresentKind::RemoteShot:
					remote_shot(e, r_clock);
					break;
				case host::PresentKind::Detonation: {
					const WeaponAudioDef *d = def_of(e.def);
					if (d != nullptr && d->explosion) {
						if (one_shot(d->explosion, 0, sound_seed(e.slot, 0x40000000u | (e.id & 0x3FFFFFFFu)), event_emitter(e),
									r_clock.frame_of(e.time, e.timeline), false)) {
							++stats.detonations;
						}
					}
					break;
				}
				case host::PresentKind::Death:
					foley_death(e.aux, r_clock); // PT1 rld [death]: a death cuts the soldier's running reload sound
					if (e.aux == own_slot) {
						end_track(own);
					} else {
						end_track(tracks[e.aux]);
					}
					break;
				case host::PresentKind::Spawn:
					if (e.slot == own_slot) {
						end_track(own);
					} else {
						Track &t = tracks[e.slot];
						end_track(t);
						t.weapon_seen = false;
					}
					break;
				default:
					break;
			}
		}
		// The own burst ends when the trigger is up, or when the predicted clock passed the next round's time.
		if (own.held != kNoInstance) {
			Voice *v = voice(own.held);
			if (v == nullptr) {
				own.held = kNoInstance;
			} else {
				bool end = own_trigger_known && !own_trigger_down;
				const host::PresentClock &now = r_clock.now();
				if (!end && now.predicted_valid) {
					const double hold = config.own_hold_intervals * v->interval_s * double(sw::kTimeUnitsPerSecond);
					end = double(now.predicted - v->last_round_time) > hold;
				}
				if (end) {
					trigger_up(*v);
					own.held = kNoInstance;
				}
			}
		}
		// Keep the own 1p voices at the listener.
		if (own_first_person) {
			for (Voice &v : voices) {
				if (v.own) {
					EmitterState em;
					em.position = listener.position;
					em.forward = listener.forward;
					if (!same_emitter(em, v.emitter)) {
						v.emitter = em;
						v.emitter_dirty = true;
					}
				}
			}
		}
	}

	EmitterState other_emitter(const host::PresentedSoldier &p_s, const MuzzlePoint *p_muzzle) const {
		EmitterState e;
		if (p_muzzle != nullptr && p_muzzle->valid && std::isfinite(p_muzzle->position.x) && std::isfinite(p_muzzle->position.y) &&
				std::isfinite(p_muzzle->position.z)) {
			e.position = p_muzzle->position;
			e.forward = p_muzzle->forward;
		} else {
			const float cp = std::cos(p_s.pitch), sp = std::sin(p_s.pitch), cy = std::cos(p_s.yaw), sy = std::sin(p_s.yaw);
			const Vec3 fwd{ sy * cp, sp, cy * cp };
			const float f = config.standin_muzzle_forward_m;
			e.position = { p_s.position[0] + fwd.x * f, p_s.position[1] + p_s.eye_height + fwd.y * f, p_s.position[2] + fwd.z * f };
			e.forward = fwd;
		}
		e.velocity = { p_s.velocity[0], p_s.velocity[1], p_s.velocity[2] };
		if (!std::isfinite(e.position.x) || !std::isfinite(e.position.y) || !std::isfinite(e.position.z)) {
			e = EmitterState{};
		}
		return e;
	}

	// Every RemoteShot of a burst was lost: the records start it at the evidence's time (counted late by the clock
	// when that is behind the mixer: the render clock is already there).
	void start_other_from_state(Track &r_t, uint8_t p_slot, uint16_t p_def, uint8_t p_burst_seq, sw::SimTime p_evidence, SimMixerClock &r_clock) {
		const WeaponAudioDef *d = def_of(p_def);
		if (d == nullptr || d->form != WeaponAudioForm::Driver) {
			if (d == nullptr || d->form == WeaponAudioForm::None) {
				++stats.unbound_defs;
			}
			return; // knife / M67: their one-shots come from RemoteShot only (no loop to keep alive)
		}
		Voice *v = start_burst(r_t, false, p_slot, p_def, sound_seed(p_slot, p_burst_seq), r_clock.frame_of(p_evidence, host::PresentTimeline::Render),
				p_evidence, host::PresentTimeline::Render, r_t.emitter, r_t.distance, false);
		if (v == nullptr) {
			return;
		}
		++stats.state_rounds;
		if (v->single) {
			trigger_up(*v);
			r_t.held = kNoInstance;
			r_t.ended_evidence = std::max(r_t.ended_evidence, p_evidence); // this record is the shot it started
		}
	}

	void on_others(std::span<const host::PresentedSoldier> p_soldiers, std::span<const host::PresentedOtherWeapon> p_weapons, std::span<const MuzzlePoint> p_muzzles,
			SimMixerClock &r_clock) {
		clock = &r_clock;
		++frame_counter;
		const host::PresentClock &now = r_clock.now();
		for (size_t i = 0; i < p_soldiers.size(); ++i) {
			const host::PresentedSoldier &s = p_soldiers[i];
			if (s.slot == own_slot) {
				continue;
			}
			Track &t = tracks[s.slot];
			t.present_frame = frame_counter;
			t.emitter = other_emitter(s, i < p_muzzles.size() ? &p_muzzles[i] : nullptr);
			t.distance = distance(t.emitter.position, listener.position);
			if (t.seen && s.generation != t.generation) {
				end_track(t); // a new life
				t.weapon_seen = false;
			}
			t.seen = true;
			t.generation = s.generation;
			Voice *v = t.held != kNoInstance ? voice(t.held) : nullptr;
			if (t.held != kNoInstance && v == nullptr) {
				t.held = kNoInstance;
			}
			if (v != nullptr) {
				v->distance = t.distance;
				if (!same_emitter(v->emitter, t.emitter)) {
					v->emitter = t.emitter;
					v->emitter_dirty = true;
				}
			}
			if (i >= p_weapons.size() || !p_weapons[i].valid || p_weapons[i].slot != s.slot) {
				continue;
			}
			const host::PresentedOtherWeapon &w = p_weapons[i];
			if (v != nullptr && v->def != w.def) {
				trigger_up(*v); // a weapon switch ends the burst
				t.held = kNoInstance;
				v = nullptr;
			}
			// PT1 rld [others]: replaces the Reload-only one-shot on a reload_seq change: Reload, its release, Action.
			foley_other(s, w, t, r_clock);
			bool evidence = false;
			const sw::SimTime when = sw::SimTime(w.sample_tick) * sw::kTimeUnitsPerTick;
			if (t.weapon_seen) {
				evidence = w.burst_seq != t.burst_seq || (w.firing && w.sample_tick != t.last_firing_tick);
			} else {
				evidence = w.firing;
			}
			if (w.sample_tick != t.last_firing_tick || !t.weapon_seen) {
				t.seq_ahead = 0; // a newer record: its burst_seq counts the bursts up to its tick
			}
			t.weapon_seen = true;
			t.burst_seq = w.burst_seq;
			t.reload_seq = w.reload_seq;
			t.def = w.def;
			t.last_firing_tick = w.sample_tick;
			// A record no newer than the last ended burst's evidence belongs to that burst.
			if (evidence && w.firing && when > t.ended_evidence) {
				if (v == nullptr) {
					start_other_from_state(t, s.slot, w.def, w.burst_seq, when, r_clock);
				} else {
					v->last_evidence = std::max(v->last_evidence, when);
				}
			}
		}
		// Every held burst of the others: rounds the RemoteShots did not place (a lost datagram), one per interval up to
		// the newest evidence; the end when the render clock passes its last evidence by an interval plus the hold.
		for (size_t i = 0; i < voices.size(); ++i) {
			Voice &v = voices[i];
			if (v.own || !v.held) {
				continue;
			}
			const sw::SimTime iu = interval_units(v);
			Track &vt = tracks[v.slot];
			const bool ahead = horizon_valid && v.have_id; // a RemoteShot burst with the newest records known
			if (ahead && vt.newest_fire > v.first_time) {
				v.last_evidence = std::max(v.last_evidence, vt.newest_fire);
			}
			// A firing record says a shot fell within two ticks before its sample: the round nearest the sample on the
			// burst's grid (rounded, so a sample up to two ticks after a known shot names that shot).
			if (!v.single && iu > 0 && v.last_evidence > v.last_event_time + iu / 2 && v.last_evidence > v.first_time) {
				const int64_t k_max = std::llround(double(v.last_evidence - v.first_time) / double(iu));
				for (int64_t k = int64_t(v.next_round); k <= k_max && k < 100000; ++k) {
					post_round(v, uint32_t(k), v.first_time + k * iu, false);
				}
			}
			// The end: with the newest records, once they and the RemoteShots run past the last shot by an interval
			// plus other_event_hold_ticks (decided about the render delay early, so the stop event lands half an
			// interval after the last round on time); without them once the render clock passes the last evidence by
			// an interval plus other_hold_ticks.
			const sw::SimTime last = std::max({ v.last_evidence, v.last_event_time, v.last_round_time });
			bool end = false;
			if (ahead) {
				end = horizon - last > iu + sw::SimTime(std::llround(config.other_event_hold_ticks * double(sw::kTimeUnitsPerTick)));
			} else if (now.render_valid) {
				end = now.render - last > other_hold_units(v);
			}
			if (end) {
				trigger_up(v);
				// The records show firing up to two ticks after a shot: those samples belong to this burst.
				vt.ended_evidence = std::max(vt.ended_evidence, last + 2 * sw::kTimeUnitsPerTick);
				if (vt.held == v.handle) {
					vt.held = kNoInstance;
				}
			}
		}
		horizon_valid = false; // set_others_newest again next frame
		// Soldiers not drawn this frame (left, out of the window): their bursts end.
		for (uint32_t slot = 0; slot < tracks.size(); ++slot) {
			Track &t = tracks[slot];
			if (t.held != kNoInstance && t.present_frame != frame_counter) {
				end_track(t);
			}
		}
	}

	void set_others_newest(std::span<const host::PresentedOtherWeapon> p_newest, sw::SimTime p_horizon) {
		horizon = p_horizon;
		horizon_valid = true;
		for (const host::PresentedOtherWeapon &w : p_newest) {
			if (!w.valid || w.slot == own_slot) {
				continue;
			}
			Track &t = tracks[w.slot];
			if (w.firing) {
				t.newest_fire = std::max(t.newest_fire, sw::SimTime(w.sample_tick) * sw::kTimeUnitsPerTick);
			}
		}
	}

	void set_listener(const ListenerState &p_listener) {
		const auto eq = [](const Vec3 &a, const Vec3 &b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
		if (listener_set && eq(listener.position, p_listener.position) && eq(listener.forward, p_listener.forward) && eq(listener.up, p_listener.up) &&
				eq(listener.velocity, p_listener.velocity)) {
			return;
		}
		listener = p_listener;
		listener_set = true;
		MixerCommand c;
		c.type = MixerCommandType::SetListener;
		c.listener = p_listener;
		if (!mixer.push(c)) {
			listener_set = false; // retried next frame
			return;
		}
		// The mixer applies a listener on the next SetEmitter only (mixer_core.cpp): re-send every live emitter.
		for (Voice &v : voices) {
			if (v.own && own_first_person) {
				v.emitter.position = listener.position;
				v.emitter.forward = listener.forward;
			}
			v.emitter_dirty = true;
		}
		stats.emitter_updates += pool.resend_emitters(mixer);
	}

	void update() {
		for (Voice &v : voices) {
			if (!v.ops.empty()) {
				flush(v);
			}
			if (v.emitter_dirty && mixer.push(emitter_command(v.handle, v.emitter))) {
				v.emitter_dirty = false;
				++stats.emitter_updates;
			}
		}
		std::array<MixerFeedback, 128> fb;
		for (int guard = 0; guard < 64; ++guard) {
			const size_t n = mixer.poll_feedback(fb);
			for (size_t i = 0; i < n; ++i) {
				if (!fb[i].finished) {
					continue;
				}
				if (voice(fb[i].handle) != nullptr) {
					erase_voice(fb[i].handle);
				} else {
					pool.on_finished(fb[i].handle);
				}
			}
			if (n < fb.size()) {
				break;
			}
		}
		pool.update(mixer, config.mix_rate);
		const uint64_t clock_now = mixer.clock();
		const uint64_t forget = uint64_t(config.voice_forget_seconds * double(config.mix_rate));
		for (size_t i = 0; i < voices.size();) {
			Voice &v = voices[i];
			if (!v.held && v.ops.empty() && clock_now > v.stop_frame + forget) {
				const InstanceHandle h = v.handle;
				kill(h);
				erase_voice(h);
				++stats.forgotten;
				continue;
			}
			++i;
		}
		mixer.reclaim();
		stats.weapon_voices = uint32_t(voices.size());
		stats.peak_weapon_voices = std::max(stats.peak_weapon_voices, stats.weapon_voices);
	}

	// PT1 rld [foley] (docs/design/pt1-rld.md, weapon_foley.h): the reload, Action and Bolt one-shots of the weapon
	// patches, posted where the weapon state changes. The own soldier's come from the sim's timed events (exact times
	// on the Predicted timeline, so they start with the viewmodel clip of the same event) and the presented own weapons
	// (dry fire, zoom); the others' from their presented fields on the Render timeline (the frame the crowd shows the
	// change). The own 1p one-shots follow the listener; the others' sit at the shooter's muzzle and are not started
	// beyond their event's distance band (the data's crossfader would start nothing there).
	struct OtherFoley {
		bool seen = false;
		uint8_t generation = 0;
		uint16_t def = sw::kNoWeapon;
		sw::WeaponPhase phase = sw::WeaponPhase::None;
		bool ads = false;
		InstanceHandle reload = kNoInstance; // the running reload one-shot (for its release)
		uint16_t reload_def = sw::kNoWeapon;
	};
	std::array<OtherFoley, 256> other_foley{};
	InstanceHandle own_reload = kNoInstance;
	uint16_t own_reload_def = sw::kNoWeapon;
	std::vector<InstanceHandle> own_foley; // the own 1p foley one-shots still in the pool (they follow the listener)
	bool own_weapons_valid = false;
	net::WeaponsWire own_weapons{};
	bool own_weapons_seen = false;
	net::WeaponsWire own_weapons_last{};
	bool own_zoom = false;
	uint32_t foley_counter = 0;

	// Starts a foley one-shot: p_event, after p_param = p_value when p_param != 0. kNoInstance when refused.
	InstanceHandle foley_start(const FoleyCue &p_cue, uint32_t p_event, uint32_t p_param, float p_value, const EmitterState &p_emitter) {
		const WeaponAudioDef *d = def_of(p_cue.def);
		if (d == nullptr || !d->program) {
			return kNoInstance;
		}
		OneShotOptions o;
		o.event = p_event;
		o.set_first_person = true;
		o.first_person = p_cue.first_person;
		o.mix_rate = config.mix_rate;
		WeaponAudioObserver *obs = observer;
		const FoleyCue cue = p_cue;
		o.before_start = [p_param, p_value, obs, cue](PatchInstance &r_instance, InstanceHandle) {
			if (p_param != 0) {
				r_instance.set_parameter(p_param, p_value);
			}
			if (obs != nullptr) {
				obs->on_foley(cue, &r_instance);
			}
		};
		const uint64_t seed = sound_seed(p_cue.slot, 0x20000000u | (++foley_counter & 0x0FFFFFFFu));
		if (!pool.start(mixer, d->program, bank, tuning, seed, p_emitter, listener, p_cue.frame, o)) {
			return kNoInstance;
		}
		++stats.one_shots;
		const InstanceHandle h = pool.last_handle();
		if (p_cue.own && p_cue.first_person) {
			own_foley.push_back(h);
		}
		return h;
	}

	// The reload's release event on its running instance (the reload was cut short).
	void foley_release(InstanceHandle &r_handle, uint16_t p_def, const FoleyCue &p_cue) {
		if (r_handle == kNoInstance) {
			return;
		}
		const WeaponAudioDef *d = def_of(p_def);
		if (d != nullptr && d->foley.reload.release_event != 0 && pool.contains(r_handle)) {
			MixerCommand c;
			c.type = MixerCommandType::PostEvent;
			c.handle = r_handle;
			c.name_hash = d->foley.reload.release_event;
			c.at_frame = p_cue.frame;
			if (mixer.push(c)) {
				++stats.foley_releases;
				if (observer != nullptr) {
					FoleyCue rc = p_cue;
					rc.kind = FoleyCueKind::ReloadRelease;
					rc.def = p_def;
					observer->on_foley(rc, nullptr);
				}
			}
		}
		r_handle = kNoInstance;
	}

	EmitterState own_foley_emitter() const {
		EmitterState e;
		e.position = listener.position;
		e.forward = listener.forward;
		return e;
	}

	FoleyCue own_cue(uint16_t p_def, sw::SimTime p_time, uint64_t p_frame) const {
		FoleyCue c;
		c.own = true;
		c.first_person = own_first_person;
		c.slot = own_slot;
		c.def = p_def;
		c.time = p_time;
		c.frame = p_frame;
		return c;
	}

	void foley_action(FoleyCue &r_cue, FoleyAction p_action, const EmitterState &p_emitter) {
		const WeaponAudioDef *d = def_of(r_cue.def);
		if (d == nullptr || !d->foley.action.present) {
			return;
		}
		// A patch without ActionType (the knife: Action alone plays its deploy) takes the event only for a deploy.
		if (!d->foley.action_type_param && p_action != FoleyAction::Deploy) {
			return;
		}
		if (!r_cue.own && r_cue.distance_m > d->foley.action.reach_m) {
			++stats.foley_out_of_reach;
			return;
		}
		r_cue.kind = FoleyCueKind::Action;
		r_cue.action = p_action;
		if (foley_start(r_cue, kHashAction, d->foley.action_type_param ? kHashActionType : 0, float(uint8_t(p_action)), p_emitter) != kNoInstance) {
			++stats.foley;
		}
	}

	void foley_reload(FoleyCue &r_cue, int32_t p_magazine, const EmitterState &p_emitter, InstanceHandle &r_handle, uint16_t &r_handle_def) {
		const WeaponAudioDef *d = def_of(r_cue.def);
		if (d == nullptr || !d->foley.reload.present) {
			return;
		}
		if (!r_cue.own && r_cue.distance_m > d->foley.reload.reach_m) {
			++stats.foley_out_of_reach;
			return;
		}
		r_cue.kind = FoleyCueKind::Reload;
		// IsShortReload: 1 when rounds are left (tactical), 0 when empty; unknown: not set (the data's default 0).
		uint32_t param = 0;
		if (d->foley.short_reload_param && p_magazine >= 0) {
			param = kHashIsShortReload;
			r_cue.short_reload = p_magazine > 0 ? 1 : 0;
		}
		r_handle = foley_start(r_cue, kHashReload, param, r_cue.short_reload > 0 ? 1.0f : 0.0f, p_emitter);
		r_handle_def = r_cue.def;
		if (r_handle != kNoInstance) {
			++stats.reloads;
		}
	}

	// OwnTimed: kReloadStarted, kReloadCancelled, kDeployStarted, kFireModeChanged, kBoltStarted.
	void foley_own_timed(const host::PresentEvent &p_e, SimMixerClock &r_clock) {
		FoleyCue c = own_cue(p_e.def, p_e.time, r_clock.frame_of(p_e.time, host::PresentTimeline::Predicted));
		const EmitterState em = own_foley_emitter();
		switch (p_e.id) {
			case sw::weapon_events::kReloadStarted: {
				int32_t magazine = -1;
				if (own_weapons_valid && p_e.aux < sw::kMaxWeaponSlots && own_weapons.slots[p_e.aux].def == p_e.def) {
					magazine = own_weapons.slots[p_e.aux].magazine; // unchanged until the commit at 85 % of the reload
				}
				foley_reload(c, magazine, em, own_reload, own_reload_def);
				break;
			}
			case sw::weapon_events::kReloadCancelled:
				foley_release(own_reload, own_reload_def, c);
				break;
			case sw::weapon_events::kDeployStarted:
				foley_release(own_reload, own_reload_def, c); // a switch cuts a running reload
				foley_action(c, FoleyAction::Deploy, em);
				break;
			case sw::weapon_events::kFireModeChanged:
				foley_action(c, FoleyAction::FireMode, em);
				break;
			case sw::weapon_events::kBoltStarted: {
				const WeaponAudioDef *d = def_of(p_e.def);
				if (d != nullptr && d->foley.bolt.present) {
					c.kind = FoleyCueKind::Bolt;
					if (foley_start(c, kHashBolt, 0, 0.0f, em) != kNoInstance) {
						++stats.foley;
					}
				}
				break;
			}
			default:
				break;
		}
	}

	// The presented own weapons of this frame: the zoom's rising edge and the dry-fire tick flag (no timed events).
	void foley_own_state(SimMixerClock &r_clock) {
		if (!own_weapons_valid) {
			own_weapons_seen = false;
			return;
		}
		const net::WeaponsWire &w = own_weapons;
		const bool zoom = w.active < sw::kMaxWeaponSlots && w.timers[0].slot == w.active && (w.timers[0].flags & sw::slot_flags::kZoomWasDown) != 0;
		const bool fresh = !own_weapons_seen || !(w == own_weapons_last);
		const uint16_t def = w.active < sw::kMaxWeaponSlots ? w.slots[w.active].def : sw::kNoWeapon;
		const host::PresentClock &now = r_clock.now();
		if (own_weapons_seen && def != sw::kNoWeapon && now.predicted_valid) {
			const uint64_t frame = r_clock.frame_of(now.predicted, host::PresentTimeline::Predicted);
			if (zoom && !own_zoom) {
				FoleyCue c = own_cue(def, now.predicted, frame);
				foley_action(c, FoleyAction::Zoom, own_foley_emitter());
			}
			if (fresh && (w.events & sw::weapon_events::kDryFire) != 0) {
				FoleyCue c = own_cue(def, now.predicted, frame);
				foley_action(c, FoleyAction::DryFire, own_foley_emitter());
			}
		}
		own_zoom = zoom;
		own_weapons_last = w;
		own_weapons_seen = true;
	}

	// Another soldier's presented fields: reload_seq (a reload started), the def (a switch: deploy), the phase (a
	// reload cut short) and ads (zoom). Called once per drawn soldier with a weapon record, before the bursts' use of it.
	void foley_other(const host::PresentedSoldier &p_s, const host::PresentedOtherWeapon &p_w, const Track &p_t, SimMixerClock &r_clock) {
		OtherFoley &o = other_foley[p_s.slot];
		const bool same_life = o.seen && o.generation == p_s.generation && p_t.weapon_seen;
		const sw::SimTime when = sw::SimTime(p_w.sample_tick) * sw::kTimeUnitsPerTick;
		FoleyCue c;
		c.slot = p_s.slot;
		c.def = p_w.def;
		c.time = when;
		c.frame = r_clock.frame_of(when, host::PresentTimeline::Render);
		c.distance_m = p_t.distance;
		if (same_life) {
			const bool reloading = o.phase == sw::WeaponPhase::Reloading || o.phase == sw::WeaponPhase::ReloadShell;
			if (o.reload != kNoInstance && (p_w.def != o.reload_def || (reloading && p_w.phase == sw::WeaponPhase::Deploying))) {
				foley_release(o.reload, o.reload_def, c);
			}
			if (p_w.reload_seq != p_t.reload_seq) {
				FoleyCue rc = c;
				foley_reload(rc, -1, p_t.emitter, o.reload, o.reload_def); // the others' magazine is not sent: the data's default
			}
			if (p_w.def != o.def && p_w.def != sw::kNoWeapon) {
				FoleyCue dc = c;
				foley_action(dc, FoleyAction::Deploy, p_t.emitter);
			}
			if (p_w.ads && !o.ads && p_w.def == o.def) {
				FoleyCue zc = c;
				foley_action(zc, FoleyAction::Zoom, p_t.emitter);
			}
		} else {
			o.reload = kNoInstance;
		}
		o.seen = true;
		o.generation = p_s.generation;
		o.def = p_w.def;
		o.phase = p_w.phase;
		o.ads = p_w.ads;
	}

	// A death cuts the soldier's running reload sound.
	void foley_death(uint8_t p_slot, SimMixerClock &r_clock) {
		FoleyCue c;
		c.slot = p_slot;
		const host::PresentClock &now = r_clock.now();
		if (p_slot == own_slot) {
			c.own = true;
			c.frame = r_clock.frame_of(now.predicted, host::PresentTimeline::Predicted);
			foley_release(own_reload, own_reload_def, c);
		} else {
			c.frame = r_clock.frame_of(now.render, host::PresentTimeline::Render);
			foley_release(other_foley[p_slot].reload, other_foley[p_slot].reload_def, c);
		}
	}

	// The own 1p foley stays at the listener (the camera moves during a reload).
	void foley_follow_listener() {
		size_t keep = 0;
		for (size_t i = 0; i < own_foley.size(); ++i) {
			if (!pool.contains(own_foley[i])) {
				continue;
			}
			if (own_first_person && mixer.push(emitter_command(own_foley[i], own_foley_emitter()))) {
				++stats.emitter_updates;
			}
			own_foley[keep++] = own_foley[i];
		}
		own_foley.resize(keep);
	}
};

WeaponAudioRouter::WeaponAudioRouter(MixerCore &r_mixer, std::shared_ptr<const WeaponAudioTable> p_table, std::shared_ptr<const ca::WaveBank> p_bank,
		const AudioTuning &p_tuning, OneShotPool &r_one_shots, const WeaponAudioConfig &p_config) :
		impl_(std::make_unique<Impl>(r_mixer, std::move(p_table), std::move(p_bank), p_tuning, r_one_shots, p_config)) {}

WeaponAudioRouter::~WeaponAudioRouter() {
	if (impl_) {
		for (const Impl::Voice &v : impl_->voices) {
			impl_->kill(v.handle);
		}
	}
}

void WeaponAudioRouter::set_own_slot(uint8_t p_slot) {
	if (p_slot != impl_->own_slot) {
		impl_->end_track(impl_->own);
		if (impl_->own_slot < impl_->tracks.size()) {
			impl_->tracks[impl_->own_slot] = Impl::Track{};
		}
		impl_->end_track(impl_->tracks[p_slot]);
	}
	impl_->own_slot = p_slot;
}

void WeaponAudioRouter::set_own_first_person(bool p_first_person) {
	impl_->own_first_person = p_first_person;
}

void WeaponAudioRouter::set_own_trigger(bool p_down) {
	impl_->own_trigger_known = true;
	impl_->own_trigger_down = p_down;
}

void WeaponAudioRouter::set_listener(const ListenerState &p_listener) {
	impl_->set_listener(p_listener);
}

void WeaponAudioRouter::on_events(std::span<const host::PresentEvent> p_events, SimMixerClock &r_clock) {
	impl_->on_events(p_events, r_clock);
}

void WeaponAudioRouter::set_others_newest(std::span<const host::PresentedOtherWeapon> p_newest, sim::weapons::SimTime p_horizon) {
	impl_->set_others_newest(p_newest, p_horizon);
}

void WeaponAudioRouter::on_others(std::span<const host::PresentedSoldier> p_soldiers, std::span<const host::PresentedOtherWeapon> p_weapons,
		std::span<const MuzzlePoint> p_muzzles, SimMixerClock &r_clock) {
	impl_->on_others(p_soldiers, p_weapons, p_muzzles, r_clock);
}

void WeaponAudioRouter::update(uint64_t) {
	impl_->update();
}

void WeaponAudioRouter::set_observer(WeaponAudioObserver *p_observer) {
	impl_->observer = p_observer;
}

const WeaponAudioStats &WeaponAudioRouter::stats() const {
	return impl_->stats;
}

} // namespace bf3::audio

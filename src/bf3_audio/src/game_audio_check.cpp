// The in-game audio check (game_audio_check.h; docs/design/m2a.md E7; as built: docs/design/m2a-au3.md "E7").
// Records the game mixer's run and judges it with au's public analyse_check plus the in-game rows; no au file is
// edited. The probes evaluate a fresh PatchInstance of the window's program at the step's distance (au's G5
// method), so the rows say what the in-game geometry made the patch do.

#include "bf3/audio/game_audio_check.h"

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/wav_io.h"
#include "bf3/core/audio/sound_ports.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bf3::audio {

namespace ca = core::audio;
namespace cp = core::audio::port;

namespace {

constexpr double kAuditionWindowSeconds = 1.0; // the RMS window of an audition one-shot
constexpr double kSilenceDb = -100.0; // the 320 m bursts' core tap must stay below (the 300-301 m Fire gate)
constexpr double kAudibleDb = -90.0; // an in-band audition window must rise above (full mix)
constexpr float kDistanceTolerance = 0.25f;

// The M16A3's far bands (au G5.2): the close group 0-20 m, the far groups 250-300 m; the distant HPF 17..200 m ->
// 0..310 Hz (v46 ScaleClamp).
constexpr float kCloseBegin = 0.0f, kCloseEnd = 20.0f, kFarBegin = 250.0f, kFarEnd = 300.0f;

float cosine_fade(float p_d, float p_begin, float p_end) {
	if (p_d <= p_begin) {
		return 1.0f;
	}
	if (p_d >= p_end) {
		return 0.0f;
	}
	const float t = (p_d - p_begin) / (p_end - p_begin);
	return 0.5f + 0.5f * std::cos(3.14159265f * t);
}

float expected_hpf_hz(float p_d) {
	return 310.0f * std::clamp((p_d - 17.0f) / 183.0f, 0.0f, 1.0f);
}

struct BandRef {
	uint32_t node = 0, group = 0;
	float begin = 0.0f, end = 0.0f;
};

std::vector<BandRef> crossfader_bands(const ca::SoundProgram &p_prog) {
	std::vector<BandRef> out;
	for (uint32_t ni = 0; ni < p_prog.nodes.size(); ++ni) {
		const ca::SoundNode &n = p_prog.nodes[ni];
		if (n.type != ca::SoundNodeType::MultiCrossfader) {
			continue;
		}
		for (uint32_t g = 0; g < n.children_count; ++g) {
			const size_t ci = size_t(n.children_begin) + g;
			if (ci >= p_prog.children.size() || p_prog.children[ci] >= p_prog.child_nodes.size()) {
				continue;
			}
			const ca::SoundNode &c = p_prog.child_nodes[p_prog.children[ci]];
			if (size_t(c.floats_begin) + cp::GroupFadeEnd >= p_prog.floats.size()) {
				continue;
			}
			out.push_back(BandRef{ ni, g, p_prog.floats[c.floats_begin + cp::GroupFadeBegin], p_prog.floats[c.floats_begin + cp::GroupFadeEnd] });
		}
	}
	return out;
}

// A non-owning shared_ptr to a program the caller keeps alive for the call (the instance being started holds it).
std::shared_ptr<const ca::SoundProgram> borrow(const ca::SoundProgram &p_program) {
	return std::shared_ptr<const ca::SoundProgram>(std::shared_ptr<const ca::SoundProgram>(), &p_program);
}

std::unique_ptr<PatchInstance> probe_at(const ca::SoundProgram &p_program, uint64_t p_seed, float p_distance, const AudioTuning &p_tuning, uint32_t p_rate) {
	core::Result<std::unique_ptr<PatchInstance>> r = PatchInstance::create(borrow(p_program), nullptr, p_tuning, p_seed, p_rate);
	if (!r) {
		return nullptr;
	}
	EmitterState e;
	e.position = { 0.0f, 0.0f, p_distance };
	e.forward = { 0.0f, 0.0f, -1.0f };
	(*r)->set_geometry(e, ListenerState{});
	(*r)->evaluate(0.0);
	return std::move(*r);
}

std::string fmt(const char *p_format, double p_a, double p_b = 0.0, double p_c = 0.0) {
	char buf[200];
	std::snprintf(buf, sizeof(buf), p_format, p_a, p_b, p_c);
	return buf;
}

} // namespace

// ---- the script ---------------------------------------------------------------------------------------------------

std::vector<GameAuditionItem> game_audition_items() {
	// au §10.2 / G5.4: the bands each item is judged by, and distances before, inside and past them.
	return {
		{ "m98b", "sound/weapons/handheld/m98_b/weapon_sniper_m98_b_02", { 280.0f, 325.0f, 400.0f }, { { 300.0f, 350.0f } } },
		{ "rpg7", "sound/weapons/handheld/rpg-7/weapon_at_rpg7", { 30.0f, 67.5f, 120.0f }, { { 45.0f, 90.0f } } },
		{ "m1a2", "sound/vehicles/land/m1a2_abrams/m1a2_abrams", { 10.0f, 32.5f, 90.0f, 120.0f }, { { 15.0f, 50.0f }, { 80.0f, 100.0f } } },
	};
}

GameCheckScript game_check_script() {
	GameCheckScript s;
	for (float d : kGameCheckDistances) {
		GameCheckStep st;
		st.name = "d" + std::to_string(int(d));
		st.distance_m = d;
		st.bursts = 2;
		s.steps.push_back(st);
	}
	// au's far_pinned segment: one 150 m burst with both distance HPFs pinned to 0 Hz (the hpf rows' reference).
	GameCheckStep pinned;
	pinned.name = "far_pinned";
	pinned.distance_m = 150.0f;
	pinned.bursts = 1;
	pinned.pin_distance_hpfs = true;
	s.steps.push_back(pinned);
	for (const GameAuditionItem &item : game_audition_items()) {
		for (float d : item.distances_m) {
			GameCheckStep st;
			st.name = item.name + "@" + fmt("%g", d);
			st.distance_m = d;
			st.bursts = 1;
			st.audition = true;
			st.patch = item.patch;
			st.bands = item.bands;
			s.steps.push_back(st);
		}
	}
	s.analysis = CheckScript{};
	s.analysis.rounds = s.rounds;
	s.timeout_s = 120.0;
	return s;
}

// ---- GameAudioCheck -----------------------------------------------------------------------------------------------

struct GameAudioCheck::State {
	GameCheckScript script;
	uint32_t rate = kMixRate;
	uint64_t start = 0;
	uint64_t total = 0;
	std::vector<float> mix; // stereo, preallocated
	std::atomic<uint64_t> recorded_until{ 0 };
	std::atomic<uint64_t> dropped{ 0 };
	std::unique_ptr<TapRecorder> core_tap, noise_tap;
	uint8_t shooter = 0xFF;
	int64_t step = -1;
	uint64_t step_begin = 0;
	float step_distance = 0.0f;
	std::vector<GameCheckWindow> windows;
	std::vector<uint32_t> per_step; // windows taken per step
	core::PathGuard guard;
	AudioTuning tuning;
};

GameAudioCheck::GameAudioCheck(const GameCheckScript &p_script, uint32_t p_rate, uint64_t p_start_frame) :
		state_(std::make_unique<State>()) {
	State &st = *state_;
	st.script = p_script;
	st.rate = p_rate != 0 ? p_rate : kMixRate;
	st.start = p_start_frame;
	const double seconds = std::clamp(p_script.timeout_s, 1.0, 600.0); // bounded: 600 s of stereo is 230 MB
	st.total = uint64_t(seconds * double(st.rate));
	st.mix.assign(size_t(st.total) * 2, 0.0f);
	st.core_tap = std::make_unique<TapRecorder>(st.start, st.total);
	st.noise_tap = std::make_unique<TapRecorder>(st.start, st.total);
	uint64_t bursts = 0;
	for (const GameCheckStep &s : p_script.steps) {
		bursts += s.bursts;
	}
	st.windows.reserve(size_t(std::min<uint64_t>(bursts, 4096)));
	st.per_step.assign(p_script.steps.size(), 0);
	st.recorded_until.store(p_start_frame, std::memory_order_relaxed);
}

GameAudioCheck::~GameAudioCheck() = default;

void GameAudioCheck::set_shooter(uint8_t p_slot) {
	state_->shooter = p_slot;
}

void GameAudioCheck::begin_step(uint32_t p_step, float p_listener_distance_m) {
	State &st = *state_;
	if (p_step >= st.script.steps.size()) {
		st.step = -1;
		return;
	}
	st.step = p_step;
	st.step_distance = p_listener_distance_m;
	st.step_begin = st.recorded_until.load(std::memory_order_relaxed);
}

void GameAudioCheck::on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t p_first_frame) {
	State &st = *state_;
	uint64_t dropped = 0;
	for (uint32_t i = 0; i < p_frames; ++i) {
		const uint64_t f = p_first_frame + i;
		if (f < st.start || f - st.start >= st.total) {
			++dropped;
			continue;
		}
		const size_t k = size_t(f - st.start) * 2;
		st.mix[k] = p_interleaved_stereo[size_t(i) * 2];
		st.mix[k + 1] = p_interleaved_stereo[size_t(i) * 2 + 1];
	}
	if (dropped != 0) {
		st.dropped.fetch_add(dropped, std::memory_order_relaxed);
	}
	st.recorded_until.store(p_first_frame + p_frames, std::memory_order_release);
}

void GameAudioCheck::on_driver_instance(uint8_t p_slot, uint64_t p_seed, uint64_t p_first_round_frame, PatchInstance &r_instance) {
	State &st = *state_;
	if (p_slot != st.shooter || st.step < 0) {
		return;
	}
	const GameCheckStep &step = st.script.steps[size_t(st.step)];
	if (step.audition || st.per_step[size_t(st.step)] >= step.bursts) {
		return;
	}
	const uint64_t settle = uint64_t(std::max(0.0, st.script.settle_s) * double(st.rate));
	if (p_first_round_frame < st.step_begin + settle || p_first_round_frame < st.start) {
		return; // the camera was still moving (or before the recording)
	}
	r_instance.set_voice_tap(kM16CoreTapVoices, st.core_tap.get());
	r_instance.add_voice_tap(kM16NoiseTapVoices, st.noise_tap.get());
	if (step.pin_distance_hpfs) {
		r_instance.override_plugin_attribute(kM16NoiseHpf, 0, 0.0f);
		r_instance.override_plugin_attribute(kM16CoreHpf, 0, 0.0f);
	}
	GameCheckWindow w;
	w.step = uint32_t(st.step);
	w.seed = p_seed;
	w.first_frame = p_first_round_frame;
	w.listener_distance_m = st.step_distance;
	// The probes: a fresh instance of the same program and seed at the step's distance.
	const ca::SoundProgram &prog = r_instance.program();
	if (std::unique_ptr<PatchInstance> probe = probe_at(prog, p_seed, st.step_distance, st.tuning, st.rate)) {
		w.probe_hpf_hz = probe->plugin_attribute(kM16NoiseHpf, 0);
		float far_sum = 0.0f;
		int far_n = 0;
		for (const BandRef &b : crossfader_bands(prog)) {
			if (b.begin == kCloseBegin && b.end == kCloseEnd) {
				w.probe_close = probe->crossfader_group_amplitude(b.node, b.group);
			}
			if (b.begin == kFarBegin && b.end == kFarEnd) {
				far_sum += probe->crossfader_group_amplitude(b.node, b.group);
				++far_n;
			}
		}
		w.probe_far = far_n > 0 ? far_sum / float(far_n) : -1.0f;
		probe->set_parameter(kHashIsFirstPerson, 0.0f);
		probe->post_event(kHashFire, 0);
		probe->evaluate(0.075);
		w.probe_pitch = probe->value(28);
	}
	st.windows.push_back(w);
	++st.per_step[size_t(st.step)];
}

void GameAudioCheck::on_trigger_up(uint8_t p_slot, uint64_t p_seed, uint64_t p_stop_frame) {
	State &st = *state_;
	if (p_slot != st.shooter) {
		return;
	}
	for (auto it = st.windows.rbegin(); it != st.windows.rend(); ++it) {
		if (!it->audition && !it->stopped && it->seed == p_seed) {
			it->stop_frame = std::max(p_stop_frame, it->first_frame + 1);
			it->stopped = true;
			return;
		}
	}
}

void GameAudioCheck::on_audition(uint32_t p_step, uint64_t p_seed, uint64_t p_frame, PatchInstance &r_instance) {
	State &st = *state_;
	if (p_step >= st.script.steps.size() || st.per_step[p_step] >= st.script.steps[p_step].bursts) {
		return;
	}
	const GameCheckStep &step = st.script.steps[p_step];
	GameCheckWindow w;
	w.step = p_step;
	w.seed = p_seed;
	w.first_frame = p_frame;
	w.stop_frame = p_frame + uint64_t(kAuditionWindowSeconds * double(st.rate));
	w.stopped = true;
	w.audition = true;
	w.listener_distance_m = st.step == int64_t(p_step) ? st.step_distance : step.distance_m;
	const ca::SoundProgram &prog = r_instance.program();
	if (std::unique_ptr<PatchInstance> probe = probe_at(prog, p_seed, w.listener_distance_m, st.tuning, st.rate)) {
		const std::vector<BandRef> bands = crossfader_bands(prog);
		for (size_t i = 0; i < step.bands.size() && i < w.band_amplitude.size(); ++i) {
			for (const BandRef &b : bands) {
				if (b.begin == step.bands[i].first && b.end == step.bands[i].second) {
					w.band_amplitude[i] = probe->crossfader_group_amplitude(b.node, b.group);
					++w.bands_found;
					break;
				}
			}
		}
	}
	st.windows.push_back(w);
	++st.per_step[p_step];
}

bool GameAudioCheck::step_done(uint32_t p_step) const {
	const State &st = *state_;
	if (p_step >= st.script.steps.size()) {
		return true;
	}
	if (st.per_step[p_step] < st.script.steps[p_step].bursts) {
		return false;
	}
	const uint64_t now = st.recorded_until.load(std::memory_order_acquire);
	const uint64_t tail = uint64_t(st.script.burst_period_s * double(st.rate));
	for (const GameCheckWindow &w : st.windows) {
		if (w.step == p_step && (!w.stopped || now < w.first_frame + tail)) {
			return false;
		}
	}
	return true;
}

std::span<const GameCheckWindow> GameAudioCheck::windows() const {
	return state_->windows;
}

void GameAudioCheck::set_guard(const core::PathGuard &p_guard) {
	state_->guard = p_guard;
}

core::Status GameAudioCheck::write_wav(const std::filesystem::path &p_path) const {
	return write_wav(state_->guard, p_path);
}

core::Status GameAudioCheck::write_wav(const core::PathGuard &p_guard, const std::filesystem::path &p_path) const {
	const State &st = *state_;
	const uint64_t until = st.recorded_until.load(std::memory_order_acquire);
	const uint64_t frames = until > st.start ? std::min(until - st.start, st.total) : 0;
	return bf3::audio::write_wav(p_guard, p_path, std::span<const float>(st.mix.data(), size_t(frames) * 2), 2, st.rate);
}

const GameCheckScript &GameAudioCheck::script() const {
	return state_->script;
}

uint32_t GameAudioCheck::rate() const {
	return state_->rate;
}

uint64_t GameAudioCheck::start_frame() const {
	return state_->start;
}

uint64_t GameAudioCheck::total_frames() const {
	return state_->total;
}

uint64_t GameAudioCheck::recorded_until() const {
	return state_->recorded_until.load(std::memory_order_acquire);
}

std::span<const float> GameAudioCheck::mix() const {
	return state_->mix;
}

std::span<const float> GameAudioCheck::core_tap() const {
	return state_->core_tap->samples();
}

std::span<const float> GameAudioCheck::noise_tap() const {
	return state_->noise_tap->samples();
}

uint64_t GameAudioCheck::dropped_frames() const {
	return state_->dropped.load(std::memory_order_relaxed);
}

// ---- analysis -----------------------------------------------------------------------------------------------------

CheckReport analyse_game_check(const GameCheckScript &p_script, const GameAudioCheck &p_check, const GameCheckCounters &p_counters,
		const AudioTuning &p_tuning, uint32_t p_rate) {
	CheckReport report;
	const auto row = [&](std::string p_name, double p_value, std::string p_rule, bool p_pass) {
		report.rows.push_back(CheckRow{ std::move(p_name), p_value, std::move(p_rule), p_pass });
	};
	const uint64_t start = p_check.start_frame();
	const uint64_t total = p_check.total_frames();
	const std::span<const GameCheckWindow> windows = p_check.windows();
	const auto step_name = [&](uint32_t p_step) -> const std::string & { return p_script.steps[p_step].name; };
	// au's segment names for the steps analyse_check's level and hpf rows read.
	const auto segment_name = [&](const GameCheckStep &p_step) -> std::string {
		if (p_step.pin_distance_hpfs) {
			return "far_pinned";
		}
		if (p_step.distance_m == 5.0f) {
			return "near";
		}
		if (p_step.distance_m == 100.0f) {
			return "mid";
		}
		if (p_step.distance_m == 150.0f) {
			return "far";
		}
		return p_step.name;
	};

	// Coverage: every step recorded its bursts.
	for (uint32_t s = 0; s < p_script.steps.size(); ++s) {
		const GameCheckStep &step = p_script.steps[s];
		uint32_t n = 0;
		for (const GameCheckWindow &w : windows) {
			n += (w.step == s && w.stopped) ? 1 : 0;
		}
		row("windows." + step.name, double(n), "== " + std::to_string(step.bursts), n == step.bursts);
	}

	// au's analyse_check over the burst steps below the Fire gate: one segment per step.
	CheckScript cs = p_script.analysis;
	cs.rounds = p_script.rounds;
	cs.segments.clear();
	std::vector<ScheduledBurst> bursts;
	std::vector<int> segment_of_step(p_script.steps.size(), -1);
	for (uint32_t s = 0; s < p_script.steps.size(); ++s) {
		const GameCheckStep &step = p_script.steps[s];
		if (step.audition || step.distance_m > 300.0f) {
			continue;
		}
		CheckSegment seg;
		seg.name = segment_name(step);
		seg.distance_m = step.distance_m;
		seg.bursts = step.bursts;
		seg.pin_distance_hpfs = step.pin_distance_hpfs;
		segment_of_step[s] = int(cs.segments.size());
		cs.segments.push_back(seg);
	}
	std::vector<uint32_t> index_in_segment(cs.segments.size(), 0);
	float far_hpf = -1.0f;
	for (const GameCheckWindow &w : windows) {
		if (w.audition || !w.stopped || w.step >= segment_of_step.size() || segment_of_step[w.step] < 0 || w.first_frame < start ||
				w.stop_frame - start > total) {
			continue;
		}
		ScheduledBurst b;
		b.segment = uint32_t(segment_of_step[w.step]);
		b.index = index_in_segment[b.segment]++;
		b.seed = w.seed;
		b.first_round_frame = w.first_frame - start;
		b.stop_frame = w.stop_frame - start;
		if (cs.segments[b.segment].name == "far" && b.index == 0) {
			far_hpf = w.probe_hpf_hz;
		}
		bursts.push_back(b);
	}
	if (far_hpf < 0.0f) {
		far_hpf = expected_hpf_hz(150.0f);
	}
	CheckReport au = analyse_check(cs, bursts, p_check.mix(), p_check.core_tap(), p_check.noise_tap(), far_hpf, p_rate, p_tuning);
	for (CheckRow &r : au.rows) {
		report.rows.push_back(std::move(r));
	}

	// The Fire gate: the 320 m bursts start no sampler (silent core tap over their windows).
	for (const GameCheckWindow &w : windows) {
		const GameCheckStep &step = p_script.steps[w.step];
		if (w.audition || !w.stopped || step.distance_m <= 301.0f || w.first_frame < start || w.stop_frame - start > total) {
			continue;
		}
		const double db = rms_db(p_check.core_tap(), 1, w.first_frame - start, w.stop_frame - w.first_frame);
		row("gate." + step.name, db, fmt("<= %.0f dB (core tap, the 300-301 m Fire gate)", kSilenceDb), db <= kSilenceDb);
	}

	// The listener distance of every window within 0.25 m of its step's, and the probes (au G5 laws at the in-game
	// distance).
	std::vector<uint32_t> seen(p_script.steps.size(), 0);
	for (const GameCheckWindow &w : windows) {
		const GameCheckStep &step = p_script.steps[w.step];
		const std::string tag = step_name(w.step) + "." + std::to_string(seen[w.step]++);
		const float dd = std::fabs(w.listener_distance_m - step.distance_m);
		row("distance." + tag, dd, fmt("<= %.2f m (listener at %.2f m)", kDistanceTolerance, w.listener_distance_m), dd <= kDistanceTolerance);
		const float d = w.listener_distance_m;
		if (!w.audition) {
			if (step.pin_distance_hpfs || d > 301.0f) {
				continue;
			}
			const float want_hpf = expected_hpf_hz(d);
			row("probe.hpf." + tag, std::fabs(w.probe_hpf_hz - want_hpf), fmt("<= 1 Hz (v46 %.2f, 310 x clamp((d - 17) / 183) = %.2f Hz)", w.probe_hpf_hz, want_hpf),
					w.probe_hpf_hz >= 0.0f && std::fabs(w.probe_hpf_hz - want_hpf) <= 1.0f);
			row("probe.pitch." + tag, w.probe_pitch, "in [1.21, 1.262] (v28 after the Fire)", w.probe_pitch >= 1.21f && w.probe_pitch <= 1.262f);
			const float want_close = cosine_fade(d, kCloseBegin, kCloseEnd);
			row("probe.close." + tag, std::fabs(w.probe_close - want_close), fmt("<= 1e-4 (0-20 m group %.4f, cosine %.4f)", w.probe_close, want_close),
					w.probe_close >= 0.0f && std::fabs(w.probe_close - want_close) <= 1e-4f);
			if (d >= kFarBegin) {
				const float want_far = cosine_fade(d, kFarBegin, kFarEnd);
				row("probe.far." + tag, std::fabs(w.probe_far - want_far), fmt("<= 1e-4 (250-300 m groups %.4f, cosine %.4f)", w.probe_far, want_far),
						w.probe_far >= 0.0f && std::fabs(w.probe_far - want_far) <= 1e-4f);
			}
			continue;
		}
		// Auditions: each band's group amplitude partial inside its band, exactly 0 or 1 outside (au G5.4 reading R),
		// and the in-band windows audible in the mix.
		if (w.bands_found != std::min<size_t>(step.bands.size(), w.band_amplitude.size())) {
			row("audition.bands." + tag, double(w.bands_found), "every item band is a crossfader group of the patch", false);
			continue;
		}
		bool inside_any = false;
		for (size_t i = 0; i < step.bands.size() && i < w.band_amplitude.size(); ++i) {
			const auto [b, e] = step.bands[i];
			const float a = w.band_amplitude[i];
			const bool inside = d > b && d < e;
			inside_any = inside_any || inside;
			const std::string band = fmt("%g-%g", b, e);
			if (inside) {
				row("audition.band." + tag + "." + band, a, "in (0, 1) inside the band", a > 0.0f && a < 1.0f);
			} else {
				row("audition.band." + tag + "." + band, a, "0 or 1 outside the band", a == 0.0f || a == 1.0f);
			}
		}
		if (inside_any && w.first_frame >= start && w.stop_frame - start <= total) {
			const double db = rms_db(p_check.mix(), 2, w.first_frame - start, w.stop_frame - w.first_frame);
			row("audition.audible." + tag, db, fmt(">= %.0f dBFS (full mix, %.1f s window)", kAudibleDb, kAuditionWindowSeconds), db >= kAudibleDb);
		}
	}

	// In-game counters.
	row("mixer.short_blocks", double(p_counters.short_blocks), "== 0", p_counters.short_blocks == 0);
	row("mixer.late_blocks", double(p_counters.late_blocks), "== 0", p_counters.late_blocks == 0);
	row("mixer.late_commands", double(p_counters.late_commands), "== 0", p_counters.late_commands == 0);
	row("router.mixer_refusals", double(p_counters.mixer_refusals), "== 0", p_counters.mixer_refusals == 0);
	row("record.dropped_frames", double(p_check.dropped_frames()), "report (blocks outside the recording span)", true);
	return report;
}

} // namespace bf3::audio

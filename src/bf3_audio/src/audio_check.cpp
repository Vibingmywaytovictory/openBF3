// The M16A3 check shared by G7 (offline) and G8 (Godot) (audio_check.h, design §10.4).

#include "bf3/audio/audio_check.h"

#include "bf3/audio/wav_io.h"
#include "bf3/core/audio/sound_ports.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace bf3::audio {

namespace ca = core::audio;

namespace {

constexpr uint64_t kSeedBase = 0x5eed0016a3ull;
constexpr double kScheduleHorizonSeconds = 0.5; // a burst's commands are pushed this long before its first round
constexpr uint32_t kStartLeadFrames = 4 * kBlockFrames; // the Start lands this many frames before round 0
constexpr uint32_t kStopAfterFrames = 2 * kBlockFrames; // the Stop command, after the stop event
constexpr size_t kTraceEntries = 4096;

} // namespace

CheckScript m16_check_script() {
	CheckScript s;
	s.segments = {
		{ "near", 0.5f, false, 2, kSeedBase + 0, false },
		{ "mid", 60.0f, false, 2, kSeedBase + 2, false },
		{ "far", 150.0f, false, 2, kSeedBase + 4, false },
		{ "far_pinned", 150.0f, false, 1, kSeedBase + 4, true },
	};
	return s;
}

std::vector<ScheduledBurst> schedule_check(const CheckScript &p_script, uint32_t p_rate) {
	std::vector<ScheduledBurst> out;
	uint32_t k = 0;
	for (uint32_t s = 0; s < p_script.segments.size(); ++s) {
		for (uint32_t b = 0; b < p_script.segments[s].bursts; ++b, ++k) {
			ScheduledBurst sb;
			sb.segment = s;
			sb.index = b;
			sb.seed = p_script.segments[s].seed + b;
			sb.first_round_frame = uint64_t(std::llround((p_script.start_s + double(k) * p_script.burst_period_s) * double(p_rate)));
			const double stop_s = double(p_script.rounds > 0 ? p_script.rounds - 1 : 0) * p_script.shot_interval_s + p_script.stop_after_last_round_s;
			sb.stop_frame = sb.first_round_frame + uint64_t(std::llround(stop_s * double(p_rate)));
			out.push_back(sb);
		}
	}
	return out;
}

// ---- TapRecorder --------------------------------------------------------------------------------------------------

TapRecorder::TapRecorder(uint64_t p_start_frame, uint64_t p_frames) :
		start_(p_start_frame), buffer_(size_t(p_frames), 0.0f) {}

void TapRecorder::on_voices(const float *p_mono, uint32_t p_frames, uint64_t p_first_frame) {
	for (uint32_t i = 0; i < p_frames; ++i) {
		const uint64_t f = p_first_frame + i;
		if (f < start_ || f - start_ >= buffer_.size()) {
			++dropped_;
			continue;
		}
		buffer_[size_t(f - start_)] += p_mono[i];
	}
}

std::span<const float> TapRecorder::samples() const {
	return buffer_;
}

uint64_t TapRecorder::dropped_frames() const {
	return dropped_;
}

// ---- CheckDriver --------------------------------------------------------------------------------------------------

struct CheckDriver::State {
	CheckScript script;
	std::shared_ptr<const ca::SoundProgram> program;
	std::shared_ptr<const ca::WaveBank> bank;
	AudioTuning tuning;
	uint64_t start = 0;
	uint32_t rate = kMixRate;
	std::vector<ScheduledBurst> bursts;
	WeaponSoundBinding binding;
	std::unique_ptr<TapRecorder> core_tap, noise_tap;
	std::vector<SamplerTrace> traces;
	float far_noise_hpf = 0.0f;
	uint64_t total = 0;
	// Per burst: the commands still to push (Start first), and the instance until the Start is accepted.
	struct Pending {
		bool built = false;
		std::vector<MixerCommand> commands;
		size_t cursor = 0;
		std::unique_ptr<PatchInstance> instance;
	};
	std::vector<Pending> pending;
};

namespace {

bool has_hpf_link(const ca::SoundProgram &p, ca::SoundPluginRef p_plugin) {
	if (p_plugin.voice >= p.voices.size() || p_plugin.plugin >= p.voices[p_plugin.voice].plugins_count) {
		return false;
	}
	if (p.plugins[p.voices[p_plugin.voice].plugins_begin + p_plugin.plugin].id != ca::fourcc("HI20")) {
		return false;
	}
	for (const ca::SoundPluginLink &l : p.links) {
		if (l.plugin == p_plugin && l.attribute == 0 && l.port.connected()) {
			return true;
		}
	}
	return false;
}

} // namespace

CheckDriver::~CheckDriver() = default;

core::Result<std::unique_ptr<CheckDriver>> CheckDriver::create(const CheckScript &p_script, std::shared_ptr<const ca::SoundProgram> p_program,
		std::shared_ptr<const ca::WaveBank> p_bank, const AudioTuning &p_tuning, uint64_t p_start_frame, uint32_t p_rate) {
	if (!p_program || !p_bank) {
		return core::make_error("CheckDriver: no program or bank");
	}
	const ca::SoundProgram &p = *p_program;
	if (p.voices.size() < 30) {
		return core::make_error("CheckDriver: " + p.name + " has " + std::to_string(p.voices.size()) + " voices; the M16A3 check needs 30");
	}
	if (!has_hpf_link(p, kM16NoiseHpf) || !has_hpf_link(p, kM16CoreHpf)) {
		return core::make_error("CheckDriver: the M16A3 distance high-passes (voice 14 plugin 2, voice 6 plugin 1) are not linked HI20 plugins");
	}
	core::Result<WeaponSoundBinding> binding = bind_weapon_sound(p_program, p_script.shot_interval_s, p_rate);
	if (!binding) {
		return binding.error();
	}
	if (!binding->step_event || !binding->enable_step_parameter) {
		return core::make_error("CheckDriver: the patch has no Sampler Step event / EnableStep parameter");
	}
	std::unique_ptr<CheckDriver> self(new CheckDriver());
	self->state_ = std::make_unique<State>();
	State &st = *self->state_;
	st.script = p_script;
	st.program = p_program;
	st.bank = p_bank;
	st.tuning = p_tuning;
	st.start = p_start_frame;
	st.rate = p_rate;
	st.bursts = schedule_check(p_script, p_rate);
	st.binding = *binding;
	st.total = uint64_t(std::llround((p_script.start_s + double(st.bursts.size()) * p_script.burst_period_s) * double(p_rate)));
	st.core_tap = std::make_unique<TapRecorder>(st.start, st.total);
	st.noise_tap = std::make_unique<TapRecorder>(st.start, st.total);
	st.traces.resize(st.bursts.size());
	for (SamplerTrace &t : st.traces) {
		t.entries.resize(kTraceEntries);
	}
	st.pending.resize(st.bursts.size());
	// The far segment's evaluated voice-14 HPF, for the prediction of row (d).
	{
		core::Result<std::unique_ptr<PatchInstance>> probe = PatchInstance::create(p_program, p_bank, p_tuning, 1, p_rate);
		if (!probe) {
			return probe.error();
		}
		float far = 150.0f;
		for (const CheckSegment &s : p_script.segments) {
			if (s.name == "far") {
				far = s.distance_m;
			}
		}
		EmitterState e;
		e.position = { 0.0f, 0.0f, far };
		e.forward = { 0.0f, 0.0f, -1.0f };
		(*probe)->set_geometry(e, ListenerState{});
		(*probe)->evaluate(0.0);
		st.far_noise_hpf = (*probe)->plugin_attribute(kM16NoiseHpf, 0);
	}
	return self;
}

core::Status CheckDriver::pump(MixerCore &r_mixer) {
	State &st = *state_;
	const uint64_t horizon = uint64_t(kScheduleHorizonSeconds * double(st.rate));
	for (size_t b = 0; b < st.bursts.size(); ++b) {
		State::Pending &pend = st.pending[b];
		const ScheduledBurst &burst = st.bursts[b];
		const uint64_t first = st.start + burst.first_round_frame;
		if (!pend.built) {
			if (r_mixer.clock() + horizon < first) {
				break; // later bursts are later still
			}
			const CheckSegment &seg = st.script.segments[burst.segment];
			if (first < r_mixer.earliest_frame() + kStartLeadFrames) {
				return core::make_error("CheckDriver: burst " + std::to_string(b) + " is due before the mixer's earliest frame (the game thread stalled)");
			}
			core::Result<std::unique_ptr<PatchInstance>> inst = PatchInstance::create(st.program, st.bank, st.tuning, burst.seed, st.rate);
			if (!inst) {
				return inst.error();
			}
			PatchInstance &pi = **inst;
			const InstanceHandle handle = r_mixer.next_handle();
			WeaponSoundDriver driver(st.binding, st.script.fire_mode, handle);
			driver.prepare(pi, seg.first_person);
			EmitterState e;
			e.position = { 0.0f, 0.0f, seg.distance_m };
			e.forward = { 0.0f, 0.0f, -1.0f };
			pi.set_geometry(e, ListenerState{});
			if (seg.pin_distance_hpfs || seg.pin_noise_hpf) {
				pi.override_plugin_attribute(kM16NoiseHpf, 0, 0.0f);
			}
			if (seg.pin_distance_hpfs) {
				pi.override_plugin_attribute(kM16CoreHpf, 0, 0.0f);
			}
			pi.set_voice_tap(kM16CoreTapVoices, st.core_tap.get());
			pi.add_voice_tap(kM16NoiseTapVoices, st.noise_tap.get());
			pi.set_sampler_trace(&st.traces[b]);
			pend.instance = std::move(*inst);
			// The command list: Start, Fire, Steps, the stop event, Stop.
			MixerCommand start;
			start.type = MixerCommandType::Start;
			start.handle = handle;
			start.at_frame = first - kStartLeadFrames;
			start.instance = pend.instance.get();
			pend.commands.push_back(start);
			const auto event = [&](uint32_t p_hash, uint64_t p_frame) {
				MixerCommand c;
				c.type = MixerCommandType::PostEvent;
				c.handle = handle;
				c.name_hash = p_hash;
				c.at_frame = p_frame;
				pend.commands.push_back(c);
			};
			event(st.binding.start_event, first);
			for (uint32_t r = 1; r < st.script.rounds; ++r) {
				const uint32_t hash = st.script.fire_mode == WeaponFireMode::Single ? st.binding.start_event : *st.binding.step_event;
				event(hash, first + uint64_t(std::llround(double(r) * st.script.shot_interval_s * double(st.rate))));
			}
			const uint64_t stop = st.start + burst.stop_frame;
			if (st.binding.stop_event != 0) {
				event(st.binding.stop_event, stop);
			}
			MixerCommand stop_cmd;
			stop_cmd.type = MixerCommandType::Stop;
			stop_cmd.handle = handle;
			stop_cmd.at_frame = stop + kStopAfterFrames;
			pend.commands.push_back(stop_cmd);
			pend.built = true;
		}
		while (pend.cursor < pend.commands.size()) {
			const MixerCommand &c = pend.commands[pend.cursor];
			if (c.at_frame != 0 && c.at_frame < r_mixer.clock()) {
				return core::make_error("CheckDriver: a command of burst " + std::to_string(b) + " would land late");
			}
			if (!r_mixer.push(c)) {
				return core::Status::success(); // ring full: retry next pump
			}
			if (c.type == MixerCommandType::Start) {
				(void)pend.instance.release(); // the mixer owns it now
			}
			++pend.cursor;
		}
	}
	return core::Status::success();
}

bool CheckDriver::done(const MixerCore &p_mixer) const {
	const State &st = *state_;
	for (const State::Pending &p : st.pending) {
		if (!p.built || p.cursor < p.commands.size()) {
			return false;
		}
	}
	return p_mixer.clock() >= st.start + st.total;
}

uint64_t CheckDriver::start_frame() const {
	return state_->start;
}

uint64_t CheckDriver::total_frames() const {
	return state_->total;
}

std::span<const ScheduledBurst> CheckDriver::bursts() const {
	return state_->bursts;
}

const TapRecorder &CheckDriver::core_tap() const {
	return *state_->core_tap;
}

const TapRecorder &CheckDriver::noise_tap() const {
	return *state_->noise_tap;
}

std::span<const SamplerTrace> CheckDriver::traces() const {
	return state_->traces;
}

float CheckDriver::far_noise_hpf_hz() const {
	return state_->far_noise_hpf;
}

// ---- analysis -----------------------------------------------------------------------------------------------------

bool CheckReport::pass() const {
	if (rows.empty()) {
		return false;
	}
	for (const CheckRow &r : rows) {
		if (!r.pass) {
			return false;
		}
	}
	return true;
}

std::string CheckReport::table() const {
	std::string out;
	char line[512];
	for (const CheckRow &r : rows) {
		std::snprintf(line, sizeof(line), "%s %s %.3f %s\n", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.value, r.rule.c_str());
		out += line;
	}
	return out;
}

CheckReport analyse_check(const CheckScript &p_script, std::span<const ScheduledBurst> p_bursts, std::span<const float> p_mix_stereo,
		std::span<const float> p_core_tap, std::span<const float> p_noise_tap, float p_far_noise_hpf_hz, uint32_t p_rate, const AudioTuning &p_tuning) {
	CheckReport report;
	const auto row = [&](std::string p_name, double p_value, std::string p_rule, bool p_pass) {
		report.rows.push_back(CheckRow{ std::move(p_name), p_value, std::move(p_rule), p_pass });
	};
	char rule[160];
	// (a) onsets on the core tap, per burst window.
	const std::vector<uint64_t> onsets = detect_onsets(p_core_tap, 1, p_rate, OnsetOptions{});
	const uint64_t lead = uint64_t(0.010 * double(p_rate));
	for (const ScheduledBurst &b : p_bursts) {
		const std::string tag = p_script.segments[b.segment].name + "." + std::to_string(b.index);
		const BurstWindow window{ b.first_round_frame > lead ? b.first_round_frame - lead : 0, b.stop_frame };
		const OnsetSpacing sp = onset_spacing(onsets, window, p_script.expected_spacing_frames);
		std::snprintf(rule, sizeof(rule), "== %u", p_script.rounds);
		row("onsets." + tag, double(sp.count), rule, sp.count == p_script.rounds);
		std::snprintf(rule, sizeof(rule), "<= %.0f (mean gap %.1f, expected %.0f)", p_script.spacing_tolerance_frames, sp.mean_frames, p_script.expected_spacing_frames);
		row("spacing." + tag, sp.max_deviation_frames, rule, sp.count == p_script.rounds && sp.max_deviation_frames <= p_script.spacing_tolerance_frames);
	}
	// (b) full-mix levels: mean burst-window RMS per segment.
	const auto segment_level = [&](std::string_view p_name) {
		double sum = 0.0;
		int n = 0;
		for (const ScheduledBurst &b : p_bursts) {
			if (p_script.segments[b.segment].name == p_name) {
				sum += rms_db(p_mix_stereo, 2, b.first_round_frame, b.stop_frame - b.first_round_frame);
				++n;
			}
		}
		return n ? sum / n : -144.0;
	};
	const double near = segment_level("near"), mid = segment_level("mid"), far = segment_level("far");
	std::snprintf(rule, sizeof(rule), ">= 3 dB (near %.1f, mid %.1f dBFS)", near, mid);
	row("level.near_minus_mid", near - mid, rule, near - mid >= 3.0);
	std::snprintf(rule, sizeof(rule), ">= 3 dB (mid %.1f, far %.1f dBFS)", mid, far);
	row("level.mid_minus_far", mid - far, rule, mid - far >= 3.0);
	// (c), (d): far's first burst against far_pinned, 20-100 Hz.
	const ScheduledBurst *far0 = nullptr, *pinned = nullptr;
	for (const ScheduledBurst &b : p_bursts) {
		const CheckSegment &s = p_script.segments[b.segment];
		if (s.name == "far" && b.index == 0) {
			far0 = &b;
		}
		if (s.pin_distance_hpfs && pinned == nullptr) {
			pinned = &b;
		}
	}
	if (far0 == nullptr || pinned == nullptr) {
		row("hpf.segments", 0.0, "the script has far and far_pinned", false);
		return report;
	}
	const uint64_t len = far0->stop_frame - far0->first_round_frame;
	const float mix_far = band_energy_db(p_mix_stereo, 2, p_rate, far0->first_round_frame, len, 20.0f, 100.0f);
	const float mix_pin = band_energy_db(p_mix_stereo, 2, p_rate, pinned->first_round_frame, len, 20.0f, 100.0f);
	std::snprintf(rule, sizeof(rule), ">= 10 dB (pinned %.1f, far %.1f dB)", mix_pin, mix_far);
	row("hpf.mix", double(mix_pin - mix_far), rule, mix_pin - mix_far >= 10.0f);
	const float tap_far = band_energy_db(p_noise_tap, 1, p_rate, far0->first_round_frame, len, 20.0f, 100.0f);
	const float tap_pin = band_energy_db(p_noise_tap, 1, p_rate, pinned->first_round_frame, len, 20.0f, 100.0f);
	std::snprintf(rule, sizeof(rule), ">= 10 dB (pinned %.1f, far %.1f dB)", tap_pin, tap_far);
	row("hpf.noise_tap", double(tap_pin - tap_far), rule, tap_pin - tap_far >= 10.0f);
	const float predicted =
			predicted_highpass_band_db(p_noise_tap, 1, p_rate, pinned->first_round_frame, len, 20.0f, 100.0f, p_far_noise_hpf_hz, p_tuning.iir2_q);
	std::snprintf(rule, sizeof(rule), "<= 1.5 dB (measured %.2f, predicted %.2f dB at %.2f Hz)", tap_far, predicted, p_far_noise_hpf_hz);
	row("hpf.noise_prediction", std::fabs(double(tap_far - predicted)), rule, std::fabs(tap_far - predicted) <= 1.5f);
	return report;
}

// ---- offline render --------------------------------------------------------------------------------------------

core::Result<OfflineCheck> run_check_offline(const CheckScript &p_script, std::shared_ptr<const ca::SoundProgram> p_program,
		std::shared_ptr<const ca::WaveBank> p_bank, const AudioTuning &p_tuning, const OfflineCheckOptions &p_options) {
	MixerConfig config = p_options.mixer;
	config.schedule_lead_blocks = uint32_t(p_tuning.schedule_lead_blocks);
	auto mixer = std::make_unique<MixerCore>(config);
	const uint32_t rate = config.mix_rate;
	const uint64_t start = p_options.start_frame != 0 ? p_options.start_frame : mixer->earliest_frame();
	core::Result<std::unique_ptr<CheckDriver>> driver = CheckDriver::create(p_script, p_program, p_bank, p_tuning, start, rate);
	if (!driver) {
		return driver.error();
	}
	CheckDriver &d = **driver;
	const uint64_t end = start + d.total_frames();
	WavRecorder recorder(rate, double(end + kBlockFrames * 4) / double(rate));
	mixer->set_tap(&recorder);
	std::vector<float> buffer(size_t(8192) * 2);
	const std::vector<uint32_t> calls = p_options.call_frames.empty() ? std::vector<uint32_t>{ kBlockFrames } : p_options.call_frames;
	size_t call = 0;
	uint64_t served = 0;
	while (!d.done(*mixer) || served < end) {
		BF3_TRY(d.pump(*mixer));
		const uint32_t n = std::clamp<uint32_t>(calls[call++ % calls.size()], 1, 8192);
		mixer->mix(buffer.data(), n);
		served += n;
		mixer->reclaim();
		if (served > end + uint64_t(rate) * 600) {
			return core::make_error("run_check_offline: the script did not finish");
		}
	}
	OfflineCheck out;
	const std::span<const float> rec = recorder.samples();
	if (rec.size() < size_t(end) * 2) {
		return core::make_error("run_check_offline: the recording is short");
	}
	out.bursts.assign(d.bursts().begin(), d.bursts().end());
	out.start_frame = start;
	out.total_frames = d.total_frames();
	out.mix.assign(rec.begin() + ptrdiff_t(start * 2), rec.begin() + ptrdiff_t(end * 2));
	out.core_tap.assign(d.core_tap().samples().begin(), d.core_tap().samples().end());
	out.noise_tap.assign(d.noise_tap().samples().begin(), d.noise_tap().samples().end());
	out.traces.assign(d.traces().begin(), d.traces().end());
	out.far_noise_hpf_hz = d.far_noise_hpf_hz();
	const MixerStats &st = mixer->stats();
	out.short_blocks = st.short_blocks.load();
	out.late_commands = st.late_commands.load();
	out.ring_overflows = st.ring_overflows.load();
	out.starved_voices = st.starved_voices.load();
	out.decode_errors = st.decode_errors.load();
	out.deferred_events = st.deferred_events.load();
	out.tap_dropped = d.core_tap().dropped_frames() + d.noise_tap().dropped_frames();
	for (const SamplerTrace &t : out.traces) {
		out.trace_dropped += t.dropped;
	}
	out.record_dropped = recorder.dropped_frames();
	out.report = analyse_check(p_script, out.bursts, out.mix, out.core_tap, out.noise_tap, out.far_noise_hpf_hz, rate, p_tuning);
	mixer.reset(); // destroys any instance still owned (none once the script is done)
	return out;
}

// ---- row (e): the sampler model --------------------------------------------------------------------------------

namespace {

// The Sampler node whose first slot plays on p_voice.
std::optional<uint32_t> sampler_on_voice(const ca::SoundProgram &p, uint16_t p_voice) {
	for (uint32_t i = 0; i < p.nodes.size(); ++i) {
		const ca::SoundNode &n = p.nodes[i];
		if (n.type == ca::SoundNodeType::Sampler && n.plugins_count >= ca::port::kSamplerSlotPlugins &&
				p.plugin_refs[n.plugins_begin + ca::port::SlotSndPlayer].voice == p_voice) {
			return i;
		}
	}
	return std::nullopt;
}

} // namespace

void add_model_trace_rows(CheckReport &r_report, const OfflineCheck &p_check, const CheckScript &p_script, const ca::SoundProgram &p_program,
		const ca::WaveBank &p_bank, uint32_t p_rate) {
	const std::optional<uint32_t> noise = sampler_on_voice(p_program, 20);
	const std::optional<uint32_t> core = sampler_on_voice(p_program, 26);
	if (!noise || !core) {
		r_report.rows.push_back(CheckRow{ "trace.samplers", 0.0, "noise_far on voice 20 and corebassdistant on voice 26", false });
		return;
	}
	char rule[200];
	for (size_t b = 0; b < p_check.bursts.size(); ++b) {
		const ScheduledBurst &burst = p_check.bursts[b];
		const CheckSegment &seg = p_script.segments[burst.segment];
		if (seg.distance_m < 50.0f) {
			continue; // noise_far and corebassdistant play from 50 m
		}
		const std::string tag = seg.name + "." + std::to_string(burst.index);
		const SamplerTrace &trace = p_check.traces[b];
		const uint64_t first = p_check.start_frame + burst.first_round_frame;
		const uint64_t stop = p_check.start_frame + burst.stop_frame;
		// noise_far
		{
			std::vector<SamplerTraceEntry> e;
			for (uint32_t i = 0; i < trace.count; ++i) {
				if (trace.entries[i].node == *noise) {
					e.push_back(trace.entries[i]);
				}
			}
			bool ok = e.size() >= 4 && e.front().kind == SamplerTraceKind::Trigger && e.back().kind == SamplerTraceKind::End;
			double worst = 0.0;
			size_t tails = 0;
			std::string why;
			for (size_t i = 1; ok && i + 1 < e.size(); ++i) {
				const SamplerTraceEntry &prev = e[i - 1], &cur = e[i];
				if (cur.kind == SamplerTraceKind::Tail) {
					++tails;
				} else if (cur.kind != SamplerTraceKind::Advance) {
					ok = false;
					why = "unexpected kind";
					break;
				}
				if (cur.wave >= p_bank.waves().size() || cur.variation >= p_bank.waves()[cur.wave].variations.size()) {
					ok = false;
					why = "trace wave";
					break;
				}
				const ca::WaveVariation &var = p_bank.waves()[cur.wave].variations[cur.variation];
				if (prev.segment >= var.segments.size()) {
					ok = false;
					why = "segment index";
					break;
				}
				const double expected = double(var.segments[prev.segment].length_seconds) * double(p_rate) / double(prev.pitch);
				worst = std::max(worst, std::fabs(double(cur.frame - prev.frame) - expected) / std::max(1.0, expected));
				if (cur.prev_end != cur.frame || cur.slot != prev.slot) {
					ok = false;
					why = "not contiguous on one slot";
				}
				if (cur.kind == SamplerTraceKind::Advance) {
					if (cur.frame >= stop || int32_t(cur.segment) < var.first_loop_segment || int32_t(cur.segment) > var.last_loop_segment) {
						ok = false;
						why = "loop segment after the stop or outside the loop";
					}
				} else if (int32_t(cur.segment) != var.last_loop_segment + 1 || cur.frame < stop) {
					ok = false;
					why = "tail segment or frame";
				}
			}
			// Contiguity is exact (prev_end == frame on one slot); the duration of each segment is its length at the pitch
			// of its first frame, within 3 % (the Lfo moves the pitch by up to 2 % inside a segment).
			ok = ok && tails == 1 && worst <= 0.03;
			std::snprintf(rule, sizeof(rule), "contiguous, duration error <= 0.03; %zu entries, %zu tail%s%s", e.size(), tails, why.empty() ? "" : "; ",
					why.c_str());
			r_report.rows.push_back(CheckRow{ "trace.noise_far." + tag, worst, rule, ok });
		}
		// corebassdistant
		{
			std::vector<SamplerTraceEntry> e;
			for (uint32_t i = 0; i < trace.count; ++i) {
				const SamplerTraceEntry &x = trace.entries[i];
				if (x.node == *core && (x.kind == SamplerTraceKind::Trigger || x.kind == SamplerTraceKind::Step)) {
					e.push_back(x);
				}
			}
			bool ok = e.size() == p_script.rounds && !e.empty() && e.front().kind == SamplerTraceKind::Trigger;
			double worst = 0.0;
			for (size_t r = 0; ok && r < e.size(); ++r) {
				const uint64_t want = first + uint64_t(std::llround(double(r) * p_script.shot_interval_s * double(p_rate)));
				worst = std::max(worst, std::fabs(double(e[r].frame) - double(want)));
				if (r > 0 && (e[r].kind != SamplerTraceKind::Step || e[r].slot == e[r - 1].slot)) {
					ok = false;
				}
			}
			ok = ok && worst == 0.0;
			std::snprintf(rule, sizeof(rule), "== 0 frames; %zu starts (want %u), alternating slots", e.size(), p_script.rounds);
			r_report.rows.push_back(CheckRow{ "trace.corebass." + tag, worst, rule, ok });
		}
	}
}

} // namespace bf3::audio

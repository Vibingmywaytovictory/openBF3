// M2b au: audio A3 (docs/design/m2b.md §6, E10; docs/design/m2b-au.md).
//   audio-hdr          (audio.hdr.unit, data-free, < 2 s) E10: the HDR window's numbers (HdrWindow alone and through
//                      MixerCore with synthetic HDR sources: a 71 dB footstep, a 110 dB shot, a 63 dB source), the
//                      gain the window gives (R5), pre-cull, suspension, the release on the retail curve; the scope
//                      rules (cull_voices: DuplicateSpawn, closest N, latest N, max N; Bullet_Impact keeps the closest
//                      5 within 1 m / 0.4 s, Death_Scream at most 4) and the 128-voice cap, also through MixerCore;
//                      the profiler; the non-HDR path unchanged; hostile settings and spans.
//   audio-hdr-install  (audio.hdr.install, exit 77 without an install, install.unchanged) the retail HdrSetting and
//                      the 33 scopes read from MP_Subway, the patch -> scope map of a few Metro patches; prints both.
#include "test_support.h"

#include "bf3/audio/audio_load.h"
#include "bf3/audio/dsp.h"
#include "bf3/audio/hdr_window.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/voice_cull.h"
#include "bf3/core/audio/hdr_setting.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_scope.h"
#include "bf3/core/audio/sound_wave.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::audio;
namespace cp = bf3::core::audio::port;
namespace ba = bf3::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kStartHash = 1;
constexpr uint32_t kStopHash = 2;
constexpr double kBlockSeconds = double(ba::kBlockFrames) / double(ba::kMixRate);

// ---- a PCM16 stream and a one-segment wave (the audio_unit_tests.cpp format builders) ------------------------------

void put_be24(std::vector<uint8_t> &r, uint32_t v) {
	r.push_back(uint8_t(v >> 16));
	r.push_back(uint8_t(v >> 8));
	r.push_back(uint8_t(v));
}
void put_be32(std::vector<uint8_t> &r, uint32_t v) {
	r.push_back(uint8_t(v >> 24));
	put_be24(r, v & 0xffffff);
}
std::vector<uint8_t> pcm_stream(const std::vector<int16_t> &p_samples, uint32_t p_rate) {
	std::vector<uint8_t> r;
	const uint32_t frames = uint32_t(p_samples.size());
	r.push_back(0x48);
	put_be24(r, 12);
	r.push_back(2);
	r.push_back(uint8_t((p_rate >> 16) & 3));
	r.push_back(uint8_t(p_rate >> 8));
	r.push_back(uint8_t(p_rate));
	put_be32(r, frames);
	r.push_back(0x44);
	put_be24(r, uint32_t(8 + p_samples.size() * 2));
	put_be32(r, frames);
	for (int16_t s : p_samples) {
		r.push_back(uint8_t(uint16_t(s) >> 8));
		r.push_back(uint8_t(uint16_t(s)));
	}
	r.push_back(0x45);
	put_be24(r, 4);
	return r;
}

// A 1 kHz sine of 4,800 frames (100 periods: it loops without a seam), one segment that is its own loop.
struct ToneWave {
	std::shared_ptr<const WaveBank> bank;
	Guid partition;
};
ToneWave tone_wave() {
	SoundWaveInfo w;
	w.name = "a3_tone";
	static const uint8_t kPartition[16] = { 0xA3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	w.partition = Guid::from_bytes(kPartition);
	w.channels = 1;
	std::vector<int16_t> samples(4800);
	for (size_t i = 0; i < samples.size(); ++i) {
		samples[i] = int16_t(std::lrint(16000.0 * std::sin(2.0 * kPi * 1000.0 * double(i) / 48000.0)));
	}
	const std::vector<uint8_t> chunk = pcm_stream(samples, 48000);
	WaveVariation var;
	var.chunk_index = 0;
	var.first_loop_segment = 0;
	var.last_loop_segment = 0;
	WaveSegment seg;
	seg.samples_offset = 0;
	seg.length_seconds = 4800.0f / 48000.0f;
	var.segments.push_back(seg);
	w.chunks.push_back(WaveChunkRef{ Guid{}, uint32_t(chunk.size()) });
	w.variations.push_back(var);
	std::vector<SoundWaveInfo> waves{ w };
	std::vector<std::vector<std::vector<uint8_t>>> bytes{ { chunk } };
	Result<std::shared_ptr<const WaveBank>> bank = WaveBank::from_memory(std::move(waves), std::move(bytes));
	REQUIRE_MESSAGE(bank.ok(), (bank.ok() ? std::string() : bank.error().message));
	return ToneWave{ *bank, w.partition };
}

// ---- synthetic sources ----------------------------------------------------------------------------------------------

struct Source {
	float loudness_db = 71.0f;
	bool enable_hdr = true;
	bool looping = true; // LtForward on the tone: plays until killed; false: the tone once (a 0.1 s one-shot)
	uint8_t id = 1; // the patch partition's last byte (scope maps)
	float slope_db_per_m = 0.0f; // the output's attenuation: slope x distance (a straight Hermite line)
};

// Start event -> Sampler (one slot: SnP1 Rsp0 Pau0 Gai0 Sen0 -> GameMixMain) -> a FlatOutput whose main send is the
// slot voice's Sen0, Gain 0 dB, EnableHdr as given.
std::shared_ptr<const SoundProgram> source_program(const ToneWave &p_wave, const Source &p_src) {
	SoundProgram p;
	p.name = "a3_source";
	uint8_t guid[16] = { 0xA3, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, p_src.id };
	p.partition = Guid::from_bytes(guid);
	p.loudness_db = p_src.loudness_db;
	p.is_looping = p_src.looping;
	p.start_event = kStartHash;
	p.stop_event = kStopHash;
	const uint16_t trig = uint16_t(p.registers++);
	p.inputs.push_back(SoundInput{ kStartHash, trig, 0.0f, true });
	const uint16_t stop = uint16_t(p.registers++);
	p.inputs.push_back(SoundInput{ kStopHash, stop, 0.0f, true });
	p.waves.push_back(SoundWaveRef{ p_wave.partition, "a3_tone" });
	const auto make = [&](SoundNodeType p_type, uint8_t p_option) {
		const SoundNodeTypeInfo &info = sound_node_type_info(p_type);
		SoundNode n;
		n.type = p_type;
		n.option = p_option;
		n.ports_begin = uint32_t(p.ports.size());
		n.ports_count = uint16_t(info.ports.size() + cp::extra_ports(p_type));
		p.ports.resize(p.ports.size() + n.ports_count);
		n.floats_begin = uint32_t(p.floats.size());
		n.floats_count = uint16_t(info.floats.size());
		p.floats.resize(p.floats.size() + n.floats_count, 0.0f);
		return n;
	};
	// The slot voice.
	SoundVoice v;
	v.plugins_begin = uint32_t(p.plugins.size());
	for (const char *id : { "SnP1", "Rsp0", "Pau0", "Gai0", "Sen0" }) {
		SoundVoicePlugin pl;
		pl.id = (uint32_t(uint8_t(id[0])) << 24) | (uint32_t(uint8_t(id[1])) << 16) | (uint32_t(uint8_t(id[2])) << 8) | uint32_t(uint8_t(id[3]));
		pl.channels = 1;
		if (std::strcmp(id, "Sen0") == 0) {
			pl.connection = uint16_t(p.connections.size());
			p.connections.push_back(SoundConnection{ true, 0, MasterBus::GameMixMain });
		}
		p.plugins.push_back(pl);
	}
	v.plugins_count = 5;
	p.voices.push_back(v);
	p.voice_order.push_back(0);
	// The sampler.
	SoundNode s = make(SoundNodeType::Sampler, uint8_t(p_src.looping ? SamplerLoop::Forward : SamplerLoop::None));
	s.waves_begin = uint32_t(p.wave_refs.size());
	s.waves_count = 1;
	p.wave_refs.push_back(0);
	s.plugins_begin = uint32_t(p.plugin_refs.size());
	s.plugins_count = cp::kSamplerSlotPlugins;
	for (uint16_t k = 0; k < 4; ++k) {
		p.plugin_refs.push_back(SoundPluginRef{ 0, k });
	}
	p.floats[s.floats_begin + cp::SmpBasePitch] = 1.0f;
	p.ports[s.ports_begin + cp::SmpAmplitude] = SoundPort{ kNoRegister, 1.0f };
	p.ports[s.ports_begin + cp::SmpPitch] = SoundPort{ kNoRegister, 1.0f };
	p.ports[s.ports_begin + cp::SmpTrigger] = SoundPort{ trig, 0.0f };
	p.nodes.push_back(s);
	// The output.
	SoundNode o = make(SoundNodeType::FlatOutput, 0);
	o.output = 0;
	o.plugins_begin = uint32_t(p.plugin_refs.size());
	o.plugins_count = cp::kOutputPlugins;
	for (uint16_t k = 0; k < cp::kOutputPlugins; ++k) {
		p.plugin_refs.push_back(k == cp::OutMainSend ? SoundPluginRef{ 0, 4 } : SoundPluginRef{});
	}
	p.nodes.push_back(o);
	SoundOutput out;
	out.node = uint32_t(p.nodes.size() - 1);
	out.kind = OutputKind::Flat;
	out.main_send = MasterBus::GameMixMain;
	out.main_send_plugin = SoundPluginRef{ 0, 4 };
	out.enable_hdr = p_src.enable_hdr;
	if (p_src.slope_db_per_m != 0.0f) {
		out.attenuation.points = { { 0.0f, 0.0f, -p_src.slope_db_per_m }, { 1000.0f, -1000.0f * p_src.slope_db_per_m, -p_src.slope_db_per_m } };
	}
	p.outputs.push_back(out);
	REQUIRE_MESSAGE(validate_sound_program(p).ok(), (validate_sound_program(p).ok() ? std::string() : validate_sound_program(p).error().message));
	return std::make_shared<const SoundProgram>(std::move(p));
}

// Records the left channel of every block.
struct Recorder final : ba::MixTap {
	std::vector<float> left;
	void on_block(const float *p, uint32_t n, uint64_t) override {
		for (uint32_t i = 0; i < n; ++i) {
			left.push_back(p[size_t(i) * 2]);
		}
	}
	double rms_db(size_t p_from, size_t p_to) const {
		double sum = 0.0;
		p_to = std::min(p_to, left.size());
		for (size_t i = p_from; i < p_to; ++i) {
			sum += double(left[i]) * double(left[i]);
		}
		return p_to > p_from ? 10.0 * std::log10(std::max(1e-30, sum / double(p_to - p_from))) : -300.0;
	}
};

// A mixer with the sources the case starts; one block per step().
struct Rig {
	ToneWave wave = tone_wave();
	std::unique_ptr<ba::MixerCore> mixer;
	Recorder rec;
	std::vector<float> buf = std::vector<float>(size_t(ba::kBlockFrames) * 2);
	std::vector<ba::MixerFeedback> finished;

	explicit Rig(ba::MixerConfig p_config) {
		mixer = std::make_unique<ba::MixerCore>(p_config);
		mixer->set_tap(&rec);
	}
	~Rig() { mixer.reset(); }
	// Starts p_src at p_distance metres in front of the listener, now (its first block is the next step()).
	ba::InstanceHandle start(const Source &p_src, float p_distance = 1.0f, float p_x = 0.0f) {
		Result<std::unique_ptr<ba::PatchInstance>> inst = ba::PatchInstance::create(source_program(wave, p_src), wave.bank, ba::AudioTuning{}, 7, ba::kMixRate);
		REQUIRE_MESSAGE(inst.ok(), (inst.ok() ? std::string() : inst.error().message));
		ba::EmitterState e;
		e.position = { p_x, 0.0f, p_distance };
		(*inst)->set_geometry(e, ba::ListenerState{});
		ba::MixerCommand c;
		c.type = ba::MixerCommandType::Start;
		c.handle = mixer->next_handle();
		c.instance = inst->get();
		REQUIRE(mixer->push(c));
		(void)inst->release();
		ba::MixerCommand ev;
		ev.type = ba::MixerCommandType::PostEvent;
		ev.handle = c.handle;
		ev.name_hash = kStartHash;
		REQUIRE(mixer->push(ev));
		return c.handle;
	}
	void kill(ba::InstanceHandle p_handle) {
		ba::MixerCommand c;
		c.type = ba::MixerCommandType::Kill;
		c.handle = p_handle;
		REQUIRE(mixer->push(c));
	}
	void step(int p_blocks = 1) {
		for (int i = 0; i < p_blocks; ++i) {
			mixer->mix(buf.data(), ba::kBlockFrames);
			mixer->reclaim();
			std::array<ba::MixerFeedback, 64> fb{};
			const size_t n = mixer->poll_feedback(fb);
			for (size_t k = 0; k < n; ++k) {
				if (fb[k].finished) {
					finished.push_back(fb[k]);
				}
			}
		}
	}
	bool was_finished(ba::InstanceHandle p_handle) const {
		return std::any_of(finished.begin(), finished.end(), [&](const ba::MixerFeedback &f) { return f.handle == p_handle; });
	}
	float top() const { return mixer->stats().window_top_db.load(); }
	float bottom() const { return mixer->stats().window_bottom_db.load(); }
	size_t frames() const { return rec.left.size(); }
};

ba::MixerConfig hdr_config() {
	ba::MixerConfig c;
	c.hdr = retail_hdr_setting();
	return c;
}

ba::CullVoice voice(float p_loudness, float p_distance = 1.0f, bool p_starting = true, uint16_t p_scope = kNoScope, uint64_t p_start = 0, bool p_looping = false) {
	ba::CullVoice v;
	v.loudness_db = p_loudness;
	v.distance_m = p_distance;
	v.position[2] = p_distance;
	v.starting = p_starting;
	v.scope = p_scope;
	v.start_frame = p_start;
	v.looping = p_looping;
	return v;
}

// The two retail scopes E10 names (the install row checks the data has them so).
ScopeTable e10_scopes() {
	ScopeTable t;
	SoundScope impact;
	impact.name = "Bullet_Impact_Scope";
	impact.duplicate_radius_m = 1.0f;
	impact.duplicate_time_s = 0.4f;
	impact.closest = 5;
	SoundScope scream;
	scream.name = "Death_Scream_Scope";
	scream.latest = 4;
	t.scopes = { impact, scream };
	return t;
}

std::vector<ba::CullVerdict> cull(const std::vector<ba::CullVoice> &p_voices, const ScopeTable *p_scopes, float p_precull, uint32_t p_cap, ba::CullCounts &r_counts) {
	std::vector<ba::CullVerdict> out(p_voices.size(), ba::CullVerdict::Keep);
	ba::cull_voices(p_voices, p_scopes, p_precull, p_cap, 0, ba::kMixRate, out, r_counts);
	return out;
}

size_t count(const std::vector<ba::CullVerdict> &p_v, ba::CullVerdict p_what) {
	return size_t(std::count(p_v.begin(), p_v.end(), p_what));
}

} // namespace

// =====================================================================================================================

TEST_SUITE("audio-hdr") {
	TEST_CASE("E10: the HDR window alone: 71 dB -> 80 / 35, 110 dB -> 110 within 0.05 s, 65, 63 dB suspended, the release") {
		ba::HdrWindow w;
		w.configure(retail_hdr_setting(), ba::kMixRate);
		CHECK(w.top_db() == 80.0f);
		CHECK(w.bottom_db() == 35.0f);
		for (int i = 0; i < 50; ++i) {
			w.step(71.0f, ba::kBlockFrames); // a 71 dB footstep alone
		}
		CHECK(w.top_db() == doctest::Approx(80.0f).epsilon(1e-6));
		CHECK(w.bottom_db() == doctest::Approx(35.0f).epsilon(1e-6));
		CHECK(w.gain_db(71.0f) == doctest::Approx(71.0f - 80.0f - 5.0f));
		CHECK_FALSE(w.suspended(71.0f));
		// A 110 dB shot: the top reaches 110 within 0.05 s +- one block.
		int blocks = 0;
		float prev = w.top_db();
		while (w.top_db() < 110.0f - 1e-3f && blocks < 100) {
			w.step(110.0f, ba::kBlockFrames);
			++blocks;
			CHECK(w.top_db() >= prev); // the attack only rises
			CHECK(w.gain_db(110.0f) <= -2.0f + 1e-4f); // overshoot: at most AllowedOvershoot above the top, minus the headroom
			prev = w.top_db();
		}
		const double attack_s = double(blocks) * kBlockSeconds;
		MESSAGE("attack 80 -> 110 dB: " << blocks << " blocks = " << attack_s * 1000.0 << " ms");
		CHECK(attack_s <= 0.05 + kBlockSeconds);
		CHECK(attack_s >= 0.05 - kBlockSeconds);
		CHECK(w.top_db() == doctest::Approx(110.0f));
		CHECK(w.bottom_db() == doctest::Approx(65.0f));
		CHECK(w.suspended(63.0f)); // a 63 dB source is suspended
		CHECK_FALSE(w.suspended(71.0f));
		CHECK(w.gain_db(110.0f) == doctest::Approx(-5.0f));
		CHECK(w.precull_db() == doctest::Approx(62.0f));
		// After the shot: the top releases to 80 over release_seconds(30) on the curve (monotone, never below 80).
		const float duration = w.release_seconds(30.0f);
		CHECK(duration == doctest::Approx(0.035f + (0.85f - 0.035f) * (30.0f / 45.0f)));
		int rel = 0;
		prev = w.top_db();
		while (w.top_db() > 80.0f + 1e-3f && rel < 1000) {
			w.step(71.0f, ba::kBlockFrames);
			++rel;
			const float expect = 80.0f + 30.0f * w.release_shape(float(double(rel) * kBlockSeconds / duration));
			CHECK(w.top_db() == doctest::Approx(expect).epsilon(1e-4));
			CHECK(w.top_db() <= prev);
			CHECK(w.top_db() >= 80.0f);
			CHECK(w.bottom_db() == doctest::Approx(std::max(35.0f, w.top_db() - 45.0f)).epsilon(1e-3));
			prev = w.top_db();
		}
		MESSAGE("release 110 -> 80 dB: " << rel << " blocks = " << double(rel) * kBlockSeconds * 1000.0 << " ms (T = " << duration * 1000.0f << " ms)");
		CHECK(double(rel) * kBlockSeconds <= double(duration) + kBlockSeconds);
		CHECK(w.top_db() == doctest::Approx(80.0f));
		CHECK(w.bottom_db() == doctest::Approx(35.0f));
	}

	TEST_CASE("the release shape: the retail curve rescaled to 1 .. 0, monotone; no curve is linear; a short dip releases fast") {
		ba::HdrWindow w;
		w.configure(retail_hdr_setting(), ba::kMixRate);
		CHECK(w.release_shape(0.0f) == doctest::Approx(1.0f));
		CHECK(w.release_shape(1.0f) == doctest::Approx(0.0f));
		float prev = 2.0f;
		for (int i = 0; i <= 100; ++i) {
			const float y = w.release_shape(float(i) / 100.0f);
			CHECK(y <= prev + 1e-6f);
			CHECK(y >= 0.0f);
			CHECK(y <= 1.0f);
			prev = y;
		}
		CHECK(w.release_shape(std::nanf("")) == doctest::Approx(0.0f)); // hostile x: the end
		ba::HdrWindow lin;
		lin.configure(HdrSetting{}, ba::kMixRate); // no curve
		CHECK(lin.release_shape(0.25f) == doctest::Approx(0.75f));
		CHECK(w.release_seconds(0.0f) == doctest::Approx(0.035f));
		CHECK(w.release_seconds(45.0f) == doctest::Approx(0.85f));
		CHECK(w.release_seconds(1000.0f) == doctest::Approx(0.85f));
		// A NaN loudest is no voice: the top stays at WindowMinTop.
		for (int i = 0; i < 10; ++i) {
			w.step(std::nanf(""), ba::kBlockFrames);
		}
		CHECK(w.top_db() == 80.0f);
		CHECK(w.bottom_db() == 35.0f);
		w.step(-std::numeric_limits<float>::infinity(), ba::kBlockFrames);
		CHECK(w.top_db() == 80.0f);
	}

	TEST_CASE("E10 through MixerCore: the window's numbers, the gain (R5), suspension, pre-cull, pending, the release") {
		Rig rig(hdr_config());
		// A 71 dB footstep alone: top 80, bottom 35.
		const ba::InstanceHandle foot = rig.start(Source{ 71.0f, true, true, 1 });
		rig.step(20);
		CHECK(rig.top() == doctest::Approx(80.0f));
		CHECK(rig.bottom() == doctest::Approx(35.0f));
		CHECK(rig.mixer->stats().suspended.load() == 0);
		const double foot_hdr_db = rig.rec.rms_db(rig.frames() - 10 * ba::kBlockFrames, rig.frames());
		// The same footstep on the fixed window (no HdrSetting): R5 makes it 121 - 85 = 36 dB louder.
		{
			Rig fixed{ ba::MixerConfig{} };
			fixed.start(Source{ 71.0f, true, true, 1 });
			fixed.step(20);
			const double foot_fixed_db = fixed.rec.rms_db(fixed.frames() - 10 * ba::kBlockFrames, fixed.frames());
			MESSAGE("71 dB footstep: " << foot_hdr_db << " dBFS on the HDR window, " << foot_fixed_db << " dBFS on the fixed 115 / 6 window");
			CHECK(foot_hdr_db - foot_fixed_db == doctest::Approx((115.0 + 6.0) - (80.0 + 5.0)).epsilon(0.003));
		}
		// A 110 dB shot starts: the top reaches 110 within 0.05 s (+- one block), the bottom 65.
		const ba::InstanceHandle shot = rig.start(Source{ 110.0f, true, true, 2 });
		int blocks = 0;
		while (rig.top() < 110.0f - 1e-3f && blocks < 50) {
			rig.step();
			++blocks;
		}
		MESSAGE("MixerCore attack 80 -> 110 dB: " << blocks << " blocks = " << double(blocks) * kBlockSeconds * 1000.0 << " ms");
		CHECK(double(blocks) * kBlockSeconds <= 0.05 + kBlockSeconds);
		CHECK(double(blocks) * kBlockSeconds >= 0.05 - kBlockSeconds);
		rig.step(4);
		CHECK(rig.top() == doctest::Approx(110.0f));
		CHECK(rig.bottom() == doctest::Approx(65.0f));
		CHECK(rig.mixer->stats().suspended.load() == 0); // the footstep at 71 is above 65
		// A 63 dB source starts: above the pre-cull edge (62), below the bottom (65): suspended, silent.
		const ba::InstanceHandle quiet = rig.start(Source{ 63.0f, true, true, 3 });
		rig.step(3);
		CHECK(rig.mixer->stats().suspended.load() == 1);
		CHECK_FALSE(rig.was_finished(quiet));
		CHECK(rig.mixer->stats().active_instances.load() == 3); // still advancing
		// A 60 dB one-shot is pre-culled (60 < 65 - 3), a 60 dB loop becomes pending (kept, not rendered).
		const uint64_t pre0 = rig.mixer->stats().culled_precull.load();
		const ba::InstanceHandle drop = rig.start(Source{ 60.0f, true, false, 4 });
		const ba::InstanceHandle park = rig.start(Source{ 60.0f, true, true, 5 });
		rig.step(2);
		CHECK(rig.mixer->stats().culled_precull.load() == pre0 + 1);
		CHECK(rig.was_finished(drop)); // the game thread hears of it (the pool / router free it)
		CHECK_FALSE(rig.was_finished(park));
		CHECK(rig.mixer->stats().active_instances.load() == 3); // the pending loop is not rendered
		// A non-HDR source is never pre-culled or suspended (today's path).
		const ba::InstanceHandle direct = rig.start(Source{ 40.0f, false, false, 6 });
		rig.step(1);
		CHECK_FALSE(rig.was_finished(direct));
		CHECK(rig.mixer->stats().suspended.load() == 1);
		// The shot ends: the top releases on the curve to 80 (the footstep holds 71 < 80); the pending loop resumes
		// once the edge falls below 60 (bottom 35 -> edge 32), the 63 dB source is no longer suspended.
		rig.kill(shot);
		rig.kill(direct);
		const float duration = ba::HdrWindow().release_seconds(30.0f); // the default setting's release (the retail one)
		int rel = 0;
		float prev = rig.top();
		while (rig.top() > 80.0f + 1e-3f && rel < 200) {
			rig.step();
			++rel;
			CHECK(rig.top() <= prev);
			prev = rig.top();
		}
		MESSAGE("MixerCore release 110 -> 80 dB: " << rel << " blocks = " << double(rel) * kBlockSeconds * 1000.0 << " ms");
		CHECK(double(rel) * kBlockSeconds <= double(duration) + 2 * kBlockSeconds);
		rig.step(2);
		CHECK(rig.top() == doctest::Approx(80.0f));
		CHECK(rig.bottom() == doctest::Approx(35.0f));
		CHECK(rig.mixer->stats().suspended.load() == 0);
		CHECK(rig.mixer->stats().active_instances.load() == 3); // footstep, the 63 dB source, the resumed loop
		CHECK(rig.mixer->stats().live_voices_max.load() >= 3);
		rig.kill(foot);
		rig.kill(quiet);
		rig.kill(park);
		rig.step(2);
		CHECK(rig.mixer->stats().active_instances.load() == 0);
	}

	TEST_CASE("R5: a suspended source is silent; the shot's gain is loudness - top - headroom; the attack is clamped by the overshoot") {
		// The shot alone, and the shot with a 63 dB source: after the suspension ramp the output is the same.
		Rig alone(hdr_config());
		alone.start(Source{ 110.0f, true, true, 2 });
		alone.step(10);
		Rig both(hdr_config());
		both.start(Source{ 110.0f, true, true, 2 });
		both.step(8);
		both.start(Source{ 63.0f, true, true, 3 });
		both.step(2);
		REQUIRE(alone.frames() == both.frames());
		double diff = 0.0;
		for (size_t i = alone.frames() - ba::kBlockFrames; i < alone.frames(); ++i) {
			diff = std::max(diff, double(std::fabs(alone.rec.left[i] - both.rec.left[i])));
		}
		CHECK(diff == 0.0);
		// The shot at top 110: -5 dB against a 71 dB footstep at top 80 (-14 dB): 9 dB louder. The windows hold 480
		// frames (ten tone periods), so the RMS does not depend on the phase.
		Rig foot(hdr_config());
		foot.start(Source{ 71.0f, true, true, 1 });
		foot.step(10);
		const size_t last = alone.frames() - ba::kBlockFrames;
		const double shot_db = alone.rec.rms_db(last, last + 480);
		const double foot_db = foot.rec.rms_db(last, last + 480);
		CHECK(shot_db - foot_db == doctest::Approx(9.0).epsilon(0.002));
		// The first block of the shot (top 86.4 after one attack block): the overshoot clamp gives
		// min(110, 86.4 + 3) - 86.4 - 5 = -2 dB, 3 dB above the steady -5 dB (not 110 - 86.4 - 5 = +18.6 dB).
		const double first_db = alone.rec.rms_db(32, 32 + 480);
		MESSAGE("shot: first block " << first_db << " dBFS, steady " << shot_db << " dBFS; footstep " << foot_db << " dBFS");
		CHECK(first_db - shot_db == doctest::Approx(3.0).epsilon(0.01));
	}

	TEST_CASE("the non-HDR path is unchanged: an HdrSetting does not touch a patch without EnableHdr outputs") {
		Rig fixed{ ba::MixerConfig{} };
		Rig hdr(hdr_config());
		for (Rig *r : { &fixed, &hdr }) {
			r->start(Source{ 95.0f, false, true, 1 }, 3.0f);
			r->start(Source{ 80.0f, false, false, 2 }, 9.0f);
			r->step(12);
		}
		REQUIRE(fixed.frames() == hdr.frames());
		CHECK(std::equal(fixed.rec.left.begin(), fixed.rec.left.end(), hdr.rec.left.begin()));
		CHECK(hdr.mixer->stats().culled_precull.load() == 0);
		CHECK(hdr.top() == doctest::Approx(80.0f)); // no HDR voice: the window sits at WindowMinTop
	}

	TEST_CASE("scopes: DuplicateSpawn 1 m / 0.4 s, Bullet_Impact keeps the closest 5, Death_Scream at most 4, loops pending") {
		const ScopeTable t = e10_scopes();
		const uint32_t rate = ba::kMixRate;
		{
			// Two impacts in one block 0.5 m apart: the second is a duplicate; 1.5 m apart: both stay.
			std::vector<ba::CullVoice> v = { voice(88.0f, 5.0f, true, 0, 100), voice(88.0f, 5.5f, true, 0, 120), voice(88.0f, 7.0f, true, 0, 130) };
			ba::CullCounts c;
			const auto out = cull(v, &t, -1000.0f, 128, c);
			CHECK(out[0] == ba::CullVerdict::Keep);
			CHECK(out[1] == ba::CullVerdict::Scope);
			CHECK(out[2] == ba::CullVerdict::Keep);
			CHECK(c.scope == 1);
		}
		{
			// Near an impact that started 0.3 s ago: a duplicate; 0.5 s ago: not.
			std::vector<ba::CullVoice> v = { voice(88.0f, 5.0f, false, 0, 0), voice(88.0f, 5.2f, true, 0, uint64_t(0.3 * rate)) };
			ba::CullCounts c;
			CHECK(cull(v, &t, -1000.0f, 128, c)[1] == ba::CullVerdict::Scope);
			v[1].start_frame = uint64_t(0.5 * rate);
			ba::CullCounts c2;
			CHECK(cull(v, &t, -1000.0f, 128, c2)[1] == ba::CullVerdict::Keep);
		}
		{
			// Eight impacts 2 m apart: the closest 5 stay.
			std::vector<ba::CullVoice> v;
			for (int i = 0; i < 8; ++i) {
				ba::CullVoice x = voice(88.0f, 2.0f + 2.0f * float((i * 5) % 8), true, 0, uint64_t(i));
				v.push_back(x);
			}
			ba::CullCounts c;
			const auto out = cull(v, &t, -1000.0f, 128, c);
			CHECK(count(out, ba::CullVerdict::Keep) == 5);
			CHECK(count(out, ba::CullVerdict::Scope) == 3);
			for (size_t i = 0; i < v.size(); ++i) {
				CHECK((out[i] == ba::CullVerdict::Keep) == (v[i].distance_m <= 10.0f));
			}
		}
		{
			// Six death screams: the newest 4 stay (Max_Instaces_4, KeepOldest false).
			std::vector<ba::CullVoice> v;
			for (int i = 0; i < 6; ++i) {
				v.push_back(voice(95.0f, 10.0f, i >= 4, 1, uint64_t(100 * i)));
			}
			ba::CullCounts c;
			const auto out = cull(v, &t, -1000.0f, 128, c);
			CHECK(count(out, ba::CullVerdict::Keep) == 4);
			CHECK(out[0] == ba::CullVerdict::Scope);
			CHECK(out[1] == ba::CullVerdict::Scope);
			// KeepOldest true (max_instances): the oldest 4 stay, the new ones are refused.
			ScopeTable oldest = t;
			oldest.scopes[1].latest = 0;
			oldest.scopes[1].max_instances = 4;
			ba::CullCounts c2;
			const auto out2 = cull(v, &oldest, -1000.0f, 128, c2);
			CHECK(out2[4] == ba::CullVerdict::Scope);
			CHECK(out2[5] == ba::CullVerdict::Scope);
			CHECK(count(out2, ba::CullVerdict::Keep) == 4);
		}
		{
			// A loop over a scope limit is pending, not dropped; a scope index outside the table is ignored.
			std::vector<ba::CullVoice> v;
			for (int i = 0; i < 6; ++i) {
				v.push_back(voice(95.0f, 10.0f, false, 1, uint64_t(100 * i), true));
			}
			v.push_back(voice(95.0f, 10.0f, true, 77, 0));
			ba::CullCounts c;
			const auto out = cull(v, &t, -1000.0f, 128, c);
			CHECK(count(out, ba::CullVerdict::Pending) == 2);
			CHECK(c.pending == 2);
			CHECK(c.scope == 0);
			CHECK(out[6] == ba::CullVerdict::Keep);
		}
	}

	TEST_CASE("pre-cull and the 128-voice cap: the quietest go, loops pend, ties drop the newer; hostile spans") {
		{
			std::vector<ba::CullVoice> v = { voice(60.0f), voice(60.0f, 1.0f, false), voice(60.0f, 1.0f, true, kNoScope, 0, true), voice(70.0f) };
			v.push_back(voice(50.0f));
			v.back().hdr = false;
			ba::CullCounts c;
			const auto out = cull(v, nullptr, 62.0f, 128, c);
			CHECK(out[0] == ba::CullVerdict::Precull); // a starting one-shot below the edge
			CHECK(out[1] == ba::CullVerdict::Keep); // playing: the window suspends it instead
			CHECK(out[2] == ba::CullVerdict::Pending); // a starting loop
			CHECK(out[3] == ba::CullVerdict::Keep);
			CHECK(out[4] == ba::CullVerdict::Keep); // not an HDR voice
			CHECK(c.precull == 1);
			CHECK(c.pending == 1);
		}
		{
			std::vector<ba::CullVoice> v;
			for (int i = 0; i < 130; ++i) {
				v.push_back(voice(100.0f - 0.1f * float(i), 1.0f, false, kNoScope, uint64_t(i), i == 129));
			}
			v.push_back(voice(10.0f)); // starting, HDR
			v.back().loudness_db = std::nanf(""); // hostile: below any edge, scored lowest, never a crash
			ba::CullCounts c;
			const auto out = cull(v, nullptr, -1000.0f, 128, c);
			CHECK(count(out, ba::CullVerdict::Keep) == 128);
			CHECK(out[129] == ba::CullVerdict::Pending); // the quietest-but-one is a loop
			CHECK(out[128] == ba::CullVerdict::Cap);
			CHECK(out[130] == ba::CullVerdict::Precull); // NaN is below any edge: a starting HDR one-shot goes at step 1
			CHECK(c.cap == 1);
			// Equal loudness: the newer goes.
			std::vector<ba::CullVoice> eq = { voice(80.0f, 1.0f, false, kNoScope, 5), voice(80.0f, 1.0f, false, kNoScope, 1), voice(80.0f, 1.0f, false, kNoScope, 9) };
			ba::CullCounts c2;
			const auto out2 = cull(eq, nullptr, -1000.0f, 2, c2);
			CHECK(out2[2] == ba::CullVerdict::Cap);
			CHECK(out2[0] == ba::CullVerdict::Keep);
		}
		{
			// The cap counts sampler voices: three instances of 50 voices and a 20-voice one, in loudness order. The two
			// loudest fit (100), the third does not (150 > 128), so it and every voice ranked below it go, the 20-voice
			// one too although it alone would fit. 0 voices count as 1; a hostile count is clamped to kMaxCullVoices.
			std::vector<ba::CullVoice> w = { voice(100.0f, 1.0f, false, kNoScope, 1), voice(95.0f, 1.0f, false, kNoScope, 2),
				voice(90.0f, 1.0f, false, kNoScope, 3, true), voice(85.0f, 1.0f, false, kNoScope, 4) };
			w[0].voices = 50;
			w[1].voices = 50;
			w[2].voices = 50;
			w[3].voices = 20;
			ba::CullCounts c;
			const auto out = cull(w, nullptr, -1000.0f, 128, c);
			CHECK(out[0] == ba::CullVerdict::Keep);
			CHECK(out[1] == ba::CullVerdict::Keep);
			CHECK(out[2] == ba::CullVerdict::Pending); // a loop
			CHECK(out[3] == ba::CullVerdict::Cap);
			CHECK(c.cap == 1);
			CHECK(c.pending == 1);
			// Exactly at the cap: nothing goes.
			w[2].voices = 8;
			ba::CullCounts c2;
			CHECK(count(cull(w, nullptr, -1000.0f, 128, c2), ba::CullVerdict::Keep) == 4);
			w[2].voices = 9;
			ba::CullCounts c3;
			CHECK(cull(w, nullptr, -1000.0f, 128, c3)[3] == ba::CullVerdict::Cap);
			std::vector<ba::CullVoice> zero(200, voice(70.0f, 1.0f, false));
			for (ba::CullVoice &z : zero) {
				z.voices = 0;
			}
			zero[0].voices = 0xFFFFFFFFu;
			zero[0].loudness_db = 10.0f; // the quietest: clamped to 1024, culled first
			ba::CullCounts c4;
			const auto out4 = cull(zero, nullptr, -1000.0f, 128, c4);
			CHECK(out4[0] == ba::CullVerdict::Cap);
			CHECK(count(out4, ba::CullVerdict::Keep) == 128);
		}
		{
			// A span above kMaxCullVoices, or a verdict span of another size, is not culled.
			std::vector<ba::CullVoice> big(ba::kMaxCullVoices + 1, voice(10.0f));
			ba::CullCounts c;
			const auto out = cull(big, nullptr, 62.0f, 4, c);
			CHECK(count(out, ba::CullVerdict::Keep) == big.size());
			CHECK(c.precull + c.cap + c.scope + c.pending == 0);
			std::vector<ba::CullVerdict> short_out(2, ba::CullVerdict::Cap);
			std::vector<ba::CullVoice> three(3, voice(10.0f));
			ba::cull_voices(three, nullptr, 62.0f, 1, 0, ba::kMixRate, short_out, c);
			CHECK(short_out[0] == ba::CullVerdict::Keep);
			CHECK(short_out[1] == ba::CullVerdict::Keep);
		}
	}

	TEST_CASE("scopes and the cap through MixerCore: dropped one-shots come back finished, pending loops resume") {
		{
			ba::MixerConfig cfg = hdr_config();
			auto scopes = std::make_shared<ScopeTable>(e10_scopes());
			// The impact patches (ids 10..17) map to Bullet_Impact_Scope.
			for (uint8_t id = 10; id < 18; ++id) {
				uint8_t g[16] = { 0xA3, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, id };
				scopes->patch_scopes.emplace_back(Guid::from_bytes(g), uint16_t(0));
			}
			std::sort(scopes->patch_scopes.begin(), scopes->patch_scopes.end());
			cfg.scopes = scopes;
			Rig rig(cfg);
			std::vector<ba::InstanceHandle> h;
			for (uint8_t i = 0; i < 8; ++i) {
				h.push_back(rig.start(Source{ 88.0f, true, false, uint8_t(10 + i) }, 2.0f + 2.0f * float(i)));
			}
			rig.step(1);
			CHECK(rig.mixer->stats().culled_scope.load() == 3);
			CHECK(rig.mixer->stats().active_instances.load() == 5);
			for (size_t i = 0; i < h.size(); ++i) {
				CHECK(rig.was_finished(h[i]) == (i >= 5));
			}
		}
		{
			ba::MixerConfig cfg;
			cfg.max_voices = 4;
			cfg.max_instances = 8;
			Rig rig(cfg);
			std::vector<ba::InstanceHandle> loops;
			for (int i = 0; i < 6; ++i) {
				Source s{ 90.0f, false, true, uint8_t(20 + i), 1.0f };
				loops.push_back(rig.start(s, 1.0f + 5.0f * float(i))); // farther = quieter at the listener
			}
			rig.step(2);
			CHECK(rig.mixer->stats().active_instances.load() == 4);
			CHECK(rig.mixer->stats().live_voices_max.load() == 4);
			CHECK(rig.mixer->stats().culled_cap.load() == 0); // loops pend, they are not dropped
			CHECK(rig.finished.empty());
			rig.kill(loops[0]);
			rig.step(1);
			CHECK(rig.mixer->stats().active_instances.load() == 4); // the loudest pending one resumed
			// A one-shot over the cap is dropped (Cap) and comes back finished.
			Source quiet{ 90.0f, false, false, 30, 1.0f };
			const ba::InstanceHandle q = rig.start(quiet, 200.0f);
			rig.step(1);
			CHECK(rig.mixer->stats().culled_cap.load() == 1);
			CHECK(rig.was_finished(q));
			// Stopping a pending loop drops it.
			ba::MixerCommand stop;
			stop.type = ba::MixerCommandType::Stop;
			stop.handle = loops[5];
			REQUIRE(rig.mixer->push(stop));
			rig.step(1);
			CHECK(rig.was_finished(loops[5]));
		}
	}

	TEST_CASE("the profiler: one histogram entry per block, quantiles from the edges, the readout") {
		ba::MixerStats s;
		CHECK(ba::mix_share_quantile(s, 0.99) == 0.0);
		const uint64_t block_ns = uint64_t(ba::kBlockFrames) * 1000000000ull / ba::kMixRate;
		for (int i = 0; i < 98; ++i) {
			ba::record_mix_ns(s, block_ns / 200, block_ns); // 0.5 %
		}
		ba::record_mix_ns(s, block_ns / 11, block_ns); // 9.1 %
		ba::record_mix_ns(s, block_ns * 2, block_ns); // late
		CHECK(s.mix_ns_hist[0].load() == 98);
		CHECK(s.mix_ns_hist[7].load() == 1); // [0.08, 0.10)
		CHECK(s.mix_ns_hist[15].load() == 1);
		CHECK(ba::mix_share_quantile(s, 0.50) == doctest::Approx(0.01));
		CHECK(ba::mix_share_quantile(s, 0.99) == doctest::Approx(0.10));
		CHECK(std::isinf(ba::mix_share_quantile(s, 1.0)));
		CHECK(s.mix_ns_max.load() == block_ns * 2);
		CHECK(s.mix_ns_last.load() == block_ns * 2);
		Rig rig(hdr_config());
		rig.start(Source{ 71.0f, true, true, 1 });
		rig.step(30);
		const ba::MixerStats &ms = rig.mixer->stats();
		uint64_t sum = 0;
		for (const auto &h : ms.mix_ns_hist) {
			sum += h.load();
		}
		CHECK(sum == ms.blocks.load());
		const ba::MixerProfile p = ba::read_mixer_profile(ms);
		CHECK(p.blocks == 30);
		CHECK(p.live_instances == 1);
		CHECK(p.live_voices_max == 1);
		CHECK(p.window_top_db == doctest::Approx(80.0f));
		const std::string text = ba::format_mixer_profile(p);
		MESSAGE(text);
		CHECK(text.find("audio A3") == 0);
	}

	TEST_CASE("hostile HdrSettings are refused; the retail defaults validate") {
		CHECK(validate_hdr_setting(retail_hdr_setting()).ok());
		CHECK(validate_hdr_setting(HdrSetting{}).ok());
		HdrSetting h = retail_hdr_setting();
		h.window_size_db = 0.0f;
		CHECK_FALSE(validate_hdr_setting(h).ok());
		h = retail_hdr_setting();
		h.window_min_top_db = std::nanf("");
		CHECK_FALSE(validate_hdr_setting(h).ok());
		h = retail_hdr_setting();
		h.top_min_release_s = 2.0f;
		CHECK_FALSE(validate_hdr_setting(h).ok());
		h = retail_hdr_setting();
		h.bottom_release_s = -1.0f;
		CHECK_FALSE(validate_hdr_setting(h).ok());
		h = retail_hdr_setting();
		std::swap(h.top_release_curve.points[0], h.top_release_curve.points[3]);
		CHECK_FALSE(validate_hdr_setting(h).ok());
		h = retail_hdr_setting();
		h.top_release_curve.points.assign(kMaxCurvePoints + 1, AudioCurvePoint{});
		CHECK_FALSE(validate_hdr_setting(h).ok());
		h = retail_hdr_setting();
		h.top_release_curve.points[2].k = std::numeric_limits<float>::infinity();
		CHECK_FALSE(validate_hdr_setting(h).ok());
		// A window over a zero-time attack and release still steps (instant moves).
		HdrSetting instant = retail_hdr_setting();
		instant.top_attack_s = 0.0f;
		instant.top_min_release_s = instant.top_max_release_s = 0.0f;
		instant.bottom_release_s = 0.0f;
		ba::HdrWindow w;
		w.configure(instant, ba::kMixRate);
		w.step(120.0f, ba::kBlockFrames);
		CHECK(w.top_db() == 120.0f);
		CHECK(w.bottom_db() == 75.0f);
		w.step(0.0f, ba::kBlockFrames);
		CHECK(w.top_db() == 80.0f);
		CHECK(w.bottom_db() == 35.0f);
	}
}

// =====================================================================================================================

namespace {

struct InstallFixture {
	InstallReader reader;
	std::optional<InstallSnapshot> before;
	std::shared_ptr<Install> install;
	std::unique_ptr<EbxLibrary> subway;
};

InstallFixture *install_fixture() {
	static std::unique_ptr<InstallFixture> f;
	static bool tried = false;
	if (tried) {
		return f.get();
	}
	tried = true;
	auto made = std::make_unique<InstallFixture>();
	if (!made->reader.locate({})) {
		std::fprintf(stderr, "audio-hdr-install: %s\n", made->reader.describe().c_str());
		return nullptr;
	}
	made->before = made->reader.snapshot();
	Result<std::shared_ptr<Install>> install = Install::open(made->reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
	made->install = *install;
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(made->install, { "Win32/Levels/MP_Subway/MP_Subway" });
	REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
	made->subway = std::move(*lib);
	f = std::move(made);
	return f.get();
}

InstallFixture *need_install() {
	InstallFixture *f = install_fixture();
	if (f == nullptr) {
		bf3::test::skip_install_case("no BF3 install found");
	}
	return f;
}

const SoundScope *scope_named(const ScopeTable &p_t, std::string_view p_name) {
	for (const SoundScope &s : p_t.scopes) {
		if (s.name == p_name) {
			return &s;
		}
	}
	return nullptr;
}

} // namespace

TEST_SUITE("audio-hdr-install") {
	TEST_CASE("the retail HdrSetting (sound/soundstates on MP_Subway)") {
		InstallFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		gameplay::WorkBudget budget;
		Result<HdrSetting> h = read_hdr_setting(*f->subway, &budget);
		REQUIRE_MESSAGE(h.ok(), (h.ok() ? std::string() : h.error().message));
		char line[400];
		std::snprintf(line, sizeof(line),
				"HdrSetting: size %.3f, min top %.3f, min bottom %.3f, top attack %.3f, release %.3f .. %.3f (%zu curve points), bottom %.3f / %.3f, headroom "
				"%.3f, overshoot %.3f, compress %.3f, discharge %.3f, max energy %.3f; %zu budget units",
				h->window_size_db, h->window_min_top_db, h->window_min_bottom_db, h->top_attack_s, h->top_min_release_s, h->top_max_release_s,
				h->top_release_curve.points.size(), h->bottom_attack_s, h->bottom_release_s, h->headroom_db, h->allowed_overshoot_db, h->compress_factor,
				h->discharge_factor, h->max_allowed_energy, budget.used());
		MESSAGE(line);
		const HdrSetting d = retail_hdr_setting();
		CHECK(h->window_size_db == d.window_size_db);
		CHECK(h->window_min_top_db == d.window_min_top_db);
		CHECK(h->window_min_bottom_db == d.window_min_bottom_db);
		CHECK(h->top_attack_s == d.top_attack_s);
		CHECK(h->top_min_release_s == d.top_min_release_s);
		CHECK(h->top_max_release_s == d.top_max_release_s);
		CHECK(h->bottom_attack_s == d.bottom_attack_s);
		CHECK(h->bottom_release_s == d.bottom_release_s);
		CHECK(h->headroom_db == d.headroom_db);
		CHECK(h->allowed_overshoot_db == d.allowed_overshoot_db);
		CHECK(h->compress_factor == d.compress_factor);
		CHECK(h->discharge_factor == d.discharge_factor);
		CHECK(h->max_allowed_energy == d.max_allowed_energy);
		REQUIRE(h->top_release_curve.points.size() == d.top_release_curve.points.size());
		for (size_t i = 0; i < d.top_release_curve.points.size(); ++i) {
			CHECK(h->top_release_curve.points[i].x == d.top_release_curve.points[i].x);
			CHECK(h->top_release_curve.points[i].y == d.top_release_curve.points[i].y);
			CHECK(h->top_release_curve.points[i].k == d.top_release_curve.points[i].k);
		}
		// A budget that cannot pay for the read fails, never a partial setting.
		gameplay::WorkBudget tiny(3);
		CHECK_FALSE(read_hdr_setting(*f->subway, &tiny).ok());
		const SnapshotDiff diff = diff_snapshots(*f->before, f->reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("the 33 retail scopes and the patch -> scope map of Metro patches") {
		InstallFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		gameplay::WorkBudget budget;
		Result<ScopeTable> t = read_scopes(*f->subway, &budget);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		std::string table = "scopes (" + std::to_string(t->scopes.size()) + ", " + std::to_string(budget.used()) + " budget units):\n";
		for (const SoundScope &s : t->scopes) {
			char line[300];
			std::snprintf(line, sizeof(line), "  %-32s %-30s closest %u latest %u max %u duplicate %.2f m / %.2f s%s\n", s.name.c_str(), s.strategy.c_str(),
					unsigned(s.closest), unsigned(s.latest), unsigned(s.max_instances), double(s.duplicate_radius_m), double(s.duplicate_time_s),
					s.group_types ? " (GroupTypes)" : "");
			table += line;
		}
		MESSAGE(table);
		CHECK(t->scopes.size() == 33);
		const SoundScope *impact = scope_named(*t, "Bullet_Impact_Scope");
		REQUIRE(impact != nullptr);
		CHECK(impact->duplicate_radius_m == doctest::Approx(1.0f));
		CHECK(impact->duplicate_time_s == doctest::Approx(0.4f));
		CHECK(impact->closest == 5);
		const SoundScope *expl = scope_named(*t, "Explosion_Scope");
		REQUIRE(expl != nullptr);
		CHECK(expl->closest == 10);
		const SoundScope *scream = scope_named(*t, "Death_Scream_Scope");
		REQUIRE(scream != nullptr);
		CHECK(scream->latest == 4);
		CHECK(scream->max_instances == 0);
		const SoundScope *ui = scope_named(*t, "UI_Scope");
		REQUIRE(ui != nullptr);
		CHECK(ui->latest == 1);
		// The map: an impact patch names Bullet_Impact_Scope, the M16A3 names none.
		const char *kImpact = "sound/bullet_craft/bullet_impacts/bullet_impact_concrete";
		const char *kM16 = "sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02";
		std::vector<Guid> patches;
		for (const char *name : { kImpact, kM16 }) {
			const EbxPartition *p = f->subway->partition_by_name(name);
			REQUIRE_MESSAGE(p != nullptr, name);
			patches.push_back(p->guid());
		}
		patches.push_back(Guid{}); // absent: skipped, counted
		size_t skipped = 0;
		REQUIRE(map_patch_scopes(*f->subway, patches, *t, nullptr, &skipped).ok());
		CHECK(skipped == 1);
		REQUIRE(t->patch_scopes.size() == 1);
		const uint16_t s = t->scope_of(patches[0]);
		REQUIRE(s != kNoScope);
		CHECK(t->scopes[s].name == "Bullet_Impact_Scope");
		CHECK(t->scope_of(patches[1]) == kNoScope);
		// load_mixer_a3 over the compiled programs: both parts read, one program scoped.
		std::vector<std::shared_ptr<const SoundProgram>> programs;
		for (const char *name : { kImpact, kM16 }) {
			Result<std::shared_ptr<const SoundProgram>> p = ba::compile_patch(*f->subway, name);
			REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
			programs.push_back(*p);
		}
		const ba::MixerA3Assets a3 = ba::load_mixer_a3(*f->subway, programs);
		CHECK_MESSAGE(a3.notes.empty(), a3.notes);
		CHECK(a3.hdr.has_value());
		REQUIRE(a3.scopes != nullptr);
		CHECK(a3.scoped_patches == 1);
		MESSAGE("M16A3 loudness " << programs[1]->loudness_db << " dB, impact " << programs[0]->loudness_db << " dB; HDR outputs "
								  << std::count_if(programs[1]->outputs.begin(), programs[1]->outputs.end(), [](const SoundOutput &o) { return o.enable_hdr; }) << " of "
								  << programs[1]->outputs.size() << " (M16A3), is_looping " << programs[1]->is_looping);
		// The M16A3's outputs: what the cull's loudness reads at distance (VoiceCullInfo: Loudness + curve + Gain).
		std::string outs = "M16A3 outputs (kind, EnableHdr, Gain, MinDistance; curve + Gain at 10 / 100 / 300 / 900 m):\n";
		for (const SoundOutput &o : programs[1]->outputs) {
			char line[200];
			std::snprintf(line, sizeof(line), "  %d %d %.1f %.1f; %.1f / %.1f / %.1f / %.1f dB (%zu points)\n", int(o.kind), int(o.enable_hdr), double(o.gain_db),
					double(o.min_distance), double(o.attenuation.evaluate(10.0f) + o.gain_db), double(o.attenuation.evaluate(100.0f) + o.gain_db),
					double(o.attenuation.evaluate(300.0f) + o.gain_db), double(o.attenuation.evaluate(900.0f) + o.gain_db), o.attenuation.points.size());
			outs += line;
		}
		MESSAGE(outs);
		ba::MixerConfig mc;
		ba::apply_mixer_a3(a3, mc);
		CHECK(mc.hdr.has_value());
		CHECK(mc.scopes == a3.scopes);
		gameplay::WorkBudget tiny(5);
		CHECK_FALSE(read_scopes(*f->subway, &tiny).ok());
		const SnapshotDiff diff = diff_snapshots(*f->before, f->reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}

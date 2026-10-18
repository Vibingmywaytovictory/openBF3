// M2a au3 (docs/design/m2a-au3.md): the game's weapon audio router, impact audio and in-game check.
//   audio-router  (audio.router.unit, no data) synthetic SoundPrograms through WeaponAudioRouter, ImpactAudio and
//                 SimMixerClock on an offline MixerCore: own shots on the Predicted timeline, others' bursts on the
//                 Render timeline by distance, reloads and one-shots, impacts and effect sounds, the voice limit and
//                 its stealing rules, the listener re-send, determinism, hostile input, the check's bookkeeping.
//   audio-game    (install rows, exit 77 without an install; install.unchanged in each) the Metro kit items through
//                 load_weapon_audio, the offline E7 rehearsal (a cl-faithful presentation stream of an M16A3 shooter
//                 into the router, GameAudioCheck on the mixer, analyse_game_check), and the 64-shooter 60 s render.
// The program builder and the test wave are copies of audio_unit_tests.cpp's (au's file is not edited).
#include "test_support.h"

#include "bf3/audio/audio_check.h"
#include "bf3/audio/game_audio_check.h"
#include "bf3/audio/impact_audio.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::audio;
namespace cp = bf3::core::audio::port;
namespace ba = bf3::audio;
namespace host = bf3::host;
namespace sw = bf3::sim::weapons;
using ba::SamplerTraceEntry;
using ba::SamplerTraceKind;

namespace {

// ---- stream and program builders (copied from audio_unit_tests.cpp) ----------------------------------------------

void put_be24(std::vector<uint8_t> &r, uint32_t v) {
	r.push_back(uint8_t(v >> 16));
	r.push_back(uint8_t(v >> 8));
	r.push_back(uint8_t(v));
}
void put_be32(std::vector<uint8_t> &r, uint32_t v) {
	r.push_back(uint8_t(v >> 24));
	put_be24(r, v & 0xffffff);
}
void header_block(std::vector<uint8_t> &r, uint8_t p_codec, uint32_t p_channels, uint32_t p_rate, uint32_t p_frames) {
	r.push_back(0x48);
	put_be24(r, 12);
	r.push_back(p_codec);
	r.push_back(uint8_t(((p_channels - 1) << 2) | ((p_rate >> 16) & 3)));
	r.push_back(uint8_t(p_rate >> 8));
	r.push_back(uint8_t(p_rate));
	put_be32(r, p_frames);
}
std::vector<uint8_t> pcm_stream(const std::vector<int16_t> &p_samples, uint32_t p_channels, uint32_t p_rate) {
	std::vector<uint8_t> r;
	const uint32_t frames = uint32_t(p_samples.size() / p_channels);
	header_block(r, 2, p_channels, p_rate, frames);
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

struct Builder {
	SoundProgram p;
	Builder() {
		p.name = "test";
	}
	uint16_t reg() {
		return uint16_t(p.registers++);
	}
	uint16_t input(uint32_t p_hash, bool p_event, float p_default = 0.0f) {
		const uint16_t r = reg();
		p.inputs.push_back(SoundInput{ p_hash, r, p_default, p_event });
		return r;
	}
	static SoundPort R(uint16_t p_reg) {
		return SoundPort{ p_reg, 0.0f };
	}
	static SoundPort C(float p_value) {
		return SoundPort{ kNoRegister, p_value };
	}
	SoundNode make(SoundNodeType p_type, uint8_t p_option, uint8_t p_flags) {
		const SoundNodeTypeInfo &info = sound_node_type_info(p_type);
		SoundNode n;
		n.type = p_type;
		n.option = p_option;
		n.flags = p_flags;
		n.ports_begin = uint32_t(p.ports.size());
		n.ports_count = uint16_t(info.ports.size() + cp::extra_ports(p_type));
		p.ports.resize(p.ports.size() + n.ports_count);
		n.floats_begin = uint32_t(p.floats.size());
		n.floats_count = uint16_t(info.floats.size());
		p.floats.resize(p.floats.size() + n.floats_count, 0.0f);
		return n;
	}
	uint32_t node(SoundNodeType p_type, uint8_t p_option = 0, uint8_t p_flags = 0) {
		SoundNode n = make(p_type, p_option, p_flags);
		n.children_begin = uint32_t(p.children.size());
		n.children_count = 0;
		p.nodes.push_back(n);
		return uint32_t(p.nodes.size() - 1);
	}
	void set(uint32_t p_node, uint32_t p_port, SoundPort p_value) {
		p.ports[p.nodes[p_node].ports_begin + p_port] = p_value;
	}
	void setf(uint32_t p_node, uint32_t p_index, float p_value) {
		p.floats[p.nodes[p_node].floats_begin + p_index] = p_value;
	}
	uint16_t sampler_voice() {
		SoundVoice v;
		v.stage = 0;
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
		p.voice_order.push_back(uint16_t(p.voices.size() - 1));
		return uint16_t(p.voices.size() - 1);
	}
	uint32_t sampler(uint32_t p_slots, uint16_t p_wave, SamplerLoop p_loop, uint8_t p_flags = 0) {
		SoundNode n = make(SoundNodeType::Sampler, uint8_t(p_loop), p_flags);
		n.waves_begin = uint32_t(p.wave_refs.size());
		n.waves_count = 1;
		p.wave_refs.push_back(p_wave);
		n.plugins_begin = uint32_t(p.plugin_refs.size());
		n.plugins_count = uint16_t(p_slots * cp::kSamplerSlotPlugins);
		for (uint32_t s = 0; s < p_slots; ++s) {
			const uint16_t v = sampler_voice();
			for (uint16_t k = 0; k < 4; ++k) {
				p.plugin_refs.push_back(SoundPluginRef{ v, k });
			}
		}
		p.nodes.push_back(n);
		const uint32_t idx = uint32_t(p.nodes.size() - 1);
		setf(idx, cp::SmpBasePitch, 1.0f);
		set(idx, cp::SmpAmplitude, C(1.0f));
		set(idx, cp::SmpPitch, C(1.0f));
		return idx;
	}
	std::shared_ptr<const SoundProgram> build() {
		REQUIRE_MESSAGE(validate_sound_program(p).ok(), (validate_sound_program(p).ok() ? std::string() : validate_sound_program(p).error().message));
		return std::make_shared<SoundProgram>(p);
	}
};

struct TestWave {
	std::shared_ptr<const WaveBank> bank;
	Guid partition;
};
// p_segments PCM segments of p_frames frames, segment s an impulse of 1000 (s + 1) at frame 0.
TestWave make_wave(uint32_t p_segments, uint32_t p_frames, int16_t p_first_loop, int16_t p_last_loop) {
	SoundWaveInfo w;
	w.name = "test_wave";
	static const uint8_t kPartition[16] = { 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 };
	w.partition = Guid::from_bytes(kPartition);
	w.channels = 1;
	std::vector<uint8_t> chunk;
	WaveVariation var;
	var.chunk_index = 0;
	var.first_loop_segment = p_first_loop;
	var.last_loop_segment = p_last_loop;
	for (uint32_t s = 0; s < p_segments; ++s) {
		std::vector<int16_t> samples(p_frames, 0);
		samples[0] = int16_t(1000 * (s + 1));
		WaveSegment seg;
		seg.samples_offset = uint32_t(chunk.size());
		seg.length_seconds = float(p_frames) / 48000.0f;
		const std::vector<uint8_t> st = pcm_stream(samples, 1, 48000);
		chunk.insert(chunk.end(), st.begin(), st.end());
		var.segments.push_back(seg);
	}
	w.chunks.push_back(WaveChunkRef{ Guid{}, uint32_t(chunk.size()) });
	w.variations.push_back(var);
	std::vector<SoundWaveInfo> waves{ w };
	std::vector<std::vector<std::vector<uint8_t>>> bytes{ { chunk } };
	Result<std::shared_ptr<const WaveBank>> bank = WaveBank::from_memory(std::move(waves), std::move(bytes));
	REQUIRE_MESSAGE(bank.ok(), (bank.ok() ? std::string() : bank.error().message));
	return TestWave{ *bank, w.partition };
}

// ---- the synthetic weapon ----------------------------------------------------------------------------------------

constexpr uint32_t kStepHash = 0x51e90001u;
constexpr uint32_t kStopHash = 0x57090001u;
constexpr uint32_t kEnableStepHash = 0xe5ab1e01u;
constexpr uint32_t kImpactStart = 0x1a9ac701u;
constexpr uint32_t kRate = 48000;
constexpr uint32_t kFramesPerTick = 800; // 48 kHz / 60 Hz: one display frame of the rig is one tick
constexpr sw::SimTime kTick = sw::kTimeUnitsPerTick;
constexpr int64_t kBias = 2048; // the lead (2 blocks) + 2 blocks of margin
constexpr uint32_t kIntervalFrames = 3600; // 0.075 s
constexpr sw::SimTime kRenderDelay = 6 * kTick; // cl's interpolation delay in the rig: RemoteShots arrive this early

// Fire starts a 2-slot stepped Sampler (Step steps it, the stop event releases it into its tail); Reload starts a
// second sampler; a Physics node writes the listener distance to dist.
struct WeaponProgram {
	std::shared_ptr<const SoundProgram> program;
	uint16_t dist = 0, first_person = 0, enable_step = 0;
};
WeaponProgram weapon_program(const TestWave &w) {
	Builder b;
	WeaponProgram out;
	const uint16_t fire = b.input(ba::kHashFire, true), step = b.input(kStepHash, true), stop = b.input(kStopHash, true);
	out.enable_step = b.input(kEnableStepHash, false, 0.0f);
	out.first_person = b.input(ba::kHashIsFirstPerson, false, 0.0f);
	const uint16_t reload = b.input(ba::kHashReload, true);
	b.p.start_event = ba::kHashFire;
	b.p.stop_event = kStopHash;
	b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
	const uint32_t s = b.sampler(2, 0, SamplerLoop::Release);
	b.set(s, cp::SmpTrigger, Builder::R(fire));
	b.set(s, cp::SmpStep, Builder::R(step));
	b.set(s, cp::SmpRelease, Builder::R(stop));
	b.set(s, cp::SmpEnableStep, Builder::R(out.enable_step));
	const uint32_t r = b.sampler(1, 0, SamplerLoop::None);
	b.set(r, cp::SmpTrigger, Builder::R(reload));
	const uint32_t phys = b.node(SoundNodeType::Physics);
	out.dist = b.reg();
	b.set(phys, cp::PhysDistance, Builder::R(out.dist));
	out.program = b.build();
	return out;
}

// A one-shot: p_event starts one sampler (start_event = p_event when p_as_start).
std::shared_ptr<const SoundProgram> one_shot_program(const TestWave &w, uint32_t p_event, bool p_as_start) {
	Builder b;
	const uint16_t trig = b.input(p_event, true);
	b.input(kStopHash, true);
	if (p_as_start) {
		b.p.start_event = p_event;
	}
	b.p.stop_event = kStopHash;
	b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
	const uint32_t s = b.sampler(1, 0, SamplerLoop::None);
	b.set(s, cp::SmpTrigger, Builder::R(trig));
	return b.build();
}

enum Def : uint16_t { kAuto = 0, kSingle = 1, kKnife = 2, kNone = 3, kGrenade = 4, kAuto2 = 5 };

std::shared_ptr<ba::WeaponAudioTable> make_table(const TestWave &w, const WeaponProgram &p_weapon) {
	auto t = std::make_shared<ba::WeaponAudioTable>();
	t->defs.resize(6);
	const auto driver = [&](ba::WeaponAudioDef &d, ba::WeaponFireMode p_mode, double p_interval) {
		d.form = ba::WeaponAudioForm::Driver;
		d.patch = "test/weapon";
		d.program = p_weapon.program;
		d.fire_mode = p_mode;
		d.shot_interval_s = p_interval;
		d.has_reload = true;
		Result<ba::WeaponSoundBinding> b = ba::bind_weapon_sound(p_weapon.program, p_interval, kRate);
		REQUIRE(b.ok());
		d.binding = *b;
	};
	driver(t->defs[kAuto], ba::WeaponFireMode::Automatic, 0.075);
	driver(t->defs[kSingle], ba::WeaponFireMode::Single, 0.2);
	driver(t->defs[kAuto2], ba::WeaponFireMode::Automatic, 0.075);
	ba::WeaponAudioDef &knife = t->defs[kKnife];
	knife.form = ba::WeaponAudioForm::OneShot;
	knife.program = one_shot_program(w, ba::kHashMelee, false);
	knife.one_shot_event = ba::one_shot_event_of(*knife.program);
	ba::WeaponAudioDef &gren = t->defs[kGrenade];
	gren.form = ba::WeaponAudioForm::OneShot;
	gren.program = one_shot_program(w, ba::kHashFire, false);
	gren.one_shot_event = ba::one_shot_event_of(*gren.program);
	gren.explosion = one_shot_program(w, kImpactStart, true);
	t->defs[kNone].error = "no sound partition";
	t->compiled = 5;
	t->failed = 0;
	return t;
}

// ---- the rig: router + pool + impacts on an offline mixer, one display frame = one tick = 800 frames ----------------

struct Seen {
	uint8_t slot = 0;
	uint64_t seed = 0;
	uint64_t first = 0;
	uint64_t stop = 0;
	bool up = false;
	float distance = -1.0f, first_person = -1.0f, enable_step = -1.0f;
	ba::SamplerTrace trace;
};

// Sees every driver instance before its Start: a trace on it, and one control evaluation (no audio, no events) to read
// the distance and the parameters the router set.
struct Recorder final : ba::WeaponAudioObserver {
	WeaponProgram weapon;
	std::deque<Seen> seen;
	void on_driver_instance(uint8_t p_slot, uint64_t p_seed, uint64_t p_first, ba::PatchInstance &r_instance) override {
		Seen &s = seen.emplace_back();
		s.slot = p_slot;
		s.seed = p_seed;
		s.first = p_first;
		s.trace.entries.resize(512);
		r_instance.set_sampler_trace(&s.trace);
		r_instance.evaluate(0.0);
		s.distance = r_instance.value(weapon.dist);
		s.first_person = r_instance.value(weapon.first_person);
		s.enable_step = r_instance.value(weapon.enable_step);
	}
	void on_trigger_up(uint8_t p_slot, uint64_t p_seed, uint64_t p_stop) override {
		for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
			if (it->slot == p_slot && it->seed == p_seed && !it->up) {
				it->up = true;
				it->stop = p_stop;
				return;
			}
		}
		FAIL("trigger up without an instance");
	}
	size_t of(uint8_t p_slot) const {
		return size_t(std::count_if(seen.begin(), seen.end(), [&](const Seen &s) { return s.slot == p_slot; }));
	}
};

struct Capture final : ba::MixTap {
	std::vector<float> samples;
	void on_block(const float *p, uint32_t p_frames, uint64_t) override {
		samples.insert(samples.end(), p, p + size_t(p_frames) * 2);
	}
};

std::vector<SamplerTraceEntry> entries(const ba::SamplerTrace &p_trace) {
	return std::vector<SamplerTraceEntry>(p_trace.entries.begin(), p_trace.entries.begin() + std::min<size_t>(p_trace.count, p_trace.entries.size()));
}
std::vector<uint64_t> frames_of(const ba::SamplerTrace &p_trace, SamplerTraceKind p_kind) {
	std::vector<uint64_t> out;
	for (const SamplerTraceEntry &e : entries(p_trace)) {
		if (e.kind == p_kind) {
			out.push_back(e.frame);
		}
	}
	return out;
}

struct Rig {
	TestWave wave = make_wave(5, 6000, 1, 3); // segments longer than a shot interval: a Step lands while one plays
	WeaponProgram weapon = weapon_program(wave);
	std::shared_ptr<ba::WeaponAudioTable> table = make_table(wave, weapon);
	Recorder recorder;
	Capture capture;
	std::unique_ptr<ba::MixerCore> mixer;
	std::unique_ptr<ba::OneShotPool> pool;
	std::unique_ptr<ba::SimMixerClock> clock;
	std::unique_ptr<ba::WeaponAudioRouter> router;
	std::unique_ptr<ba::ImpactAudio> impacts;
	ba::ListenerState listener;
	sw::SimTime predicted = 1000 * kTick, render = 994 * kTick;
	uint64_t clock_at_events = 0;
	std::vector<float> buffer = std::vector<float>(size_t(kFramesPerTick) * 2);

	explicit Rig(ba::WeaponAudioConfig p_config = {}, uint32_t p_pool = ba::kMaxOneShotVoices) {
		recorder.weapon = weapon;
		ba::MixerConfig mc;
		mc.max_instances = ba::kGameMixerInstances;
		mc.command_capacity = 8192;
		mixer = std::make_unique<ba::MixerCore>(mc);
		mixer->set_tap(&capture);
		pool = std::make_unique<ba::OneShotPool>(p_pool);
		ba::SimMixerClockConfig cc;
		cc.bias_frames = kBias;
		clock = std::make_unique<ba::SimMixerClock>(*mixer, cc);
		router = std::make_unique<ba::WeaponAudioRouter>(*mixer, table, wave.bank, ba::AudioTuning{}, *pool, p_config);
		router->set_observer(&recorder);
		router->set_own_slot(0);
		std::vector<std::pair<std::string, std::shared_ptr<const SoundProgram>>> ps{ { "impact/concrete", one_shot_program(wave, kImpactStart, true) } };
		impacts = std::make_unique<ba::ImpactAudio>(*mixer, ba::ImpactSoundCache::from_programs(ps), wave.bank, ba::AudioTuning{}, *pool);
	}
	~Rig() {
		router.reset(); // Kill for the live drivers, then the mixer destroys what it owns
		impacts.reset();
	}
	// The frame an event at p_time maps to in the current frame (before the events go in).
	uint64_t expect(sw::SimTime p_time, host::PresentTimeline p_tl) const {
		const sw::SimTime now = p_tl == host::PresentTimeline::Predicted ? predicted : render;
		return uint64_t(int64_t(clock_at_events) + (p_time - now) / 1250 + kBias);
	}
	void begin() {
		host::PresentClock now;
		now.predicted_valid = now.render_valid = true;
		now.predicted = predicted;
		now.render = render;
		clock->set_now(now);
		router->set_listener(listener);
		impacts->set_listener(listener);
		clock_at_events = mixer->clock();
	}
	void frame(std::span<const host::PresentEvent> p_events = {}, std::span<const host::PresentedSoldier> p_soldiers = {},
			std::span<const host::PresentedOtherWeapon> p_weapons = {}, std::span<const ba::MuzzlePoint> p_muzzles = {},
			const std::vector<host::PresentedOtherWeapon> *p_newest = nullptr) {
		begin();
		if (p_newest != nullptr) {
			router->set_others_newest(*p_newest, sw::SimTime((render + kRenderDelay) / kTick + 1) * kTick);
		}
		router->on_events(p_events, *clock);
		router->on_others(p_soldiers, p_weapons, p_muzzles, *clock);
		end();
	}
	void end() {
		router->update(0);
		mixer->mix(buffer.data(), kFramesPerTick);
		predicted += kTick;
		render += kTick;
	}
	void idle(int p_frames) {
		for (int i = 0; i < p_frames; ++i) {
			frame();
		}
	}
};

host::PresentEvent own_shot(uint16_t p_def, uint32_t p_id, sw::SimTime p_time) {
	host::PresentEvent e;
	e.kind = host::PresentKind::OwnShot;
	e.timeline = host::PresentTimeline::Predicted;
	e.slot = 0;
	e.def = p_def;
	e.id = p_id;
	e.time = p_time;
	return e;
}

host::PresentEvent remote_shot(uint8_t p_slot, uint16_t p_def, uint32_t p_id, sw::SimTime p_time, const ba::Vec3 &p_pos) {
	host::PresentEvent e;
	e.kind = host::PresentKind::RemoteShot;
	e.timeline = host::PresentTimeline::Render;
	e.slot = p_slot;
	e.def = p_def;
	e.id = p_id;
	e.time = p_time;
	e.pos[0] = p_pos.x;
	e.pos[1] = p_pos.y;
	e.pos[2] = p_pos.z;
	return e;
}

// One other soldier firing on a server schedule, presented as cl does: RemoteShots about the render delay early,
// records at render time from 30 Hz samples with sv's firing and burst_seq rules (gunplay.cpp: burst_count counts the
// ticks with shots after a tick without; firing = a shot in the snapshot interval of 2 ticks up to the sample tick).
struct Shooter {
	uint8_t slot = 5;
	uint8_t generation = 1;
	uint16_t def = kAuto;
	ba::Vec3 feet{ 0.0f, 0.0f, 50.0f };
	float yaw = 3.14159265f; // facing the listener at the origin (forward -z)
	std::vector<sw::SimTime> shots;
	std::vector<uint32_t> ids;
	std::vector<bool> delivered; // RemoteShot sent (false: the datagram is lost)
	std::vector<bool> sent;
	uint8_t reload_seq = 0;
	static uint32_t tick_of(sw::SimTime p_t) {
		return uint32_t(p_t / kTick);
	}
	bool shots_in_tick(uint32_t p_tick) const {
		return std::any_of(shots.begin(), shots.end(), [&](sw::SimTime t) { return tick_of(t) == p_tick; });
	}
	uint8_t burst_seq_at(uint32_t p_tick) const {
		uint32_t n = 0;
		uint32_t prev = std::numeric_limits<uint32_t>::max();
		for (sw::SimTime t : shots) {
			const uint32_t k = tick_of(t);
			if (k > p_tick || k == prev) {
				continue;
			}
			if (prev == std::numeric_limits<uint32_t>::max() || k != prev + 1 || !shots_in_tick(k - 1)) {
				++n;
			}
			prev = k;
		}
		return uint8_t(n & 0x0F);
	}
	bool firing_at(uint32_t p_sample) const {
		return std::any_of(shots.begin(), shots.end(), [&](sw::SimTime t) { return tick_of(t) + 2 > p_sample && tick_of(t) <= p_sample; });
	}
	void burst(sw::SimTime p_first, uint32_t p_first_id, uint32_t p_rounds, double p_interval_s = 0.075) {
		for (uint32_t k = 0; k < p_rounds; ++k) {
			shots.push_back(p_first + sw::SimTime(std::llround(double(k) * p_interval_s * double(sw::kTimeUnitsPerSecond))));
			ids.push_back(p_first_id + k);
			delivered.push_back(true);
			sent.push_back(false);
		}
	}
	ba::Vec3 eye() const {
		return { feet.x, feet.y + 1.6f, feet.z };
	}
	// The events cl hands over this frame: every shot of the ticks up to the newest snapshot's (render delay: 6 ticks).
	void events(sw::SimTime p_render, std::vector<host::PresentEvent> &r_out) {
		for (size_t i = 0; i < shots.size(); ++i) {
			if (!sent[i] && tick_of(shots[i]) <= tick_of(p_render + kRenderDelay)) {
				sent[i] = true;
				if (delivered[i]) {
					r_out.push_back(remote_shot(slot, def, ids[i], shots[i], eye()));
				}
			}
		}
	}
	host::PresentedSoldier soldier() const {
		host::PresentedSoldier s;
		s.slot = slot;
		s.generation = generation;
		s.position = { feet.x, feet.y, feet.z };
		s.yaw = yaw;
		s.eye_height = 1.6f;
		return s;
	}
	host::PresentedOtherWeapon weapon(sw::SimTime p_render) const {
		host::PresentedOtherWeapon w;
		w.valid = true;
		w.slot = slot;
		w.generation = generation;
		w.def = def;
		const uint32_t sample = (tick_of(p_render) / 2) * 2;
		w.sample_tick = sample;
		w.firing = firing_at(sample);
		w.burst_seq = burst_seq_at(sample);
		w.reload_seq = reload_seq;
		return w;
	}
};

// Runs a rig over p_frames frames with the shooters presented (p_newest: also the records at the newest tick).
void run(Rig &r, std::vector<Shooter> &p_shooters, int p_frames, std::span<const ba::MuzzlePoint> p_muzzles = {}, bool p_newest = true) {
	for (int f = 0; f < p_frames; ++f) {
		std::vector<host::PresentEvent> ev;
		std::vector<host::PresentedSoldier> soldiers;
		std::vector<host::PresentedOtherWeapon> weapons, newest;
		for (Shooter &s : p_shooters) {
			s.events(r.render, ev);
			soldiers.push_back(s.soldier());
			weapons.push_back(s.weapon(r.render));
			newest.push_back(s.weapon(r.render + kRenderDelay));
		}
		r.frame(ev, soldiers, weapons, p_muzzles, p_newest ? &newest : nullptr);
	}
}

void check_grid(const Seen &p_seen, uint32_t p_rounds, uint32_t p_interval_frames = kIntervalFrames) {
	const std::vector<uint64_t> trig = frames_of(p_seen.trace, SamplerTraceKind::Trigger);
	const std::vector<uint64_t> steps = frames_of(p_seen.trace, SamplerTraceKind::Step);
	REQUIRE(trig.size() >= 1);
	CHECK(trig[0] == p_seen.first);
	REQUIRE(steps.size() == p_rounds - 1);
	for (uint32_t k = 1; k < p_rounds; ++k) {
		CHECK_MESSAGE(steps[k - 1] == p_seen.first + uint64_t(k) * p_interval_frames, "round ", k);
	}
}

} // namespace

// =====================================================================================================================

TEST_SUITE("audio-router") {
	TEST_CASE("SimMixerClock: the two timelines, the latency, the bias, late clamps, hostile times") {
		ba::MixerCore mixer(ba::MixerConfig{});
		std::vector<float> buf(2 * 4096);
		mixer.mix(buf.data(), 4096); // clock 4096
		ba::SimMixerClock clock(mixer, ba::SimMixerClockConfig{ kRate, 0 });
		host::PresentClock now;
		now.predicted_valid = now.render_valid = true;
		now.predicted = 100 * kTick;
		now.render = 90 * kTick;
		clock.set_now(now);
		CHECK(clock.heard_frame() == 4096);
		CHECK(clock.frame_of(0, host::PresentTimeline::Immediate) == mixer.earliest_frame());
		CHECK(clock.late() == 0);
		// 0.5 s ahead on either timeline: half a second of frames past the heard frame.
		CHECK(clock.frame_of(now.render + sw::kTimeUnitsPerSecond / 2, host::PresentTimeline::Render) == 4096 + 24000);
		CHECK(clock.frame_of(now.predicted + sw::kTimeUnitsPerSecond / 2, host::PresentTimeline::Predicted) == 4096 + 24000);
		// The two timelines differ by their now: the same time is 10 ticks (8000 frames) later on Render.
		CHECK(clock.frame_of(100 * kTick + sw::kTimeUnitsPerSecond, host::PresentTimeline::Render) -
						clock.frame_of(100 * kTick + sw::kTimeUnitsPerSecond, host::PresentTimeline::Predicted) ==
				8000);
		// Latency: the heard frame is the clock minus it.
		clock.set_output_latency_frames(1000);
		CHECK(clock.heard_frame() == 3096);
		CHECK(clock.frame_of(now.render + sw::kTimeUnitsPerSecond, host::PresentTimeline::Render) == 3096 + 48000);
		clock.set_output_latency_frames(100000);
		CHECK(clock.heard_frame() == 0);
		clock.set_output_latency_frames(0);
		// Bias: a constant offset.
		clock.set_bias_frames(2048);
		CHECK(clock.bias_frames() == 2048);
		CHECK(clock.frame_of(now.predicted, host::PresentTimeline::Predicted) == 4096 + 2048);
		clock.set_bias_frames(0);
		// Past and late: clamped to earliest_frame and counted, the depth kept.
		CHECK(clock.frame_of(now.predicted - sw::kTimeUnitsPerSecond, host::PresentTimeline::Predicted) == mixer.earliest_frame());
		CHECK(clock.late() == 1);
		CHECK(clock.max_late_frames() == mixer.earliest_frame() + 48000 - 4096); // earliest - (heard - 1 s)
		// An invalid timeline maps to earliest_frame, counted late.
		host::PresentClock half = now;
		half.render_valid = false;
		clock.set_now(half);
		CHECK(clock.frame_of(now.render, host::PresentTimeline::Render) == mixer.earliest_frame());
		CHECK(clock.late() == 2);
		clock.set_now(now);
		// Hostile times: far future and far past never wrap.
		CHECK(clock.frame_of(std::numeric_limits<sw::SimTime>::max(), host::PresentTimeline::Render) > mixer.earliest_frame());
		CHECK(clock.frame_of(std::numeric_limits<sw::SimTime>::min(), host::PresentTimeline::Render) == mixer.earliest_frame());
		CHECK(clock.late() == 3);
		CHECK(clock.rate() == kRate);
		CHECK(clock.earliest_frame() == mixer.earliest_frame());
	}

	TEST_CASE("own shots: one 1p burst per trigger hold on the shot-index grid; trigger up, a gap and a def change end it") {
		Rig r;
		// 10 rounds at 75 ms (4.5 ticks), the first at 0.3 ticks into the frame's tick; each OwnShot in the frame its time
		// is reached on the predicted clock.
		const sw::SimTime t0 = r.predicted + 3 * kTick / 10;
		std::vector<sw::SimTime> times;
		for (uint32_t k = 0; k < 10; ++k) {
			times.push_back(t0 + sw::SimTime(k) * 4500000);
		}
		r.router->set_own_trigger(true);
		size_t next = 0;
		uint64_t expected_first = 0;
		for (int f = 0; f < 60; ++f) {
			std::vector<host::PresentEvent> ev;
			while (next < times.size() && times[next] <= r.predicted + kTick) {
				ev.push_back(own_shot(kAuto, 40 + uint32_t(next), times[next]));
				++next;
			}
			if (next == times.size() && f > 0 && ev.empty()) {
				r.router->set_own_trigger(false);
			}
			r.begin();
			if (!ev.empty() && ev.front().id == 40) {
				expected_first = r.expect(t0, host::PresentTimeline::Predicted);
			}
			r.router->on_events(ev, *r.clock);
			r.end();
		}
		REQUIRE(r.recorder.seen.size() == 1);
		const Seen &s = r.recorder.seen[0];
		CHECK(s.first == expected_first);
		CHECK(s.first_person == 1.0f);
		CHECK(s.enable_step == 1.0f);
		CHECK(s.distance == doctest::Approx(0.0f));
		CHECK(s.seed == ba::sound_seed(0, 0));
		check_grid(s, 10);
		REQUIRE(s.up);
		CHECK(s.stop == s.first + 9 * kIntervalFrames + kIntervalFrames / 2);
		// The stop event released the sampler into its tail; the instance finished and came back.
		CHECK(!frames_of(s.trace, SamplerTraceKind::Tail).empty());
		const ba::WeaponAudioStats &st = r.router->stats();
		CHECK(st.bursts == 1);
		CHECK(st.own_bursts == 1);
		CHECK(st.rounds == 9);
		CHECK(st.event_rounds == 10);
		CHECK(st.trigger_ups == 1);
		CHECK(st.weapon_voices == 0);
		CHECK(r.clock->late() == 0);
		CHECK(r.mixer->stats().late_commands.load() == 0);

		// Without trigger state: the burst ends once the predicted clock passes the last round by 1.5 intervals. A
		// second burst after a gap of 10 intervals, then a def change: three new bursts, seeds from the own counter.
		Rig q;
		const sw::SimTime a = q.predicted + kTick;
		std::vector<host::PresentEvent> plan;
		for (uint32_t k = 0; k < 3; ++k) {
			plan.push_back(own_shot(kAuto, 10 + k, a + sw::SimTime(k) * 4500000));
		}
		for (uint32_t k = 0; k < 3; ++k) {
			plan.push_back(own_shot(kAuto, 13 + k, a + sw::SimTime(12 + k) * 4500000));
		}
		for (uint32_t k = 0; k < 2; ++k) {
			plan.push_back(own_shot(kAuto2, 16 + k, a + sw::SimTime(15 + k) * 4500000));
		}
		size_t i = 0;
		for (int f = 0; f < 150; ++f) {
			std::vector<host::PresentEvent> ev;
			while (i < plan.size() && plan[i].time <= q.predicted) {
				ev.push_back(plan[i++]);
			}
			q.frame(ev);
		}
		REQUIRE(q.recorder.seen.size() == 3);
		CHECK(q.recorder.seen[0].seed == ba::sound_seed(0, 0));
		CHECK(q.recorder.seen[1].seed == ba::sound_seed(0, 1));
		CHECK(q.recorder.seen[2].seed == ba::sound_seed(0, 2));
		for (const Seen &s2 : q.recorder.seen) {
			CHECK(s2.up);
		}
		check_grid(q.recorder.seen[0], 3);
		check_grid(q.recorder.seen[1], 3);
		check_grid(q.recorder.seen[2], 2);
		CHECK(q.router->stats().trigger_ups == 3);
		// The free camera: IsFirstPerson 0.
		Rig fc;
		fc.router->set_own_first_person(false);
		fc.frame(std::vector<host::PresentEvent>{ own_shot(kAuto, 1, fc.predicted) });
		fc.idle(20);
		REQUIRE(fc.recorder.seen.size() == 1);
		CHECK(fc.recorder.seen[0].first_person == 0.0f);
	}

	TEST_CASE("own single fire: one instance per shot, released at once, EnableStep 0") {
		Rig r;
		std::vector<host::PresentEvent> plan;
		for (uint32_t k = 0; k < 3; ++k) {
			plan.push_back(own_shot(kSingle, 7 + k, r.predicted + sw::SimTime(k) * 12 * kTick));
		}
		size_t i = 0;
		for (int f = 0; f < 80; ++f) {
			std::vector<host::PresentEvent> ev;
			while (i < plan.size() && plan[i].time <= r.predicted) {
				ev.push_back(plan[i++]);
			}
			r.frame(ev);
		}
		REQUIRE(r.recorder.seen.size() == 3);
		for (const Seen &s : r.recorder.seen) {
			CHECK(s.up);
			CHECK(s.enable_step == 0.0f);
			CHECK(s.stop == s.first + 1);
			CHECK(frames_of(s.trace, SamplerTraceKind::Trigger).size() == 1);
		}
		CHECK(r.router->stats().rounds == 0);
		CHECK(r.router->stats().weapon_voices == 0);
	}

	TEST_CASE("others: RemoteShots start the burst on its frame and place its rounds, the records keep it, the hold ends it") {
		Rig r;
		std::vector<Shooter> shooters(1);
		Shooter &sh = shooters[0];
		const sw::SimTime t0 = r.render + 20 * kTick + kTick / 4;
		sh.burst(t0, 300, 10);
		run(r, shooters, 120);
		REQUIRE(r.recorder.seen.size() == 1);
		const Seen &s = r.recorder.seen[0];
		CHECK(s.slot == 5);
		CHECK(s.first_person == 0.0f);
		CHECK(s.enable_step == 1.0f);
		// The stand-in muzzle: eye height, 0.5 m along the view (towards the listener).
		CHECK(s.distance == doctest::Approx(std::sqrt(1.6 * 1.6 + 49.5 * 49.5)).epsilon(1e-4));
		// sv's burst_seq for this burst: the records' value before it plus one.
		CHECK(s.seed == ba::sound_seed(5, sh.burst_seq_at(Shooter::tick_of(t0))));
		check_grid(s, 10);
		REQUIRE(s.up);
		CHECK(s.stop == s.first + 9 * kIntervalFrames + kIntervalFrames / 2);
		const ba::WeaponAudioStats &st = r.router->stats();
		CHECK(st.bursts == 1);
		CHECK(st.event_rounds == 10);
		CHECK(st.state_rounds == 0);
		CHECK(r.clock->late() == 0);

		// Lost RemoteShots inside the burst: the shot indices still place every round.
		Rig q;
		std::vector<Shooter> lossy(1);
		lossy[0].burst(q.render + 20 * kTick, 50, 10);
		for (size_t k : { 2u, 6u, 9u }) {
			lossy[0].delivered[k] = false;
		}
		run(q, lossy, 120);
		REQUIRE(q.recorder.seen.size() == 1);
		check_grid(q.recorder.seen[0], 10); // 2 and 6 from the index gaps, 9 from the newest records
		CHECK(q.router->stats().state_rounds == 3);
		CHECK(q.recorder.seen[0].up);
		// Without the newest records: the render clock ends the burst, late (its stop clamped to earliest_frame).
		Rig late;
		std::vector<Shooter> plain(1);
		plain[0].burst(late.render + 20 * kTick, 50, 10);
		run(late, plain, 120, {}, false);
		REQUIRE(late.recorder.seen.size() == 1);
		check_grid(late.recorder.seen[0], 10);
		CHECK(late.recorder.seen[0].stop > late.recorder.seen[0].first + 9 * kIntervalFrames + kIntervalFrames / 2);

		// Every RemoteShot lost: the records start the burst (late, at render time) and keep it to its last evidence.
		Rig z;
		std::vector<Shooter> silent(1);
		silent[0].burst(z.render + 20 * kTick, 80, 10);
		std::fill(silent[0].delivered.begin(), silent[0].delivered.end(), false);
		run(z, silent, 120);
		REQUIRE(z.recorder.seen.size() == 1);
		CHECK(z.recorder.seen[0].up);
		CHECK(z.router->stats().state_rounds >= 8);
		CHECK(z.recorder.seen[0].seed == ba::sound_seed(5, silent[0].burst_seq_at(Shooter::tick_of(silent[0].shots[0]) + 1)));

		// Two bursts with a pause: two instances; a death ends a burst at once.
		Rig d;
		std::vector<Shooter> two(1);
		two[0].burst(d.render + 20 * kTick, 10, 5);
		two[0].burst(d.render + 80 * kTick, 15, 5);
		run(d, two, 160);
		CHECK(d.recorder.seen.size() == 2);
		CHECK(d.recorder.seen[0].seed != d.recorder.seen[1].seed);
		for (const Seen &x : d.recorder.seen) {
			CHECK(x.up);
			check_grid(x, 5);
		}
		Rig k;
		std::vector<Shooter> dead(1);
		dead[0].burst(k.render + 10 * kTick, 1, 30);
		run(k, dead, 30);
		host::PresentEvent death;
		death.kind = host::PresentKind::Death;
		death.timeline = host::PresentTimeline::Render;
		death.aux = 5;
		k.frame(std::vector<host::PresentEvent>{ death });
		REQUIRE(k.recorder.seen.size() == 1);
		CHECK(k.recorder.seen[0].up);
	}

	TEST_CASE("others by distance: the crowd muzzle, the cull past max distance, the listener re-send") {
		Rig r;
		std::vector<Shooter> shooters(2);
		shooters[0].slot = 3;
		shooters[0].feet = { 0.0f, 0.0f, 100.0f };
		shooters[0].burst(r.render + 20 * kTick, 1, 4);
		shooters[1].slot = 4;
		shooters[1].feet = { 0.0f, 0.0f, 1200.0f }; // past max_weapon_distance_m
		shooters[1].burst(r.render + 20 * kTick, 1, 4);
		std::vector<ba::MuzzlePoint> muzzles(2);
		muzzles[0].valid = true;
		muzzles[0].position = { 3.0f, 1.2f, 99.0f };
		run(r, shooters, 26, muzzles);
		REQUIRE(r.recorder.seen.size() == 1);
		CHECK(r.recorder.seen[0].slot == 3);
		CHECK(r.recorder.seen[0].distance == doctest::Approx(std::sqrt(9.0 + 1.44 + 99.0 * 99.0)).epsilon(1e-4));
		CHECK(r.router->stats().culled >= 1);
		// The listener moves while the burst is held: one SetEmitter per live instance after the SetListener.
		const uint32_t live = r.router->stats().weapon_voices;
		REQUIRE(live == 1);
		const uint64_t before = r.router->stats().emitter_updates;
		r.listener.position = { 0.0f, 0.0f, 10.0f };
		run(r, shooters, 1, muzzles);
		CHECK(r.router->stats().emitter_updates >= before + live);
		run(r, shooters, 60, muzzles);
		CHECK(r.recorder.seen[0].up);
	}

	TEST_CASE("reloads and one-shots: own OwnTimed, others' reload_seq, knife Melee, the M67 detonation") {
		Rig r;
		host::PresentEvent reload;
		reload.kind = host::PresentKind::OwnTimed;
		reload.timeline = host::PresentTimeline::Predicted;
		reload.def = kAuto;
		reload.id = sw::weapon_events::kReloadStarted;
		reload.time = r.predicted;
		host::PresentEvent knife = own_shot(kKnife, 3, r.predicted);
		host::PresentEvent other_knife = remote_shot(6, kKnife, 9, r.render + 3 * kTick, { 0.0f, 1.6f, 5.0f });
		host::PresentEvent boom;
		boom.kind = host::PresentKind::Detonation;
		boom.timeline = host::PresentTimeline::Render;
		boom.def = kGrenade;
		boom.slot = 6;
		boom.time = r.render + kTick;
		boom.pos[2] = 20.0f;
		host::PresentEvent none = own_shot(kNone, 1, r.predicted);
		r.frame(std::vector<host::PresentEvent>{ reload, knife, other_knife, boom, none });
		std::vector<Shooter> shooters(1);
		shooters[0].slot = 6;
		run(r, shooters, 2);
		shooters[0].reload_seq = 1;
		run(r, shooters, 2);
		const ba::WeaponAudioStats &st = r.router->stats();
		CHECK(st.reloads == 2);
		CHECK(st.detonations == 1);
		CHECK(st.one_shots == 5); // own reload, own knife, other knife, explosion, other reload
		CHECK(st.unbound_defs == 1);
		CHECK(st.bursts == 0);
		CHECK(r.pool->started() == 5);
		CHECK(ba::one_shot_event_of(*r.table->defs[kKnife].program) == ba::kHashMelee);
		CHECK(ba::one_shot_event_of(*r.table->defs[kGrenade].program) == ba::kHashFire);
		CHECK(ba::one_shot_event_of(*r.table->defs[kGrenade].explosion) == kImpactStart);
		// They sound and finish: the mix is not silent, and the pool drains (a one-shot gets its Stop after
		// kOneShotStopAfterSeconds: these patches have a stop event, so they finish only once it is seen).
		r.idle(int(ba::kOneShotStopAfterSeconds * 60.0) + 30);
		CHECK(std::any_of(r.capture.samples.begin(), r.capture.samples.end(), [](float v) { return v != 0.0f; }));
		CHECK(r.pool->live() == 0);
	}

	TEST_CASE("voice limit: running bursts are never stolen, the farthest released tail is, the rest are culled") {
		ba::WeaponAudioConfig cfg;
		cfg.max_weapon_voices = 3;
		Rig r(cfg, 2);
		std::vector<Shooter> s(4);
		for (uint8_t i = 0; i < 4; ++i) {
			s[i].slot = uint8_t(10 + i);
			s[i].feet = { 0.0f, 0.0f, 20.0f + 20.0f * float(i) };
		}
		// 10, 11, 12 hold long bursts; 13 starts while all three run: culled.
		for (uint8_t i = 0; i < 3; ++i) {
			s[i].burst(r.render + 10 * kTick, 1, i == 1 ? 3 : 40);
		}
		s[3].burst(r.render + 14 * kTick, 1, 1);
		run(r, s, 30);
		CHECK(r.recorder.of(13) == 0);
		CHECK(r.router->stats().culled >= 1);
		// 11's short burst has ended (a tail); a new shooter steals it rather than a running burst.
		s[3].burst(r.render + 8 * kTick, 10, 3);
		run(r, s, 20);
		CHECK(r.router->stats().stolen == 1);
		CHECK(r.recorder.of(13) == 1);
		CHECK(r.router->stats().peak_weapon_voices == 3);
		run(r, s, 120);
		// One-shot pool of 2: the third steals the oldest.
		std::vector<host::PresentEvent> knives;
		for (uint32_t k = 0; k < 3; ++k) {
			knives.push_back(own_shot(kKnife, 100 + k, r.predicted));
		}
		r.frame(knives);
		CHECK(r.pool->stolen() == 1);
		CHECK(r.pool->peak() == 2);
		CHECK(r.router->stats().mixer_refusals == 0);
		CHECK(r.mixer->stats().starved_voices.load() == 0);
	}

	TEST_CASE("impacts and effect sounds: known patches at their frame, unknown counted, out of earshot dropped, stop by key") {
		Rig r;
		r.begin();
		ba::ImpactSoundRequest hit;
		hit.patch = "impact/concrete";
		hit.position = { 0.0f, 0.0f, 10.0f };
		hit.seed = 77;
		hit.time = r.render + 2 * kTick;
		hit.timeline = host::PresentTimeline::Render;
		r.impacts->play_impact(hit, *r.clock);
		ba::ImpactSoundRequest unknown = hit;
		unknown.patch = "impact/none";
		r.impacts->play_impact(unknown, *r.clock);
		ba::ImpactSoundRequest far = hit;
		far.position = { 0.0f, 0.0f, 900.0f };
		r.impacts->play_impact(far, *r.clock);
		ba::ImpactSoundRequest nan = hit;
		nan.position.x = std::numeric_limits<float>::quiet_NaN();
		r.impacts->play_impact(nan, *r.clock);
		ba::EffectSoundRequest fx;
		fx.patch = "impact/concrete";
		fx.effect = (uint64_t(7) << 32) | 1;
		fx.component = 2;
		fx.position = { 1.0f, 0.0f, 1.0f };
		r.impacts->on_effect_sound(fx, *r.clock);
		r.end();
		r.idle(2);
		r.begin();
		ba::EffectSoundRequest stop = fx;
		stop.start = false;
		r.impacts->on_effect_sound(stop, *r.clock);
		r.impacts->on_effect_sound(stop, *r.clock); // a second stop finds nothing
		r.end();
		r.idle(int(ba::kOneShotStopAfterSeconds * 60.0) + 30);
		const ba::ImpactAudioStats &st = r.impacts->stats();
		CHECK(st.impacts == 1);
		CHECK(st.unknown_patches == 1);
		CHECK(st.effect_sounds == 1);
		CHECK(st.effect_stops == 1);
		CHECK(st.refused == 0);
		CHECK(r.pool->live() == 0);
		CHECK(std::any_of(r.capture.samples.begin(), r.capture.samples.end(), [](float v) { return v != 0.0f; }));
		// The cache: names, programs, counts.
		std::vector<std::pair<std::string, std::shared_ptr<const SoundProgram>>> ps{ { "b", r.weapon.program }, { "a", r.weapon.program }, { "", r.weapon.program },
			{ "c", nullptr } };
		std::shared_ptr<const ba::ImpactSoundCache> cache = ba::ImpactSoundCache::from_programs(ps);
		CHECK(cache->compiled() == 2);
		CHECK(cache->programs().size() == 2);
		CHECK(cache->find("a") != nullptr);
		CHECK(cache->find("c") == nullptr);
	}

	TEST_CASE("one-shot pool aging: a one-shot that never finishes is stopped, then killed and forgotten") {
		Rig r;
		// A program whose sampler never triggers (no event reaches it): it stays live until the pool ages it.
		Builder b;
		b.input(kStopHash, true);
		b.p.stop_event = 0;
		const std::shared_ptr<const SoundProgram> mute = b.build();
		r.begin();
		ba::EmitterState em;
		REQUIRE(r.pool->start(*r.mixer, mute, r.wave.bank, ba::AudioTuning{}, 1, em, r.listener, r.clock->earliest_frame()));
		r.end();
		const int frames = int(ba::kOneShotForgetSeconds * 60.0) + 30;
		r.idle(frames);
		CHECK(r.pool->live() == 0);
	}

	TEST_CASE("determinism: the same session twice mixes bit-identical output with identical counters") {
		const auto session = [](Rig &r) {
			std::vector<Shooter> s(3);
			for (uint8_t i = 0; i < 3; ++i) {
				s[i].slot = uint8_t(20 + i);
				s[i].feet = { 10.0f * float(i), 0.0f, 30.0f + 15.0f * float(i) };
				s[i].burst(r.render + sw::SimTime(10 + 7 * i) * kTick + kTick / 3, 1, 8);
			}
			std::vector<host::PresentEvent> own;
			for (uint32_t k = 0; k < 6; ++k) {
				own.push_back(own_shot(kAuto, k, r.predicted + 5 * kTick + sw::SimTime(k) * 4500000));
			}
			size_t i = 0;
			for (int f = 0; f < 140; ++f) {
				std::vector<host::PresentEvent> ev;
				while (i < own.size() && own[i].time <= r.predicted) {
					ev.push_back(own[i++]);
				}
				std::vector<host::PresentedSoldier> soldiers;
				std::vector<host::PresentedOtherWeapon> weapons;
				for (Shooter &x : s) {
					x.events(r.render, ev);
					soldiers.push_back(x.soldier());
					weapons.push_back(x.weapon(r.render));
				}
				r.listener.position = { 0.01f * float(f), 0.0f, 0.0f };
				r.frame(ev, soldiers, weapons);
			}
		};
		Rig a, b;
		session(a);
		session(b);
		REQUIRE(a.capture.samples.size() == b.capture.samples.size());
		CHECK(std::memcmp(a.capture.samples.data(), b.capture.samples.data(), a.capture.samples.size() * sizeof(float)) == 0);
		CHECK(std::memcmp(&a.router->stats(), &b.router->stats(), sizeof(ba::WeaponAudioStats)) == 0);
		CHECK(a.router->stats().bursts == 4);
		CHECK(std::any_of(a.capture.samples.begin(), a.capture.samples.end(), [](float v) { return v != 0.0f; }));
		REQUIRE(a.recorder.seen.size() == b.recorder.seen.size());
		for (size_t i = 0; i < a.recorder.seen.size(); ++i) {
			CHECK(a.recorder.seen[i].seed == b.recorder.seen[i].seed);
			CHECK(a.recorder.seen[i].first == b.recorder.seen[i].first);
		}
	}

	TEST_CASE("hostile input: NaN geometry, unknown defs, slot 255, huge ids and times are counted, never a crash") {
		Rig r;
		std::vector<host::PresentEvent> ev;
		host::PresentEvent bad = remote_shot(255, 999, 0xFFFFFFFFu, std::numeric_limits<int64_t>::max(), { 0, 0, 0 });
		ev.push_back(bad);
		host::PresentEvent nan = remote_shot(7, kAuto, 1, r.render + kTick, { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f });
		nan.dir[0] = std::numeric_limits<float>::infinity();
		ev.push_back(nan);
		ev.push_back(own_shot(0xFFFF, 1, std::numeric_limits<int64_t>::min()));
		host::PresentEvent wrap = remote_shot(7, kAuto, 0, r.render + kTick, { 0, 0, 5 });
		ev.push_back(wrap);
		host::PresentEvent death;
		death.kind = host::PresentKind::Death;
		death.aux = 255;
		ev.push_back(death);
		host::PresentEvent spawn;
		spawn.kind = host::PresentKind::Spawn;
		spawn.slot = 255;
		ev.push_back(spawn);
		host::PresentedSoldier s;
		s.slot = 9;
		s.position = { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f };
		s.yaw = std::numeric_limits<float>::infinity();
		host::PresentedOtherWeapon w;
		w.valid = true;
		w.slot = 9;
		w.def = 40000;
		w.firing = true;
		w.sample_tick = 0xFFFFFFFFu;
		std::vector<host::PresentedSoldier> soldiers{ s };
		std::vector<host::PresentedOtherWeapon> weapons{ w };
		for (int f = 0; f < 40; ++f) {
			r.frame(f == 0 ? std::span<const host::PresentEvent>(ev) : std::span<const host::PresentEvent>(), soldiers, weapons);
		}
		const ba::WeaponAudioStats &st = r.router->stats();
		CHECK(st.unbound_defs >= 3);
		CHECK(st.mixer_refusals == 0);
		CHECK(r.router->stats().weapon_voices <= 2);
		// A mismatched span (more weapons than soldiers) and an empty frame are harmless.
		std::vector<host::PresentedOtherWeapon> extra{ w, w, w };
		r.frame({}, {}, extra);
		r.idle(60);
		CHECK(r.router->stats().weapon_voices == 0);
	}

	TEST_CASE("game check bookkeeping: the script, windows from the observer, step_done, the guarded WAV") {
		const ba::GameCheckScript script = ba::game_check_script();
		REQUIRE(script.steps.size() >= 7);
		for (size_t i = 0; i < ba::kGameCheckDistances.size(); ++i) {
			CHECK(script.steps[i].distance_m == ba::kGameCheckDistances[i]);
			CHECK(script.steps[i].bursts == 2);
			CHECK(!script.steps[i].audition);
		}
		CHECK(script.steps[6].pin_distance_hpfs);
		size_t auditions = 0;
		for (const ba::GameCheckStep &s : script.steps) {
			auditions += s.audition ? 1 : 0;
			if (s.audition) {
				CHECK(!s.patch.empty());
				CHECK(!s.bands.empty());
			}
		}
		CHECK(auditions == 10); // m98b x 3, rpg7 x 3, m1a2 x 4
		ba::GameCheckScript small;
		small.steps = { script.steps[0] };
		small.timeout_s = 4.0;
		ba::GameAudioCheck check(small, kRate, 0);
		Rig r;
		struct Fan final : ba::MixTap {
			ba::MixTap *a, *b;
			void on_block(const float *p, uint32_t n, uint64_t f) override {
				a->on_block(p, n, f);
				b->on_block(p, n, f);
			}
		} fan;
		fan.a = &r.capture;
		fan.b = &check;
		r.mixer->set_tap(&fan);
		struct Both final : ba::WeaponAudioObserver {
			ba::WeaponAudioObserver *a, *b;
			void on_driver_instance(uint8_t p_slot, uint64_t p_seed, uint64_t p_first, ba::PatchInstance &r_i) override {
				a->on_driver_instance(p_slot, p_seed, p_first, r_i);
				b->on_driver_instance(p_slot, p_seed, p_first, r_i);
			}
			void on_trigger_up(uint8_t p_slot, uint64_t p_seed, uint64_t p_stop) override {
				a->on_trigger_up(p_slot, p_seed, p_stop);
				b->on_trigger_up(p_slot, p_seed, p_stop);
			}
		} both;
		both.a = &r.recorder;
		both.b = &check;
		r.router->set_observer(&both);
		check.set_shooter(5);
		std::vector<Shooter> s(1);
		s[0].feet = { 0.0f, 0.0f, 5.0f };
		s[0].burst(r.render + 10 * kTick, 1, 10); // before begin_step + settle: not taken
		run(r, s, 5);
		check.begin_step(0, 5.0f);
		s[0].burst(r.render + 80 * kTick, 20, 10);
		s[0].burst(r.render + 140 * kTick, 40, 10);
		s[0].burst(r.render + 200 * kTick, 60, 10); // a third burst: the step takes two
		run(r, s, 260);
		const std::span<const ba::GameCheckWindow> w = check.windows();
		REQUIRE(w.size() == 2);
		CHECK(w[0].stopped);
		CHECK(w[1].stopped);
		CHECK(w[0].stop_frame == w[0].first_frame + 9 * kIntervalFrames + kIntervalFrames / 2);
		CHECK(w[0].listener_distance_m == 5.0f);
		CHECK(w[0].probe_pitch >= 0.0f); // probed (the synthetic patch has no pitch register 28: a value, not a law)
		CHECK(check.step_done(0) == (check.recorded_until() >= w[1].first_frame + uint64_t(4.0 * kRate)));
		CHECK(check.step_done(5)); // out of range: done
		CHECK(check.recorded_until() > 0);
		CHECK(check.total_frames() == 4 * kRate);
		CHECK(check.dropped_frames() > 0); // the run outlasted the 4 s span
		// No guard: refused (NoRoots), nothing written.
		CHECK(!check.write_wav(std::filesystem::path("unused.wav")).ok());
		const ba::CheckReport rep = ba::analyse_game_check(small, check, ba::GameCheckCounters{}, ba::AudioTuning{}, kRate);
		CHECK(!rep.rows.empty());
		CHECK(rep.table().find("windows.d5") != std::string::npos);
		CHECK(rep.table().find("mixer.late_blocks") != std::string::npos);
		r.router->set_observer(nullptr);
		r.mixer->set_tap(&r.capture);
	}
}

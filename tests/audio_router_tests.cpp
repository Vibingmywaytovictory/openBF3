#include "test_support.h"

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/audio_check.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/audio_load.h"
#include "bf3/audio/game_audio_check.h"
#include "bf3/audio/impact_audio.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/voice_cull.h"
#include "bf3/audio/wav_io.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/client_audio/game_audio_frame.h"
#include "bf3/client_audio/kit_audio.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/path_guard.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/impact_table.h"
#include "bf3/fx/triggers.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::audio;
namespace cp = bf3::core::audio::port;
namespace ba = bf3::audio;
namespace host = bf3::host;
namespace sw = bf3::sim::weapons;
namespace sim = bf3::sim;
namespace net = bf3::net;
using ba::SamplerTraceEntry;
using ba::SamplerTraceKind;

namespace {

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

constexpr uint32_t kStepHash = 0x51e90001u;
constexpr uint32_t kStopHash = 0x57090001u;
constexpr uint32_t kEnableStepHash = 0xe5ab1e01u;
constexpr uint32_t kImpactStart = 0x1a9ac701u;
constexpr uint32_t kRate = 48000;
constexpr uint32_t kFramesPerTick = 800;
constexpr sw::SimTime kTick = sw::kTimeUnitsPerTick;
constexpr int64_t kBias = 2048;
constexpr uint32_t kIntervalFrames = 3600;
constexpr sw::SimTime kRenderDelay = 6 * kTick;

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
		d.foley = ba::read_weapon_foley(*p_weapon.program);
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

struct Seen {
	uint8_t slot = 0;
	uint64_t seed = 0;
	uint64_t first = 0;
	uint64_t stop = 0;
	bool up = false;
	float distance = -1.0f, first_person = -1.0f, enable_step = -1.0f;
	ba::SamplerTrace trace;
};

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
	TestWave wave = make_wave(5, 6000, 1, 3);
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
		router.reset();
		impacts.reset();
	}

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

struct Shooter {
	uint8_t slot = 5;
	uint8_t generation = 1;
	uint16_t def = kAuto;
	ba::Vec3 feet{ 0.0f, 0.0f, 50.0f };
	float yaw = 3.14159265f;
	std::vector<sw::SimTime> shots;
	std::vector<uint32_t> ids;
	std::vector<bool> delivered;
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

}

TEST_SUITE("audio-router") {
	TEST_CASE("SimMixerClock: the two timelines, the latency, the bias, late clamps, hostile times") {
		ba::MixerCore mixer(ba::MixerConfig{});
		std::vector<float> buf(2 * 4096);
		mixer.mix(buf.data(), 4096);
		ba::SimMixerClock clock(mixer, ba::SimMixerClockConfig{ kRate, 0 });
		host::PresentClock now;
		now.predicted_valid = now.render_valid = true;
		now.predicted = 100 * kTick;
		now.render = 90 * kTick;
		clock.set_now(now);
		const uint64_t heard = 4096 - mixer.output_delay_frames();
		CHECK(clock.heard_frame() == heard);
		CHECK(clock.frame_of(0, host::PresentTimeline::Immediate) == mixer.earliest_frame());
		CHECK(clock.late() == 0);

		CHECK(clock.frame_of(now.render + sw::kTimeUnitsPerSecond / 2, host::PresentTimeline::Render) == heard + 24000);
		CHECK(clock.frame_of(now.predicted + sw::kTimeUnitsPerSecond / 2, host::PresentTimeline::Predicted) ==
				heard + 24000);

		CHECK(clock.frame_of(100 * kTick + sw::kTimeUnitsPerSecond, host::PresentTimeline::Render) -
						clock.frame_of(100 * kTick + sw::kTimeUnitsPerSecond, host::PresentTimeline::Predicted) ==
				8000);

		clock.set_output_latency_frames(1000);
		CHECK(clock.heard_frame() == heard - 1000);
		CHECK(clock.frame_of(now.render + sw::kTimeUnitsPerSecond, host::PresentTimeline::Render) ==
				heard - 1000 + 48000);
		clock.set_output_latency_frames(100000);
		CHECK(clock.heard_frame() == 0);
		clock.set_output_latency_frames(0);

		clock.set_bias_frames(2048);
		CHECK(clock.bias_frames() == 2048);
		CHECK(clock.frame_of(now.predicted, host::PresentTimeline::Predicted) == heard + 2048);
		clock.set_bias_frames(0);

		CHECK(clock.frame_of(now.predicted - sw::kTimeUnitsPerSecond, host::PresentTimeline::Predicted) == mixer.earliest_frame());
		CHECK(clock.late() == 1);
		CHECK(clock.max_late_frames() == mixer.earliest_frame() + 48000 - heard);

		host::PresentClock half = now;
		half.render_valid = false;
		clock.set_now(half);
		CHECK(clock.frame_of(now.render, host::PresentTimeline::Render) == mixer.earliest_frame());
		CHECK(clock.late() == 2);
		clock.set_now(now);

		CHECK(clock.frame_of(std::numeric_limits<sw::SimTime>::max(), host::PresentTimeline::Render) > mixer.earliest_frame());
		CHECK(clock.frame_of(std::numeric_limits<sw::SimTime>::min(), host::PresentTimeline::Render) == mixer.earliest_frame());
		CHECK(clock.late() == 3);
		CHECK(clock.rate() == kRate);
		CHECK(clock.earliest_frame() == mixer.earliest_frame());
	}

	TEST_CASE("own shots: one 1p burst per trigger hold on the shot-index grid; trigger up, a gap and a def change end it") {
		Rig r;

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

	TEST_CASE(
			"slot sounds: a slot's accessory sound plays its def's shots with that entry's patch and binding, and only "
			"for that def") {
		Rig r;
		r.router->set_slot_sound(0, kAuto, kSingle);
		std::vector<host::PresentEvent> plan;
		for (uint32_t k = 0; k < 3; ++k) {
			plan.push_back(own_shot(kAuto, 7 + k, r.predicted + sw::SimTime(k) * 12 * kTick));
		}
		plan.push_back(own_shot(kAuto2, 10, r.predicted + 40 * kTick));
		size_t i = 0;
		for (int f = 0; f < 100; ++f) {
			std::vector<host::PresentEvent> ev;
			while (i < plan.size() && plan[i].time <= r.predicted) {
				ev.push_back(plan[i++]);
			}
			r.frame(ev);
		}
		REQUIRE(r.recorder.seen.size() == 4);
		for (size_t k = 0; k < 3; ++k) {
			CHECK(r.recorder.seen[k].enable_step == 0.0f);
			CHECK(r.recorder.seen[k].stop == r.recorder.seen[k].first + 1);
		}
		CHECK(r.recorder.seen[3].enable_step == 1.0f);
		CHECK(r.router->stats().accessory_sounds == 3);
		r.router->set_slot_sound(0, kAuto, kAuto);
		r.frame(std::vector<host::PresentEvent>{ own_shot(kAuto, 20, r.predicted) });
		r.idle(20);
		REQUIRE(r.recorder.seen.size() == 5);
		CHECK(r.recorder.seen[4].enable_step == 1.0f);
		CHECK(r.router->stats().accessory_sounds == 3);
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

		CHECK(s.distance == doctest::Approx(std::sqrt(1.6 * 1.6 + 49.5 * 49.5)).epsilon(1e-4));

		CHECK(s.seed == ba::sound_seed(5, sh.burst_seq_at(Shooter::tick_of(t0))));
		check_grid(s, 10);
		REQUIRE(s.up);
		CHECK(s.stop == s.first + 9 * kIntervalFrames + kIntervalFrames / 2);
		const ba::WeaponAudioStats &st = r.router->stats();
		CHECK(st.bursts == 1);
		CHECK(st.event_rounds == 10);
		CHECK(st.state_rounds == 0);
		CHECK(r.clock->late() == 0);

		Rig q;
		std::vector<Shooter> lossy(1);
		lossy[0].burst(q.render + 20 * kTick, 50, 10);
		for (size_t k : { 2u, 6u, 9u }) {
			lossy[0].delivered[k] = false;
		}
		run(q, lossy, 120);
		REQUIRE(q.recorder.seen.size() == 1);
		check_grid(q.recorder.seen[0], 10);
		CHECK(q.router->stats().state_rounds == 3);
		CHECK(q.recorder.seen[0].up);

		Rig late;
		std::vector<Shooter> plain(1);
		plain[0].burst(late.render + 20 * kTick, 50, 10);
		run(late, plain, 120, {}, false);
		REQUIRE(late.recorder.seen.size() == 1);
		check_grid(late.recorder.seen[0], 10);
		CHECK(late.recorder.seen[0].stop > late.recorder.seen[0].first + 9 * kIntervalFrames + kIntervalFrames / 2);

		Rig z;
		std::vector<Shooter> silent(1);
		silent[0].burst(z.render + 20 * kTick, 80, 10);
		std::fill(silent[0].delivered.begin(), silent[0].delivered.end(), false);
		run(z, silent, 120);
		REQUIRE(z.recorder.seen.size() == 1);
		CHECK(z.recorder.seen[0].up);
		CHECK(z.router->stats().state_rounds >= 8);
		CHECK(z.recorder.seen[0].seed == ba::sound_seed(5, silent[0].burst_seq_at(Shooter::tick_of(silent[0].shots[0]) + 1)));

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

		Rig sg;
		std::vector<Shooter> single(1);
		single[0].def = kSingle;
		single[0].burst(sg.render + 20 * kTick, 1, 5, 0.2);
		run(sg, single, 160);
		CHECK(sg.recorder.seen.size() == 5);
		CHECK(sg.router->stats().bursts == 5);
		CHECK(sg.router->stats().state_rounds == 0);
		for (size_t i = 0; i < sg.recorder.seen.size(); ++i) {
			CHECK(sg.recorder.seen[i].up);
			if (i > 0) {

				const int64_t gap = int64_t(sg.recorder.seen[i].first) - int64_t(sg.recorder.seen[i - 1].first);
				CHECK(std::llabs(gap - 9600) <= int64_t(ba::kBlockFrames));
			}
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
		shooters[1].feet = { 0.0f, 0.0f, 1200.0f };
		shooters[1].burst(r.render + 20 * kTick, 1, 4);
		std::vector<ba::MuzzlePoint> muzzles(2);
		muzzles[0].valid = true;
		muzzles[0].position = { 3.0f, 1.2f, 99.0f };
		run(r, shooters, 26, muzzles);
		REQUIRE(r.recorder.seen.size() == 1);
		CHECK(r.recorder.seen[0].slot == 3);
		CHECK(r.recorder.seen[0].distance == doctest::Approx(std::sqrt(9.0 + 1.44 + 99.0 * 99.0)).epsilon(1e-4));
		CHECK(r.router->stats().culled >= 1);

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
		CHECK(st.one_shots == 5);
		CHECK(st.unbound_defs == 1);
		CHECK(st.bursts == 0);
		CHECK(r.pool->started() == 5);
		CHECK(ba::one_shot_event_of(*r.table->defs[kKnife].program) == ba::kHashMelee);
		CHECK(ba::one_shot_event_of(*r.table->defs[kGrenade].program) == ba::kHashFire);
		CHECK(ba::one_shot_event_of(*r.table->defs[kGrenade].explosion) == kImpactStart);

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

		for (uint8_t i = 0; i < 3; ++i) {
			s[i].burst(r.render + 10 * kTick, 1, i == 1 ? 3 : 40);
		}
		s[3].burst(r.render + 14 * kTick, 1, 1);
		run(r, s, 30);
		CHECK(r.recorder.of(13) == 0);
		CHECK(r.router->stats().culled >= 1);

		s[3].burst(r.render + 8 * kTick, 10, 3);
		run(r, s, 20);
		CHECK(r.router->stats().stolen == 1);
		CHECK(r.recorder.of(13) == 1);
		CHECK(r.router->stats().peak_weapon_voices == 3);
		run(r, s, 120);

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
		r.impacts->on_effect_sound(stop, *r.clock);
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
		CHECK(auditions == 10);
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
		s[0].burst(r.render + 10 * kTick, 1, 10);
		run(r, s, 5);
		check.begin_step(0, 5.0f);
		s[0].burst(r.render + 80 * kTick, 20, 10);
		s[0].burst(r.render + 140 * kTick, 40, 10);
		s[0].burst(r.render + 200 * kTick, 60, 10);
		run(r, s, 260);
		const std::span<const ba::GameCheckWindow> w = check.windows();
		REQUIRE(w.size() == 2);
		CHECK(w[0].stopped);
		CHECK(w[1].stopped);
		CHECK(w[0].stop_frame == w[0].first_frame + 9 * kIntervalFrames + kIntervalFrames / 2);
		CHECK(w[0].listener_distance_m == 5.0f);
		CHECK(w[0].probe_pitch >= 0.0f);
		CHECK(check.step_done(0) == (check.recorded_until() >= w[1].first_frame + uint64_t(4.0 * kRate)));
		CHECK(check.step_done(5));
		CHECK(check.recorded_until() > 0);
		CHECK(check.total_frames() == 4 * kRate);
		CHECK(check.dropped_frames() > 0);

		CHECK(!check.write_wav(std::filesystem::path("unused.wav")).ok());
		const ba::CheckReport rep = ba::analyse_game_check(small, check, ba::GameCheckCounters{}, ba::AudioTuning{}, kRate);
		CHECK(!rep.rows.empty());
		CHECK(rep.table().find("windows.d5") != std::string::npos);
		CHECK(rep.table().find("mixer.late_blocks") != std::string::npos);

		{
			ba::GameAudioCheck skipping(small, kRate, 0);
			skipping.begin_step(0, 5.0f);
			CHECK(!skipping.step_done(0));
			skipping.skip_step(0, "no such patch");
			skipping.skip_step(0, "twice");
			skipping.skip_step(9, "out of range");
			CHECK(skipping.step_done(0));
			REQUIRE(skipping.skipped().size() == 1);
			CHECK(skipping.skipped()[0].reason == "no such patch");
			const ba::CheckReport sr = ba::analyse_game_check(small, skipping, ba::GameCheckCounters{}, ba::AudioTuning{}, kRate);
			bool skipped_failed = false, windows_failed = false;
			for (const ba::CheckRow &row : sr.rows) {
				skipped_failed = skipped_failed || (row.name == "skipped.d5" && !row.pass);
				windows_failed = windows_failed || (row.name == "windows.d5" && !row.pass);
			}
			CHECK(skipped_failed);
			CHECK(windows_failed);
		}
		r.router->set_observer(nullptr);
		r.mixer->set_tap(&r.capture);
	}
}

namespace {

constexpr const char *kM16Patch = "sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02";
constexpr double kNsPerTick = 1e9 / 60.0;

struct GameFixture {
	InstallReader reader;
	InstallSnapshot before;
	std::shared_ptr<Install> install;
	std::unique_ptr<level::LoadedLevel> metro;
	std::shared_ptr<const host::GunplayLevel> gunplay;
	std::vector<std::string> impact_names;
	std::string impact_note;
	ba::GameAudioAssets assets;
	ba::AudioTuning tuning;
	double load_seconds = 0.0;
};

GameFixture *game_fixture() {
	static std::unique_ptr<GameFixture> f;
	static bool tried = false;
	if (tried) {
		return f.get();
	}
	tried = true;
	auto made = std::make_unique<GameFixture>();
	if (!made->reader.locate({})) {
		return nullptr;
	}
	const auto t0 = std::chrono::steady_clock::now();
	made->before = made->reader.snapshot();
	Result<std::shared_ptr<Install>> install = Install::open(made->reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
	made->install = *install;
	Result<std::unique_ptr<level::LoadedLevel>> lvl = level::load_level(made->install, "MP_Subway", "TeamDeathMatch0");
	REQUIRE_MESSAGE(lvl.ok(), (lvl.ok() ? std::string() : lvl.error().message));
	made->metro = std::move(*lvl);
	const std::string id = made->metro->info.id.empty() ? made->metro->root_name : made->metro->info.id;
	Result<std::shared_ptr<const host::GunplayLevel>> g = host::load_gunplay_level(made->install, id, "TeamDeathMatch0");
	REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
	made->gunplay = *g;

	const std::optional<std::string> grid = gameplay::MaterialGrid::grid_of_level(made->metro->db(), made->metro->root_name);
	if (grid) {
		gameplay::WorkBudget budget{ bf3::fx::kDefaultCompileWorkLimit };
		Result<bf3::fx::ImpactTable> table = bf3::fx::ImpactTable::load(made->metro->db(), *grid, budget);
		if (table) {
			made->impact_names = bf3::fx::impact_sound_names(*table);
		} else {
			made->impact_note = "impact table: " + table.error().message;
		}
	} else {
		made->impact_note = "impact table: no material grid for " + made->metro->root_name;
	}
	Result<ba::GameAudioAssets> a = ba::load_game_audio_assets(made->install, *made->metro, *made->gunplay, made->impact_names, 0);
	REQUIRE_MESSAGE(a.ok(), (a.ok() ? std::string() : a.error().message));
	made->assets = std::move(*a);
	std::ifstream cfg(OPENBF3_AUDIO_TUNING_CFG, std::ios::binary);
	REQUIRE(cfg.good());
	std::stringstream text;
	text << cfg.rdbuf();
	Result<ba::AudioTuning> tuning = ba::parse_audio_tuning(text.str());
	REQUIRE_MESSAGE(tuning.ok(), (tuning.ok() ? std::string() : tuning.error().message));
	made->tuning = *tuning;
	made->load_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	f = std::move(made);
	return f.get();
}

GameFixture *need_game_install() {
	GameFixture *f = game_fixture();
	if (f == nullptr) {
		bf3::test::skip_install_case("no BF3 install found");
	}
	return f;
}

void check_game_unchanged(const GameFixture &p_fixture) {
	const SnapshotDiff diff = diff_snapshots(p_fixture.before, p_fixture.reader.snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

std::string env_value(const char *p_name) {
	char *v = nullptr;
	size_t n = 0;
	if (_dupenv_s(&v, &n, p_name) != 0 || v == nullptr) {
		return {};
	}
	std::string out(v);
	std::free(v);
	return out;
}

std::filesystem::path au3_shots_dir(const char *p_row) {
	const std::string env = env_value("OPENBF3_AU3_SHOTS");
	return env.empty() ? std::filesystem::path(OPENBF3_AU3_SHOTS) / p_row : std::filesystem::path(env);
}

PathGuard install_guard(const GameFixture &p_fixture) {
	PathGuard g;
	REQUIRE(g.add_protected_root(p_fixture.reader.root()));
	return g;
}

void write_text(const PathGuard &p_guard, const std::filesystem::path &p_path, const std::string &p_text) {
	REQUIRE(p_guard.allows_write(p_path));
	std::ofstream out(p_path, std::ios::binary | std::ios::trunc);
	out << p_text;
	CHECK(out.good());
}

uint16_t def_of_patch(const ba::WeaponAudioTable &p_table, std::string_view p_patch) {
	for (size_t i = 0; i < p_table.defs.size(); ++i) {
		if (p_table.defs[i].patch == p_patch && p_table.defs[i].form == ba::WeaponAudioForm::Driver) {
			return uint16_t(i);
		}
	}
	return sw::kNoWeapon;
}

struct NetShooter {
	uint8_t slot = 1;
	uint16_t def = sw::kNoWeapon;
	sim::SoldierState state;
	std::vector<sw::SimTime> shots;
	std::vector<uint32_t> ids;
	size_t next_tick_scan = 0;
	size_t next_sent = 0;
	uint32_t burst_count = 0;
	uint32_t reload_count = 0;
	int64_t last_shot_tick = -100;
	bool previous_tick_shots = false;
	uint32_t next_id = 1;
	std::deque<net::TimedOther> received;
	void burst(sw::SimTime p_first, uint32_t p_rounds, sw::SimTime p_interval) {
		for (uint32_t k = 0; k < p_rounds; ++k) {
			shots.push_back(p_first + sw::SimTime(k) * p_interval);
			ids.push_back(next_id++);
		}
	}

	void server_tick(uint32_t p_tick) {
		bool any = false;
		while (next_tick_scan < shots.size() && sim::Tick(sw::tick_containing(shots[next_tick_scan])) <= p_tick) {
			any = any || sw::tick_containing(shots[next_tick_scan]) == p_tick;
			++next_tick_scan;
		}
		if (any) {
			if (!previous_tick_shots) {
				++burst_count;
			}
			last_shot_tick = p_tick;
		}
		previous_tick_shots = any;
	}
	net::OtherSoldier record(uint32_t p_tick) const {
		net::OtherSoldier o = net::quantize_other(slot, state);
		const bool firing = last_shot_tick > int64_t(p_tick) - 2 && last_shot_tick <= int64_t(p_tick);
		net::set_other_weapon(o, def, sw::WeaponPhase::Ready, firing, false, reload_count, burst_count);
		net::set_other_team(o, 2);
		return o;
	}
	sim::Vec3 eye() const {
		return { state.position[0], state.position[1] + 1.6f, state.position[2] };
	}
};

struct NetStream {
	double latency_ticks = 2.5;
	double delay_ticks = 4.0;
	uint32_t server_ran = 0;
	struct Pending {
		double arrive = 0.0;
		uint32_t tick = 0;
	};
	std::deque<Pending> in_flight;
	uint32_t newest_tick = 0;
	std::unique_ptr<host::PresentationFrame> frame = std::make_unique<host::PresentationFrame>();

	void advance(double p_t, std::vector<NetShooter> &r_shooters, std::vector<host::PresentEvent> &r_events) {
		while (double(server_ran + 1) <= p_t) {
			++server_ran;
			for (NetShooter &s : r_shooters) {
				s.server_tick(server_ran);
			}
			if (server_ran % 2 == 0) {
				in_flight.push_back(Pending{ double(server_ran) + latency_ticks, server_ran });
				for (NetShooter &s : r_shooters) {
					s.received.push_back(net::TimedOther{ server_ran, s.record(server_ran) });
				}
			}
		}
		while (!in_flight.empty() && in_flight.front().arrive <= p_t) {
			const uint32_t tick = in_flight.front().tick;
			in_flight.pop_front();
			newest_tick = tick;
			for (NetShooter &s : r_shooters) {
				while (s.next_sent < s.shots.size() && sim::Tick(sw::tick_containing(s.shots[s.next_sent])) <= tick) {
					host::PresentEvent e;
					e.kind = host::PresentKind::RemoteShot;
					e.timeline = host::PresentTimeline::Render;
					e.slot = s.slot;
					e.def = s.def;
					e.id = s.ids[s.next_sent];
					e.time = s.shots[s.next_sent];
					const sim::Vec3 eye = s.eye();
					e.pos[0] = eye[0];
					e.pos[1] = eye[1];
					e.pos[2] = eye[2];
					r_events.push_back(e);
					++s.next_sent;
				}
			}
		}
	}

	void publish(double p_t, uint64_t p_now_ns, const std::vector<NetShooter> &p_shooters) {
		host::PresentationFrame &f = *frame;
		f.valid = true;
		++f.serial;
		f.tick = sim::Tick(std::floor(p_t)) + 3;
		f.tick_fraction = p_t - std::floor(p_t);
		f.publish_ns = p_now_ns;
		f.render_ticks_at_publish = p_t - latency_ticks - delay_ticks;
		f.newest_tick = newest_tick;
		f.other_count = 0;
		for (const NetShooter &s : p_shooters) {
			if (f.other_count >= f.others.size()) {
				break;
			}
			host::OtherTrack &t = f.others[f.other_count];
			t.slot = s.slot;
			t.count = 0;

			size_t last = 0;
			bool any = false;
			for (size_t i = 0; i < s.received.size(); ++i) {
				if (s.received[i].server_tick <= newest_tick) {
					last = i;
					any = true;
				}
			}
			if (!any) {
				continue;
			}
			size_t first = 0;
			for (size_t i = 0; i <= last; ++i) {
				if (double(s.received[i].server_tick) <= f.render_ticks_at_publish) {
					first = i;
				}
			}
			for (size_t i = first; i <= last && t.count < host::kOtherSamples; ++i) {
				t.samples[t.count++] = s.received[i];
			}
			t.track_newest_tick = s.received[last].server_tick;
			++f.other_count;
		}
	}
	void trim(std::vector<NetShooter> &r_shooters) {
		for (NetShooter &s : r_shooters) {
			while (s.received.size() > 12) {
				s.received.pop_front();
			}
		}
	}
};

struct GameRig {
	const GameFixture &fx;
	std::unique_ptr<ba::MixerCore> mixer;
	std::unique_ptr<ba::OneShotPool> pool;
	std::unique_ptr<ba::SimMixerClock> clock;
	std::unique_ptr<ba::WeaponAudioRouter> router;
	std::unique_ptr<ba::ImpactAudio> impacts;
	std::unique_ptr<ba::GameAudioDriver> driver = std::make_unique<ba::GameAudioDriver>();
	std::vector<host::PresentedSoldier> soldiers = std::vector<host::PresentedSoldier>(net::kMaxSlots);
	std::vector<host::PresentedOtherWeapon> weapons = std::vector<host::PresentedOtherWeapon>(net::kMaxSlots);
	std::vector<float> buffer = std::vector<float>(size_t(kFramesPerTick) * 2);
	ba::ListenerState listener;
	host::PresentClock now;
	size_t presented = 0;

	GameRig(const GameFixture &p_fx, ba::MixTap *p_tap) :
			fx(p_fx) {
		ba::MixerConfig mc;
		mc.max_instances = ba::kGameMixerInstances;
		mc.command_capacity = 8192;
		mc.schedule_lead_blocks = uint32_t(std::max(1.0f, p_fx.tuning.schedule_lead_blocks));
		mixer = std::make_unique<ba::MixerCore>(mc);
		mixer->set_tap(p_tap);
		pool = std::make_unique<ba::OneShotPool>(ba::kMaxOneShotVoices);
		ba::SimMixerClockConfig cc;
		cc.bias_frames = int64_t(mc.schedule_lead_blocks) * int64_t(ba::kBlockFrames) + 2 * int64_t(ba::kBlockFrames) + 800;
		clock = std::make_unique<ba::SimMixerClock>(*mixer, cc);
		router = std::make_unique<ba::WeaponAudioRouter>(*mixer, p_fx.assets.table, p_fx.assets.bank, p_fx.tuning, *pool);
		impacts = std::make_unique<ba::ImpactAudio>(*mixer, p_fx.assets.impacts, p_fx.assets.bank, p_fx.tuning, *pool);
	}
	~GameRig() {
		router.reset();
		impacts.reset();
	}

	void frame(const host::PresentationFrame &p_frame, uint64_t p_now_ns, std::span<const host::PresentEvent> p_events, uint8_t p_own_slot,
			std::span<const ba::ImpactSoundRequest> p_impacts = {}) {
		now = host::present_clock(p_frame, p_now_ns);
		clock->set_now(now);
		presented = host::present_others(p_frame, p_now_ns, soldiers);
		const size_t nw = host::present_other_weapons(p_frame, host::present_render_ticks(p_frame, p_now_ns), weapons);
		ba::GameAudioInputs in;
		in.frame = &p_frame;
		in.now_ns = p_now_ns;
		in.events = p_events;
		in.others = std::span<const host::PresentedSoldier>(soldiers.data(), presented);
		in.other_weapons = std::span<const host::PresentedOtherWeapon>(weapons.data(), nw);
		in.listener = listener;
		in.own_slot = p_own_slot;
		in.own_first_person = true;
		driver->step(*router, impacts.get(), *clock, in, p_impacts);
		mixer->mix(buffer.data(), kFramesPerTick);
	}
	const host::PresentedSoldier *soldier(uint8_t p_slot) const {
		for (size_t i = 0; i < presented; ++i) {
			if (soldiers[i].slot == p_slot) {
				return &soldiers[i];
			}
		}
		return nullptr;
	}
};

ba::Vec3 standin_muzzle(const host::PresentedSoldier &p_s) {
	const float cp = std::cos(p_s.pitch), spi = std::sin(p_s.pitch), cy = std::cos(p_s.yaw), sy = std::sin(p_s.yaw);
	const float f = ba::WeaponAudioConfig{}.standin_muzzle_forward_m;
	return { p_s.position[0] + sy * cp * f, p_s.position[1] + p_s.eye_height + spi * f, p_s.position[2] + cy * cp * f };
}

float distance(const ba::Vec3 &p_a, const ba::Vec3 &p_b) {
	const float dx = p_a.x - p_b.x, dy = p_a.y - p_b.y, dz = p_a.z - p_b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool gate_late_blocks() {
	return env_value("OPENBF3_AU3_GATE_LATE") == "1";
}

struct GodotCameraBasis {
	ba::Vec3 x, y, z;
};

GodotCameraBasis godot_view_basis(float p_yaw) {
	const float a = p_yaw + 3.14159265358979323846f;
	return { { std::cos(a), 0.0f, -std::sin(a) }, { 0.0f, 1.0f, 0.0f }, { std::sin(a), 0.0f, std::cos(a) } };
}

ba::ListenerState listener_of(const GodotCameraBasis &p_basis, const ba::Vec3 &p_eye) {
	ba::ListenerState l;
	l.position = p_eye;
	l.forward = { -p_basis.z.x, -p_basis.z.y, -p_basis.z.z };
	l.up = p_basis.y;
	return l;
}

uint16_t yaw16_towards(const ba::Vec3 &p_from, const ba::Vec3 &p_to) {
	const double a = std::atan2(double(p_to.x - p_from.x), double(p_to.z - p_from.z));
	return uint16_t(int64_t(std::llround(a / (2.0 * 3.14159265358979323846) * 65536.0)) & 0xffff);
}

struct ChannelEnergy {
	double left_db = -300.0, right_db = -300.0;
};

ChannelEnergy channel_energy(std::span<const float> p_mix, uint64_t p_first, uint64_t p_end) {
	double l = 0.0, r = 0.0;
	const uint64_t end = std::min<uint64_t>(p_end, p_mix.size() / 2);
	for (uint64_t i = p_first; i < end; ++i) {
		l += double(p_mix[size_t(i) * 2]) * double(p_mix[size_t(i) * 2]);
		r += double(p_mix[size_t(i) * 2 + 1]) * double(p_mix[size_t(i) * 2 + 1]);
	}
	const double n = double(std::max<uint64_t>(end > p_first ? end - p_first : 0, 1));
	return { 10.0 * std::log10(l / n + 1e-30), 10.0 * std::log10(r / n + 1e-30) };
}
}

TEST_SUITE("audio-game") {
	TEST_CASE("kit items: Metro TDM's kit patches compile strict and bind (E7 item 1), impacts and auditions load") {
		GameFixture *f = need_game_install();
		if (f == nullptr) {
			return;
		}
		const ba::GameAudioAssets &a = f->assets;
		std::string report;
		for (const std::string &line : a.report) {
			report += "  " + line + "\n";
		}
		MESSAGE("load ", f->load_seconds, " s (branch, unlocked, shared machine: indicative only)\n", report, f->impact_note);
		uint32_t drivers = 0, one_shots = 0, none = 0, compiled = 0;
		for (const ba::KitAudioItem &item : a.kits.items) {
			REQUIRE(item.def < a.table->defs.size());
			const ba::WeaponAudioDef &d = a.table->defs[item.def];
			drivers += d.form == ba::WeaponAudioForm::Driver ? 1 : 0;
			one_shots += d.form == ba::WeaponAudioForm::OneShot ? 1 : 0;
			none += d.form == ba::WeaponAudioForm::None ? 1 : 0;
			compiled += d.program ? 1 : 0;
			CHECK_MESSAGE(d.form != ba::WeaponAudioForm::None, item.variant, ": ", d.error);
			CHECK((d.one_shot_event != 0 || d.form != ba::WeaponAudioForm::OneShot));
			const bool is_one_shot = d.form == ba::WeaponAudioForm::OneShot;
			CHECK(item.one_shot == is_one_shot);
		}
		CHECK(a.kits.items.size() == 12);
		CHECK(drivers == 10);
		CHECK(one_shots == 2);
		CHECK(none == 0);
		CHECK(compiled == 12);
		CHECK(a.table->failed == 0);
		const uint16_t m16 = def_of_patch(*a.table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);
		CHECK(a.table->defs[m16].fire_mode == ba::WeaponFireMode::Automatic);
		CHECK(std::fabs(a.table->defs[m16].shot_interval_s - 0.075) < 1e-6);
		CHECK(!f->impact_names.empty());
		CHECK(a.impacts->compiled() > 0);
		CHECK(a.bank);
		CHECK(a.waves_missing == 0);

		for (const ba::GameAuditionItem &item : ba::game_audition_items()) {
			const auto it = a.auditions.find(item.patch);
			REQUIRE_MESSAGE(it != a.auditions.end(), item.name, " not loaded");
			CHECK(it->second.program);
			REQUIRE(it->second.bank);
			CHECK(it->second.bank->stats().resident_waves > 0);
		}
		CHECK(a.audition_failures.empty());
		CHECK(a.audition_waves_missing == 0);
		const auto m1a2 = a.auditions.find(std::string_view("sound/vehicles/land/m1a2_abrams/m1a2_abrams"));
		REQUIRE(m1a2 != a.auditions.end());
		CHECK(m1a2->second.source == ba::kAuditionSuperbundles);
		CHECK(m1a2->second.bank != a.bank);
		check_game_unchanged(*f);
	}

	TEST_CASE(
			"catalog audio: every CUSTOMIZE weapon of Metro TDM's loadout catalog gets a weapon audio entry; a weapon "
			"patch with no DefaultStartEvent plays the first of Fire, Step and Melee its graph receives once per shot, "
			"Step for the seven shotguns, and that shot is heard") {
		GameFixture *f = need_game_install();
		if (f == nullptr) {
			return;
		}
		const std::string id = f->metro->info.id.empty() ? f->metro->root_name : f->metro->info.id;
		Result<std::shared_ptr<const host::GunplayLevel>> g =
				host::load_gunplay_level(f->install, id, "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
		REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
		const host::GunplayLevel &level = **g;
		REQUIRE_FALSE(level.loadouts.empty());
		Result<ba::GameAudioAssets> a = ba::load_game_audio_assets(f->install, *f->metro, level, f->impact_names, 0);
		REQUIRE_MESSAGE(a.ok(), (a.ok() ? std::string() : a.error().message));
		std::set<uint16_t> catalog;
		for (const auto &team : level.loadouts.kits) {
			for (const host::LoadoutKit &k : team) {
				for (const uint8_t slot : host::kKitSlots) {
					for (const host::LoadoutItem &item : k.slots[slot]) {
						if (item.def < level.defs.size()) {
							catalog.insert(item.def);
						}
					}
				}
			}
		}
		std::set<uint16_t> listed;
		std::set<std::string> unbound;
		for (const ba::KitAudioItem &item : a->kits.items) {
			listed.insert(item.def);
			if (a->table->defs[item.def].form == ba::WeaponAudioForm::None) {
				unbound.insert(item.variant);
			}
		}
		MESSAGE("catalog weapons ", catalog.size(), ", audio items ", a->kits.items.size(), ", unbound ",
				unbound.size(), ", waves missing ", a->waves_missing);
		CHECK(catalog.size() == 92);
		for (const uint16_t d : catalog) {
			CHECK_MESSAGE(listed.count(d) == 1, level.defs[d].variant);
		}
		CHECK(unbound.empty());
		CHECK(a->table->failed == 0);
		CHECK(a->waves_missing == 0);
		std::set<std::string> variant_sounds, other_form;
		const ba::KitAudioVariant *m16_suppressor = nullptr;
		for (const ba::KitAudioVariant &v : a->kits.variants) {
			REQUIRE(v.source < a->table->defs.size());
			REQUIRE(v.def < level.defs.size());
			const ba::WeaponAudioDef &d = a->table->defs[v.source];
			CHECK_MESSAGE(d.form != ba::WeaponAudioForm::None, v.unlock, " [", v.sound, "]: ", d.error);
			if (d.form != a->table->defs[v.def].form) {
				other_form.insert(v.unlock);
			}
			CHECK(d.program);
			CHECK(v.sound != level.defs[v.def].sound);
			variant_sounds.insert(v.sound);
			if (v.unlock.ends_with("/u_m16a4_sound_suppressor")) {
				m16_suppressor = &v;
			}
			bf3::net::SoldierGear gear;
			gear.def = v.def;
			gear.accessories = { 0, 0, v.accessory, 0 };
			CHECK(ba::accessory_sound_source(a->kits.variants, gear) != v.def);
		}
		MESSAGE("accessory sounds: ", a->kits.variants.size(), " (def, accessory) variants, ", variant_sounds.size(),
				" distinct patches",
				m16_suppressor != nullptr ? ", M16A4 suppressor " + m16_suppressor->sound : std::string());
		CHECK(a->kits.variants.size() == 68);
		CHECK(other_form ==
				std::set<std::string>{ "weapons/m1014/u_m1014_silencer", "weapons/remington870/u_870_silencer",
						"weapons/saiga20k/u_saiga_20k_silencer", "weapons/usas-12/u_usas-12_silencer",
						"weapons/xp2_spas12/u_spas12_silencer", "weapons/dao-12/u_dao-12_silencer",
						"weapons/xp1_jackhammer/u_jackhammer_silencer" });
		REQUIRE(m16_suppressor != nullptr);
		CHECK(m16_suppressor->sound.find("silenced") != std::string::npos);
		bf3::net::SoldierGear plain;
		plain.def = m16_suppressor->def;
		CHECK(ba::accessory_sound_source(a->kits.variants, plain) == plain.def);
		std::set<std::string> stepped;
		ba::MixerConfig mc;
		mc.max_instances = 4;
		for (const ba::KitAudioItem &item : a->kits.items) {
			const ba::WeaponAudioDef &d = a->table->defs[item.def];
			if (d.form != ba::WeaponAudioForm::OneShot || d.program->start_event != 0 || item.one_shot) {
				continue;
			}
			CHECK_MESSAGE(ba::event_reaches_graph(*d.program, d.one_shot_event), item.variant);
			if (d.one_shot_event != ba::kHashStep) {
				continue;
			}
			stepped.insert(item.variant);
			CHECK_MESSAGE(!ba::event_reaches_graph(*d.program, ba::kHashFire), item.variant);
			ba::MixerCore mixer(mc);
			ba::OneShotPool pool(1);
			ba::OneShotOptions o;
			o.event = d.one_shot_event;
			o.set_first_person = true;
			o.first_person = true;
			REQUIRE(pool.start(mixer, d.program, a->bank, f->tuning, 7, ba::EmitterState{}, ba::ListenerState{}, 0, o));
			std::vector<float> block(2 * ba::kBlockFrames);
			float peak = 0.0f;
			for (uint32_t b = 0; b < ba::kMixRate / 2 / ba::kBlockFrames; ++b) {
				mixer.mix(block.data(), ba::kBlockFrames);
				for (const float v : block) {
					peak = std::max(peak, std::fabs(v));
				}
			}
			MESSAGE(item.variant, " Step shot peak ", peak);
			CHECK_MESSAGE(peak > 1e-3f, item.variant);
		}
		CHECK(stepped ==
				std::set<std::string>{ "weapons/m1014/u_m1014", "weapons/remington870/u_870",
						"weapons/saiga20k/u_saiga_20k", "weapons/usas-12/u_usas-12", "weapons/xp2_spas12/u_spas12",
						"weapons/dao-12/u_dao-12", "weapons/xp1_jackhammer/u_jackhammer" });
		check_game_unchanged(*f);
	}

	TEST_CASE("E7 rehearsal: an M16A3 shooter through the cl presentation stream, GameAudioCheck, analyse_game_check") {
		GameFixture *f = need_game_install();
		if (f == nullptr) {
			return;
		}
		const auto t0 = std::chrono::steady_clock::now();
		const ba::WeaponAudioTable &table = *f->assets.table;
		const uint16_t m16 = def_of_patch(table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);

		ba::GameCheckScript script = ba::game_check_script();
		std::string skips;
		script.analysis.shot_interval_s = table.defs[m16].shot_interval_s;
		script.analysis.expected_spacing_frames = table.defs[m16].shot_interval_s * double(ba::kMixRate);
		const sw::SimTime interval = sw::SimTime(std::llround(table.defs[m16].shot_interval_s * double(sw::kTimeUnitsPerSecond)));
		ba::GameAudioCheck check(script, ba::kMixRate, 0);
		const PathGuard guard = install_guard(*f);
		check.set_guard(guard);
		GameRig rig(*f, &check);
		rig.router->set_observer(&check);
		constexpr uint8_t kOwn = 0, kShooter = 1;
		check.set_shooter(kShooter);
		std::vector<NetShooter> shooters(1);
		NetShooter &sh = shooters[0];
		sh.slot = kShooter;
		sh.def = m16;
		sh.state.life = 1;
		sh.state.position = { 0.0f, 0.0f, 0.0f };
		sh.state.yaw = 0;
		NetStream net;
		const double t_start = 600.25;
		const uint32_t settle_frames = uint32_t(std::llround(script.settle_s * 60.0));
		const uint32_t start_frames = uint32_t(std::llround(script.analysis.start_s * 60.0));
		const uint32_t period_frames = uint32_t(std::llround(script.burst_period_s * 60.0));
		uint64_t f_index = 0;
		int64_t step = -1;
		uint64_t step_frame = 0;
		bool audition_started = false;
		uint32_t auditions = 0;
		const uint64_t max_frames = uint64_t(script.timeout_s * 60.0) + 600;
		std::vector<host::PresentEvent> events;
		std::vector<std::string> placed;
		bool done = false;
		for (; f_index < max_frames && !done; ++f_index) {
			const double t = t_start + double(f_index);
			const uint64_t now_ns = uint64_t(std::llround(t * kNsPerTick));
			events.clear();
			net.advance(t, shooters, events);
			net.publish(t, now_ns, shooters);

			if ((step < 0 && f_index >= 60) || (step >= 0 && check.step_done(uint32_t(step)))) {
				++step;
				if (size_t(step) >= script.steps.size()) {
					done = true;
					break;
				}
				const ba::GameCheckStep &st = script.steps[size_t(step)];
				const host::PresentedSoldier *s = rig.soldier(kShooter);
				REQUIRE(s != nullptr);
				const ba::Vec3 muzzle = standin_muzzle(*s);
				rig.listener.position = { muzzle.x, muzzle.y, muzzle.z + st.distance_m };
				rig.listener.forward = { 0.0f, 0.0f, -1.0f };
				rig.listener.up = { 0.0f, 1.0f, 0.0f };
				const float d = distance(rig.listener.position, muzzle);
				check.begin_step(uint32_t(step), d);
				step_frame = f_index;
				audition_started = false;
				placed.push_back(st.name + " " + std::to_string(d));
				if (!st.audition) {
					for (uint32_t b = 0; b < st.bursts; ++b) {
						const double first = t + double(settle_frames + start_frames + b * period_frames);
						sh.burst(sw::SimTime(std::llround(first * double(sw::kTimeUnitsPerTick))), script.rounds, interval);
					}
				}
			}
			rig.frame(*net.frame, now_ns, events, kOwn);
			if (step >= 0 && script.steps[size_t(step)].audition && !audition_started && f_index >= step_frame + settle_frames) {
				const ba::GameCheckStep &st = script.steps[size_t(step)];
				const host::PresentedSoldier *s = rig.soldier(kShooter);
				REQUIRE(s != nullptr);
				const uint64_t seed = ba::sound_seed(0xA0D1u, auditions);
				const auto it = f->assets.auditions.find(st.patch);
				if (it == f->assets.auditions.end()) {
					check.skip_step(uint32_t(step), "not loaded");
					skips += " " + st.name;
				} else {
					CHECK(ba::start_audition(*rig.pool, *rig.mixer, it->second.program, it->second.bank, f->tuning, rig.listener, standin_muzzle(*s), seed,
							rig.clock->earliest_frame(), &check));
					++auditions;
				}
				audition_started = true;
			}
			net.trim(shooters);
		}
		const double run_s = double(f_index) / 60.0;
		CHECK(done);

		const ba::MixerStats &ms = rig.mixer->stats();
		ba::GameCheckCounters counters;
		counters.short_blocks = ms.short_blocks.load();
		counters.late_blocks = ms.late_blocks.load();
		counters.late_commands = ms.late_commands.load();
		counters.mixer_refusals = rig.router->stats().mixer_refusals;
		const ba::CheckReport rep = ba::analyse_game_check(script, check, counters, f->tuning, ba::kMixRate);
		const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::string table_text = rep.table();
		MESSAGE("E7 rehearsal: ", run_s, " s of game time, ", rep.rows.size(), " rows, audition skips:", (skips.empty() ? " none" : skips), "; wall ",
				wall_s, " s (branch, unlocked, shared machine: indicative only)\n", table_text);
		size_t failed = 0;
		for (const ba::CheckRow &r : rep.rows) {
			if (r.name == "mixer.late_blocks" && !gate_late_blocks()) {
				continue;
			}
			if (!r.pass) {
				++failed;
				MESSAGE("FAIL ", r.name, " ", r.value, " ", r.rule);
			}
		}
		CHECK(failed == 0);
		CHECK(skips.empty());
		CHECK(check.skipped().empty());
		CHECK(auditions == 10);
		CHECK(run_s <= script.timeout_s);
		CHECK(check.dropped_frames() == 0);
		CHECK(rig.router->stats().mixer_refusals == 0);
		CHECK(ms.ring_overflows.load() == 0);
		CHECK(ms.starved_voices.load() == 0);

		const std::filesystem::path dir = au3_shots_dir("audio.game");
		std::error_code ec;
		REQUIRE(guard.allows_write(dir));
		std::filesystem::create_directories(dir, ec);
		const Status wav = check.write_wav(dir / "e7_rehearsal.wav");
		CHECK_MESSAGE(wav.ok(), (wav.ok() ? std::string() : wav.error().message));
		std::string placed_text;
		for (const std::string &p : placed) {
			placed_text += p + "\n";
		}
		placed_text += "windows (step, seed, first frame, stop frame; relative to the WAV start):\n";
		for (const ba::GameCheckWindow &w : check.windows()) {
			placed_text += script.steps[w.step].name + " " + std::to_string(w.seed) + " " + std::to_string(w.first_frame - check.start_frame()) + " " +
					std::to_string(w.stop_frame - check.start_frame()) + "\n";
		}
		write_text(guard, dir / "e7_rehearsal.txt", table_text + "\nsteps (name, listener distance m):\n" + placed_text);
		check_game_unchanged(*f);
	}

	TEST_CASE("a scene for the ear: own fire and reload, the M16A3 at 15 / 80 / 250 m, an SVD, impacts (gate 4 offline)") {
		GameFixture *f = need_game_install();
		if (f == nullptr) {
			return;
		}
		const ba::WeaponAudioTable &table = *f->assets.table;
		const uint16_t m16 = def_of_patch(table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);
		uint16_t svd = sw::kNoWeapon;
		for (const ba::KitAudioItem &item : f->assets.kits.items) {
			if (item.sound.find("svd") != std::string::npos && table.defs[item.def].form == ba::WeaponAudioForm::Driver) {
				svd = item.def;
			}
		}
		REQUIRE(svd != sw::kNoWeapon);
		struct Whole final : ba::MixTap {
			std::vector<float> mix;
			void on_block(const float *p, uint32_t n, uint64_t) override {
				mix.insert(mix.end(), p, p + size_t(n) * 2);
			}
		} tap;
		struct Starts final : ba::WeaponAudioObserver {
			std::vector<std::pair<uint8_t, uint64_t>> first;
			void on_driver_instance(uint8_t p_slot, uint64_t, uint64_t p_first, ba::PatchInstance &) override {
				first.push_back({ p_slot, p_first });
			}
			void on_trigger_up(uint8_t, uint64_t, uint64_t) override {}
		} starts;
		GameRig rig(*f, &tap);
		rig.router->set_observer(&starts);
		rig.listener.position = { 0.0f, 1.6f, 0.0f };
		rig.listener.forward = { 0.0f, 0.0f, 1.0f };
		const double t_start = 600.25;
		const auto at = [&](double p_s) { return sw::SimTime(std::llround((t_start + p_s * 60.0) * double(sw::kTimeUnitsPerTick))); };
		const sw::SimTime m16_iv = sw::SimTime(std::llround(table.defs[m16].shot_interval_s * double(sw::kTimeUnitsPerSecond)));

		std::vector<NetShooter> shooters(4);
		const float dist[3] = { 15.0f, 80.0f, 250.0f };
		for (int i = 0; i < 3; ++i) {
			shooters[i].slot = uint8_t(1 + i);
			shooters[i].def = m16;
			shooters[i].state.life = 1;
			shooters[i].state.position = { 0.0f, 0.0f, dist[i] + 0.5f };
			shooters[i].state.yaw = 32768;
			shooters[i].burst(at(4.0 + 3.0 * i), 10, m16_iv);
		}
		shooters[3].slot = 4;
		shooters[3].def = svd;
		shooters[3].state.life = 1;
		shooters[3].state.position = { 40.0f, 0.0f, 0.0f };
		shooters[3].state.yaw = 49152;
		for (int k = 0; k < 3; ++k) {
			shooters[3].burst(at(13.0 + 0.5 * k), 1, m16_iv);
		}

		std::vector<sw::SimTime> own;
		for (int k = 0; k < 8; ++k) {
			own.push_back(at(1.0) + sw::SimTime(k) * m16_iv);
		}
		for (int k = 0; k < 8; ++k) {
			own.push_back(at(18.0) + sw::SimTime(k) * m16_iv);
		}
		const sw::SimTime own_reload = at(2.2);
		bool reload_sent = false;
		std::vector<std::string_view> impact_names;
		for (const std::string &n : f->impact_names) {
			if (f->assets.impacts->find(n)) {
				impact_names.push_back(n);
			}
		}
		REQUIRE(!impact_names.empty());
		NetStream net;
		std::vector<host::PresentEvent> events;
		std::vector<ba::ImpactSoundRequest> impacts;
		size_t own_next = 0;
		uint32_t own_id = 1;
		std::vector<uint64_t> impact_frames;
		const uint64_t frames = 24 * 60;
		for (uint64_t fi = 0; fi < frames; ++fi) {
			const double t = t_start + double(fi);
			const uint64_t now_ns = uint64_t(std::llround(t * kNsPerTick));
			events.clear();
			impacts.clear();
			net.advance(t, shooters, events);
			net.publish(t, now_ns, shooters);
			const host::PresentClock c = host::present_clock(*net.frame, now_ns);
			while (own_next < own.size() && own[own_next] <= c.predicted) {
				host::PresentEvent e;
				e.kind = host::PresentKind::OwnShot;
				e.timeline = host::PresentTimeline::Predicted;
				e.def = m16;
				e.id = own_id++;
				e.time = own[own_next++];
				events.push_back(e);
			}
			if (!reload_sent && own_reload <= c.predicted) {
				host::PresentEvent e;
				e.kind = host::PresentKind::OwnTimed;
				e.timeline = host::PresentTimeline::Predicted;
				e.def = m16;
				e.id = sw::weapon_events::kReloadStarted;
				e.time = own_reload;
				events.push_back(e);
				reload_sent = true;
			}
			const bool trigger = own_next > 0 && own_next < own.size() && own[own_next] - own[own_next - 1] <= m16_iv;
			net.frame->own_weapons_valid = true;
			net.frame->own_weapons_curr.timers[0].flags = trigger ? sw::slot_flags::kTriggerDown : 0;

			if (fi >= 16 * 60 && fi < 16 * 60 + 90 && (fi - 16 * 60) % 18 == 0) {
				ba::ImpactSoundRequest r;
				r.patch = impact_names[(fi / 18) % impact_names.size()];
				r.position = { -2.0f + 0.8f * float((fi - 16 * 60) / 18), 0.0f, 3.0f + float((fi - 16 * 60) / 18) };
				r.seed = fi;
				r.time = c.render;
				r.timeline = host::PresentTimeline::Render;
				impacts.push_back(r);
				impact_frames.push_back(rig.clock->frame_of(c.render, host::PresentTimeline::Render));
			}
			rig.frame(*net.frame, now_ns, events, 0, impacts);
			net.trim(shooters);
		}
		const ba::WeaponAudioStats &ws = rig.router->stats();
		CHECK(ws.own_bursts == 2);
		CHECK(ws.reloads == 1);
		CHECK(ws.bursts == 2 + 3 + 3);
		CHECK(ws.mixer_refusals == 0);
		CHECK(ws.state_rounds == 0);
		CHECK(rig.impacts->stats().impacts == 5);

		const auto loudest = [&](uint64_t p_first, double p_seconds) {
			const uint64_t len = uint64_t(0.02 * ba::kMixRate);
			double best = -300.0;
			const uint64_t end = std::min<uint64_t>(p_first + uint64_t(p_seconds * ba::kMixRate), tap.mix.size() / 2);
			for (uint64_t a = p_first; a + len <= end; a += len / 2) {
				best = std::max(best, double(ba::rms_db(tap.mix, 2, a, len)));
			}
			return best;
		};
		double own_db = -300.0, at_db[4] = { -300.0, -300.0, -300.0, -300.0 };
		bool own_seen = false;
		for (const auto &[slot, first] : starts.first) {
			const double db = loudest(first, 1.0);
			if (slot == 0 && !own_seen) {
				own_db = db;
				own_seen = true;
			} else if (slot >= 1 && slot <= 4 && at_db[slot - 1] == -300.0) {
				at_db[slot - 1] = db;
			}
		}
		REQUIRE(!impact_frames.empty());
		const double impacts_db = loudest(impact_frames.front(), 1.5);
		char line[400];
		std::snprintf(line, sizeof(line),
				"scene: own M16A3 (1p) %.1f, M16A3 at 15 m %.1f, 80 m %.1f, 250 m %.1f, SVD at 40 m %.1f, impacts %.1f dBFS (loudest 20 ms, unity master)",
				own_db, at_db[0], at_db[1], at_db[2], at_db[3], impacts_db);
		MESSAGE(line);
		CHECK(own_db > at_db[1]);
		CHECK(at_db[0] > at_db[1]);
		CHECK(at_db[1] > at_db[2]);
		for (double db : { own_db, at_db[0], at_db[1], at_db[2], at_db[3], impacts_db }) {
			CHECK(db >= -110.0);
		}

		const PathGuard guard = install_guard(*f);
		const std::filesystem::path dir = au3_shots_dir("audio.game");
		REQUIRE(guard.allows_write(dir));
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const Status wav = ba::write_wav(guard, dir / "scene.wav", tap.mix, 2, ba::kMixRate);
		CHECK_MESSAGE(wav.ok(), (wav.ok() ? std::string() : wav.error().message));
		write_text(guard, dir / "scene.txt",
				std::string(line) + "\n0-1 s silence; 1 s own burst (8); 2.2 s own reload; 4 / 7 / 10 s M16A3 at 15 / 80 / 250 m (10 rounds);"
				" 13 s SVD at 40 m (3 shots); 16 s five impacts 3-8 m; 18 s own burst (8)\n");
		rig.router->set_observer(nullptr);
		check_game_unchanged(*f);
	}

	TEST_CASE("stereo: in the Godot camera's pose an M16A3 shooter and an impact on the camera's x side are louder on "
			  "the right channel, on its -x side on the left") {
		GameFixture *f = need_game_install();
		if (f == nullptr) {
			return;
		}
		const ba::WeaponAudioTable &table = *f->assets.table;
		const uint16_t m16 = def_of_patch(table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);
		std::string impact;
		for (const std::string &n : f->impact_names) {
			if (f->assets.impacts->find(n)) {
				impact = n;
				break;
			}
		}
		REQUIRE(!impact.empty());
		struct Whole final : ba::MixTap {
			std::vector<float> mix;
			void on_block(const float *p, uint32_t n, uint64_t) override {
				mix.insert(mix.end(), p, p + size_t(n) * 2);
			}
		};
		const double t_start = 600.25;
		const auto at = [&](double p_s) {
			return sw::SimTime(std::llround((t_start + p_s * 60.0) * double(sw::kTimeUnitsPerTick)));
		};
		const sw::SimTime m16_iv =
				sw::SimTime(std::llround(table.defs[m16].shot_interval_s * double(sw::kTimeUnitsPerSecond)));
		const uint64_t shots_end = uint64_t(2.6 * ba::kMixRate), impact_first = uint64_t(2.8 * ba::kMixRate),
					   impact_end = uint64_t(4.4 * ba::kMixRate);
		for (const float yaw : { 0.0f, 1.1f, -2.4f }) {
			const GodotCameraBasis cam = godot_view_basis(yaw);
			for (const float side : { 1.0f, -1.0f }) {
				Whole tap;
				GameRig rig(*f, &tap);
				const ba::Vec3 eye{ 0.0f, 1.6f, 0.0f };
				rig.listener = listener_of(cam, eye);
				std::vector<NetShooter> shooters(1);
				NetShooter &s = shooters[0];
				s.slot = 1;
				s.def = m16;
				s.state.life = 1;
				const ba::Vec3 feet{ eye.x + side * 12.0f * cam.x.x, 0.0f, eye.z + side * 12.0f * cam.x.z };
				s.state.position = { feet.x, feet.y, feet.z };
				s.state.yaw = yaw16_towards(feet, eye);
				s.burst(at(1.0), 6, m16_iv);
				NetStream net;
				std::vector<host::PresentEvent> events;
				std::vector<ba::ImpactSoundRequest> impacts;
				bool impact_sent = false;
				for (uint64_t fi = 0; fi < uint64_t(4.5 * 60); ++fi) {
					const double t = t_start + double(fi);
					const uint64_t now_ns = uint64_t(std::llround(t * kNsPerTick));
					events.clear();
					impacts.clear();
					net.advance(t, shooters, events);
					net.publish(t, now_ns, shooters);
					const host::PresentClock c = host::present_clock(*net.frame, now_ns);
					if (!impact_sent && fi >= uint64_t(3.0 * 60)) {
						ba::ImpactSoundRequest r;
						r.patch = impact;
						r.position = { eye.x + side * 5.0f * cam.x.x, 0.0f, eye.z + side * 5.0f * cam.x.z };
						r.seed = 7;
						r.time = c.render;
						r.timeline = host::PresentTimeline::Render;
						impacts.push_back(r);
						impact_sent = true;
					}
					rig.frame(*net.frame, now_ns, events, 0, impacts);
					net.trim(shooters);
				}
				CHECK(rig.router->stats().bursts == 1);
				CHECK(rig.impacts->stats().impacts == 1);
				const ChannelEnergy shots = channel_energy(tap.mix, 0, shots_end);
				const ChannelEnergy hit = channel_energy(tap.mix, impact_first, impact_end);
				const double shots_lean = shots.right_db - shots.left_db, hit_lean = hit.right_db - hit.left_db;
				MESSAGE("yaw " << yaw << " side " << side << ": M16A3 at 12 m right - left " << shots_lean << " dB ("
							   << shots.left_db << " / " << shots.right_db << "), " << impact << " at 5 m " << hit_lean
							   << " dB");
				CHECK(std::max(shots.left_db, shots.right_db) > -70.0);
				CHECK(std::max(hit.left_db, hit.right_db) > -70.0);
				CHECK(side * shots_lean > 6.0);
				CHECK(side * hit_lean > 6.0);
			}
		}
		const WaveBank &bank = *f->assets.bank;
		std::map<uint32_t, size_t> by_channels;
		for (WaveId w = 0; w < WaveId(bank.waves().size()); ++w) {
			for (const WaveVariation &var : bank.waves()[w].variations) {
				const std::span<const uint8_t> chunk = bank.chunk(w, var.chunk_index);
				StreamDecoder d;
				if (!chunk.empty() && !var.segments.empty() && d.open(chunk, var.segments[0].samples_offset).ok()) {
					++by_channels[d.header().channels];
					break;
				}
			}
		}
		std::string census;
		for (const auto &[channels, n] : by_channels) {
			census += " " + std::to_string(n) + " with " + std::to_string(channels);
		}
		MESSAGE("Metro TDM bank waves by channel count:" << census);
		check_game_unchanged(*f);
	}

	TEST_CASE("64 shooters for 60 s: the voice limit, the one-shot pool and impacts under load, no refusal or overflow") {
		GameFixture *f = need_game_install();
		if (f == nullptr) {
			return;
		}
		const ba::WeaponAudioTable &table = *f->assets.table;
		std::vector<uint16_t> drivers, one_shots;
		for (const ba::KitAudioItem &item : f->assets.kits.items) {
			const ba::WeaponAudioForm form = table.defs[item.def].form;
			(form == ba::WeaponAudioForm::Driver ? drivers : one_shots).push_back(item.def);
		}
		REQUIRE(!drivers.empty());
		const uint16_t m16 = def_of_patch(table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);
		std::vector<std::string_view> impact_names;
		for (const std::string &n : f->impact_names) {
			if (f->assets.impacts->find(n)) {
				impact_names.push_back(n);
			}
		}
		struct Silent final : ba::MixTap {
			double sum = 0.0;
			uint64_t frames = 0, non_finite = 0;
			float peak = 0.0f;
			void on_block(const float *p, uint32_t n, uint64_t) override {
				for (uint32_t i = 0; i < n * 2; ++i) {
					if (!std::isfinite(p[i])) {
						++non_finite;
						continue;
					}
					sum += double(p[i]) * double(p[i]);
					peak = std::max(peak, std::fabs(p[i]));
				}
				frames += n;
			}
		} tap;
		GameRig rig(*f, &tap);

		{
			std::vector<std::shared_ptr<const SoundProgram>> programs;
			for (const ba::WeaponAudioDef &d : table.defs) {
				programs.push_back(d.program);
				programs.push_back(d.explosion);
			}
			const std::vector<std::shared_ptr<const SoundProgram>> more = f->assets.impacts->programs();
			programs.insert(programs.end(), more.begin(), more.end());
			const ba::MixerA3Assets a3 = ba::load_mixer_a3(f->metro->db(), programs);
			REQUIRE_MESSAGE(a3.hdr.has_value(), a3.notes);
			REQUIRE_MESSAGE(a3.scopes != nullptr, a3.notes);
			MESSAGE("A3: " << a3.scopes->scopes.size() << " scopes, " << a3.scoped_patches << " of " << programs.size() << " programs scoped");
			rig.router.reset();
			rig.impacts.reset();
			rig.clock.reset();
			rig.pool.reset();
			ba::MixerConfig mc;
			mc.max_instances = ba::kGameMixerInstances;
			mc.command_capacity = 8192;
			mc.schedule_lead_blocks = uint32_t(std::max(1.0f, f->tuning.schedule_lead_blocks));
			mc.max_voices = ba::kGameMixerVoices;
			ba::apply_mixer_a3(a3, mc);
			if (const std::string fade = env_value("OPENBF3_PT1_CUT_FADE"); !fade.empty()) {
				mc.cut_fade_frames = uint32_t(std::clamp(std::atoi(fade.c_str()), 0, 4800));
			}
			if (env_value("OPENBF3_PT1_CAP_HOLD") == "0") {
				mc.cap_hold_db = 0.0f;
				mc.voice_hold_blocks = 0;
				mc.resume_margin = 0;
			}
			if (env_value("OPENBF3_PT1_LIMIT") == "0") {
				mc.limit_ceiling = 0.0f;
			}
			if (const std::string m = env_value("OPENBF3_PT1_RESUME_MARGIN"); !m.empty()) {
				mc.resume_margin = uint32_t(std::clamp(std::atoi(m.c_str()), 0, 128));
			}
			rig.mixer = std::make_unique<ba::MixerCore>(mc);
			rig.mixer->set_tap(&tap);
			rig.pool = std::make_unique<ba::OneShotPool>(ba::kMaxOneShotVoices);
			ba::SimMixerClockConfig cc;
			cc.bias_frames = int64_t(mc.schedule_lead_blocks) * int64_t(ba::kBlockFrames) + 2 * int64_t(ba::kBlockFrames) + 800;
			rig.clock = std::make_unique<ba::SimMixerClock>(*rig.mixer, cc);
			rig.router = std::make_unique<ba::WeaponAudioRouter>(*rig.mixer, f->assets.table, f->assets.bank, f->tuning, *rig.pool);
			rig.impacts = std::make_unique<ba::ImpactAudio>(*rig.mixer, f->assets.impacts, f->assets.bank, f->tuning, *rig.pool);
		}
		rig.listener.position = { 0.0f, 1.6f, 0.0f };
		std::mt19937_64 rng(0xA03u);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		const double t_start = 600.25;
		constexpr int kSeconds = 60;
		constexpr uint32_t kShooters = 63;
		std::vector<NetShooter> shooters(kShooters);
		for (uint32_t i = 0; i < kShooters; ++i) {
			NetShooter &s = shooters[i];
			s.slot = uint8_t(1 + i);
			s.state.life = 1;

			const float r = i % 2 == 0 ? 3.0f + 97.0f * unit(rng) : 100.0f + 800.0f * unit(rng);
			const float a = 6.2831853f * unit(rng);
			s.state.position = { r * std::sin(a), 0.0f, r * std::cos(a) };
			s.state.yaw = uint16_t(rng());

			const bool one_shot = !one_shots.empty() && i % 9 == 4;
			s.def = one_shot ? one_shots[i % one_shots.size()] : drivers[i % drivers.size()];
			const double iv = one_shot ? 1.0 : std::max(0.06, table.defs[s.def].shot_interval_s);
			const sw::SimTime interval = sw::SimTime(std::llround(iv * double(sw::kTimeUnitsPerSecond)));
			double t = t_start + 30.0 + 120.0 * unit(rng);
			while (t < t_start + kSeconds * 60.0 - 120.0) {

				const bool automatic = table.defs[s.def].fire_mode != ba::WeaponFireMode::Single;
				const uint32_t rounds = one_shot ? 1u : (automatic ? 5u + uint32_t(unit(rng) * 50.0f) : 1u + uint32_t(unit(rng) * 20.0f));
				s.burst(sw::SimTime(std::llround(t * double(sw::kTimeUnitsPerTick))), rounds, interval);
				t += double(rounds) * iv * 60.0 + 12.0 + 50.0 * unit(rng);
			}
		}

		std::vector<sw::SimTime> own_shots;
		for (double t = t_start + 40.0; t < t_start + kSeconds * 60.0 - 120.0;) {
			const uint32_t rounds = 3 + uint32_t(unit(rng) * 15.0f);
			for (uint32_t k = 0; k < rounds; ++k) {
				own_shots.push_back(sw::SimTime(std::llround((t + double(k) * 4.5) * double(sw::kTimeUnitsPerTick))));
			}
			t += double(rounds) * 4.5 + 30.0 + 90.0 * unit(rng);
		}

		rig.mixer->record_cuts(true);
		std::vector<ba::MixerCutEvent> pt1_cuts;
		const auto drain_cuts = [&] {
			std::array<ba::MixerCutEvent, 512> batch;
			for (size_t n = rig.mixer->poll_cuts(batch); n > 0; n = rig.mixer->poll_cuts(batch)) {
				pt1_cuts.insert(pt1_cuts.end(), batch.begin(), batch.begin() + std::ptrdiff_t(n));
			}
		};
		NetStream net;
		std::vector<host::PresentEvent> events;
		std::vector<ba::ImpactSoundRequest> impacts;
		size_t own_next = 0;
		uint32_t own_id = 1;
		uint64_t reloads = 0;
		const auto t0 = std::chrono::steady_clock::now();
		const uint64_t frames = uint64_t(kSeconds) * 60;
		for (uint64_t fi = 0; fi < frames; ++fi) {
			const double t = t_start + double(fi);
			const uint64_t now_ns = uint64_t(std::llround(t * kNsPerTick));
			events.clear();
			impacts.clear();

			if (fi % 97 == 0) {
				NetShooter &s = shooters[(fi / 97) % kShooters];
				++s.reload_count;
				++reloads;
			}
			net.advance(t, shooters, events);
			net.publish(t, now_ns, shooters);

			const host::PresentClock c = host::present_clock(*net.frame, now_ns);
			bool trigger = false;
			while (own_next < own_shots.size() && own_shots[own_next] <= c.predicted) {
				host::PresentEvent e;
				e.kind = host::PresentKind::OwnShot;
				e.timeline = host::PresentTimeline::Predicted;
				e.slot = 0;
				e.def = m16;
				e.id = own_id++;
				e.time = own_shots[own_next];
				events.push_back(e);
				++own_next;
			}
			if (own_next < own_shots.size() && own_next > 0 && own_shots[own_next] - own_shots[own_next - 1] < 5 * sw::kTimeUnitsPerTick) {
				trigger = true;
			}
			net.frame->own_weapons_valid = true;
			net.frame->own_weapons_curr.timers[0].flags = trigger ? sw::slot_flags::kTriggerDown : 0;

			if (!impact_names.empty() && fi % 3 == 0) {
				ba::ImpactSoundRequest r;
				r.patch = impact_names[size_t(rng() % impact_names.size())];
				r.position = { 60.0f * (unit(rng) - 0.5f), 0.0f, 60.0f * (unit(rng) - 0.5f) };
				r.seed = rng();
				r.time = c.render;
				r.timeline = host::PresentTimeline::Render;
				impacts.push_back(r);
			}
			rig.frame(*net.frame, now_ns, events, 0, impacts);
			drain_cuts();
			net.trim(shooters);
		}

		for (uint64_t fi = frames; fi < frames + 6 * 60; ++fi) {
			const double t = t_start + double(fi);
			const uint64_t now_ns = uint64_t(std::llround(t * kNsPerTick));
			events.clear();
			net.advance(t, shooters, events);
			net.publish(t, now_ns, shooters);
			net.frame->own_weapons_curr.timers[0].flags = 0;
			rig.frame(*net.frame, now_ns, events, 0);
			drain_cuts();
		}
		const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		const ba::MixerStats &ms = rig.mixer->stats();
		const ba::WeaponAudioStats &ws = rig.router->stats();
		const ba::ImpactAudioStats &is = rig.impacts->stats();
		const double rms_db = tap.frames > 0 ? 10.0 * std::log10(std::max(1e-30, tap.sum / double(tap.frames * 2))) : -999.0;
		char line[1200];
		std::snprintf(line, sizeof(line),
				"64 shooters, %d s + 6 s tails: wall %.2f s, %.1fx real time, max block %.3f ms of %.3f ms, late_blocks %llu (branch, unlocked, shared "
				"machine: indicative only)\n  mixer: blocks %llu short %llu late_commands %llu ring_overflows %llu starved %llu decode_errors %llu "
				"feedback_drops %llu peak_instances %u\n  router: bursts %llu culled %llu stolen %llu event_rounds %llu state_rounds %llu reloads %llu "
				"one_shots %llu refusals %llu\n  pool: peak %u stolen %llu failed %llu; impacts %llu unknown %llu; reload records %llu\n  mix: rms %.1f dBFS "
				"peak %.3f non-finite %llu",
				kSeconds, wall_s, double(kSeconds + 6) / std::max(1e-9, wall_s), double(ms.max_block_ns.load()) / 1e6, 512.0 / 48.0,
				(unsigned long long)ms.late_blocks.load(), (unsigned long long)ms.blocks.load(), (unsigned long long)ms.short_blocks.load(),
				(unsigned long long)ms.late_commands.load(), (unsigned long long)ms.ring_overflows.load(), (unsigned long long)ms.starved_voices.load(),
				(unsigned long long)ms.decode_errors.load(), (unsigned long long)ms.feedback_drops.load(), unsigned(ms.peak_instances.load()),
				(unsigned long long)ws.bursts, (unsigned long long)ws.culled, (unsigned long long)ws.stolen, (unsigned long long)ws.event_rounds,
				(unsigned long long)ws.state_rounds, (unsigned long long)ws.reloads, (unsigned long long)ws.one_shots, (unsigned long long)ws.mixer_refusals,
				unsigned(rig.pool->peak()), (unsigned long long)rig.pool->stolen(), (unsigned long long)rig.pool->failed(), (unsigned long long)is.impacts,
				(unsigned long long)is.unknown_patches, (unsigned long long)reloads, rms_db, double(tap.peak), (unsigned long long)tap.non_finite);
		MESSAGE(line);
		MESSAGE(ba::format_mixer_cuts(rig.mixer->cut_stats()));

		std::map<ba::InstanceHandle, std::vector<std::pair<uint64_t, bool>>> pt1_per;
		for (const ba::MixerCutEvent &e : pt1_cuts) {
			if (e.kind == ba::MixerCutKind::Pend || e.kind == ba::MixerCutKind::Resume) {
				pt1_per[e.handle].emplace_back(e.frame, e.kind == ba::MixerCutKind::Pend);
			}
		}
		uint64_t pt1_parks = 0, pt1_short_parks = 0, pt1_flaps = 0;
		size_t pt1_flapping = 0;
		for (const auto &[h, v] : pt1_per) {
			uint64_t mine = 0;
			for (size_t i = 0; i < v.size(); ++i) {
				pt1_parks += v[i].second ? 1 : 0;
				if (v[i].second && i + 1 < v.size() && !v[i + 1].second && v[i + 1].first - v[i].first < ba::kMixRate / 4) {
					++pt1_short_parks;
				}
				if (v[i].second && i >= 2 && !v[i - 1].second && v[i - 2].second && v[i].first - v[i - 2].first < ba::kMixRate) {
					++mine;
				}
			}
			pt1_flaps += mine;
			pt1_flapping += mine > 0 ? 1 : 0;
		}
		MESSAGE("PT1 snd: " << pt1_cuts.size() << " cuts recorded; " << pt1_parks << " parks of " << pt1_per.size() << " loops, " << pt1_short_parks
							<< " parked for < 0.25 s, " << pt1_flaps << " re-parks within 1 s on " << pt1_flapping << " loops");
		if (env_value("OPENBF3_PT1_CUT_FADE").empty() && env_value("OPENBF3_PT1_CAP_HOLD").empty() && env_value("OPENBF3_PT1_RESUME_MARGIN").empty() &&
				env_value("OPENBF3_PT1_LIMIT").empty()) {
			const ba::MixerCutStats &cs = rig.mixer->cut_stats();
			for (const ba::MixerCutKind k : { ba::MixerCutKind::Drop, ba::MixerCutKind::Pend, ba::MixerCutKind::Kill, ba::MixerCutKind::Finish, ba::MixerCutKind::Resume }) {
				INFO(ba::mixer_cut_kind_name(k));
				CHECK(cs.audible[size_t(k)].load() == 0);
			}
			CHECK(pt1_parks > 0);
			CHECK(pt1_short_parks * 20 <= pt1_parks);
			CHECK(pt1_flaps * 20 <= pt1_parks);
			CHECK(cs.event_drops.load() == 0);
			CHECK(cs.overs_out.load() == 0);
		}
		CHECK(ms.short_blocks.load() == 0);
		CHECK(ms.late_commands.load() == 0);
		CHECK(ms.ring_overflows.load() == 0);
		CHECK(ms.starved_voices.load() == 0);
		CHECK(ms.decode_errors.load() == 0);
		CHECK(ms.feedback_drops.load() == 0);
		CHECK(ms.peak_instances.load() <= ba::kGameMixerInstances);
		CHECK(ws.mixer_refusals == 0);
		CHECK(ws.bursts > 100);

		CHECK(ws.culled + ms.culled_precull.load() + ms.culled_scope.load() + ms.culled_cap.load() > 0);
		CHECK(ws.reloads > 0);
		CHECK(is.impacts > 0);
		CHECK(rig.pool->failed() == 0);
		CHECK(tap.non_finite == 0);
		CHECK(tap.frames >= uint64_t(kSeconds) * ba::kMixRate);
		CHECK(rms_db > -60.0);
		if (gate_late_blocks()) {
			CHECK(ms.late_blocks.load() == 0);
		}

		{
			const ba::MixerProfile prof = ba::read_mixer_profile(ms);
			uint64_t hist_blocks = 0;
			for (const std::atomic<uint64_t> &h : ms.mix_ns_hist) {
				hist_blocks += h.load();
			}
			char a3line[700];
			std::snprintf(a3line, sizeof(a3line),
					"E11 (A3): %s\n  blocks %llu (histogram %llu), mix p99 share < %.2f (gate <= 0.10: %s), live voices max %u (gate <= 128), culled pre %llu / scope "
					"%llu / cap %llu, window now %.1f .. %.1f dB",
					ba::format_mixer_profile(prof).c_str(), (unsigned long long)prof.blocks, (unsigned long long)hist_blocks, prof.share_p99,
					gate_late_blocks() ? "gated" : "branch, indicative only", prof.live_voices_max, (unsigned long long)prof.culled_precull,
					(unsigned long long)prof.culled_scope, (unsigned long long)prof.culled_cap, double(prof.window_bottom_db), double(prof.window_top_db));
			MESSAGE(a3line);
			CHECK(hist_blocks == prof.blocks);
			CHECK(prof.live_voices_max <= ba::kGameMixerVoices);
			CHECK(prof.live_voices_max > 0);
			CHECK(prof.culled_precull > 0);
			if (gate_late_blocks()) {
				CHECK(prof.share_p99 <= 0.10);
			}
		}
		check_game_unchanged(*f);
	}
}

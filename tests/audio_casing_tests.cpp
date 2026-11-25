#include "test_support.h"

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/client_audio/game_audio_frame.h"
#include "bf3/client_audio/kit_audio.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::core;
namespace ba = bf3::audio;
namespace ca = bf3::core::audio;
namespace cp = bf3::core::audio::port;
namespace gp = bf3::core::gameplay;
namespace host = bf3::host;
namespace present = bf3::present;
namespace sw = bf3::sim::weapons;

namespace {

constexpr uint32_t kFramesPerTick = 800;
constexpr sw::SimTime kTick = sw::kTimeUnitsPerTick;

struct CasingFixture {
	bf3::test::Mounted m;
	std::unique_ptr<level::LoadedLevel> metro;
	std::shared_ptr<const host::GunplayLevel> level;
	ba::GameAudioAssets assets;
};

CasingFixture *casing_fixture() {
	static std::unique_ptr<CasingFixture> f;
	static bool tried = false;
	if (tried) {
		return f.get();
	}
	tried = true;
	auto made = std::make_unique<CasingFixture>();
	if (!bf3::test::open_install(made->m)) {
		return nullptr;
	}
	Result<std::unique_ptr<level::LoadedLevel>> lvl =
			level::load_level(made->m.install, "MP_Subway", "TeamDeathMatch0");
	REQUIRE_MESSAGE(lvl.ok(), (lvl.ok() ? std::string() : lvl.error().message));
	made->metro = std::move(*lvl);
	const std::string id = made->metro->info.id.empty() ? made->metro->root_name : made->metro->info.id;
	Result<std::shared_ptr<const host::GunplayLevel>> g =
			host::load_gunplay_level(made->m.install, id, "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
	REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
	made->level = *g;
	Result<ba::GameAudioAssets> a = ba::load_game_audio_assets(made->m.install, *made->metro, *made->level, {}, 0);
	REQUIRE_MESSAGE(a.ok(), (a.ok() ? std::string() : a.error().message));
	made->assets = std::move(*a);
	f = std::move(made);
	return f.get();
}

CasingFixture *need_casing_install() {
	CasingFixture *f = casing_fixture();
	if (f == nullptr) {
		bf3::test::skip_install_case("no BF3 install found");
	}
	return f;
}

void check_casing_unchanged(const CasingFixture &p_f) {
	const SnapshotDiff diff = diff_snapshots(p_f.m.before, p_f.m.reader.snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

bool casing_wave(std::string_view p_name) {
	return p_name.find("shellcasings") != std::string_view::npos ||
			p_name.find("shell_casing") != std::string_view::npos;
}

struct CasingSampler {
	uint32_t node = 0;
	ca::SoundPort amplitude;
	float delay_min = 0.0f;
	float delay_max = 0.0f;
};

std::vector<CasingSampler> casing_samplers(const ca::SoundProgram &p) {
	std::vector<CasingSampler> out;
	for (uint32_t i = 0; i < p.nodes.size(); ++i) {
		const ca::SoundNode &n = p.nodes[i];
		if (n.type != ca::SoundNodeType::Sampler) {
			continue;
		}
		bool casing = false;
		const auto waves_of = [&](const ca::SoundNode &w) {
			for (uint16_t k = 0; k < w.waves_count; ++k) {
				const uint16_t r = p.wave_refs[w.waves_begin + k];
				casing = casing || (r != ca::kNoRegister && casing_wave(p.waves[r].name));
			}
		};
		waves_of(n);
		const ca::SoundPort &ext = p.ports[n.ports_begin + cp::SmpExternalWave];
		for (const ca::SoundNode &w : p.nodes) {
			if (ext.connected() && w.type == ca::SoundNodeType::WaveSwitcher &&
					p.ports[w.ports_begin + cp::WsWave].reg == ext.reg) {
				waves_of(w);
			}
		}
		if (!casing) {
			continue;
		}
		CasingSampler c;
		c.node = i;
		c.amplitude = p.ports[n.ports_begin + cp::SmpAmplitude];
		const ca::SoundPort &delay = p.ports[n.ports_begin + cp::SmpDelay];
		c.delay_min = c.delay_max = delay.constant;
		for (const ca::SoundNode &g : p.nodes) {
			if (delay.connected() && g.type == ca::SoundNodeType::NumberGenerator &&
					p.ports[g.ports_begin + cp::NumY].reg == delay.reg) {
				c.delay_min = p.floats[g.floats_begin + cp::NumMin];
				c.delay_max = p.floats[g.floats_begin + cp::NumMax];
			}
		}
		out.push_back(c);
	}
	return out;
}

struct Recorder final : ba::WeaponAudioObserver {
	std::deque<ba::SamplerTrace> traces;
	std::vector<uint64_t> fires, stops;
	void watch(ba::PatchInstance &r_instance) {
		ba::SamplerTrace &t = traces.emplace_back();
		t.entries.resize(1024);
		r_instance.set_sampler_trace(&t);
	}
	void on_driver_instance(uint8_t, uint64_t, uint64_t p_first, ba::PatchInstance &r_instance) override {
		fires.push_back(p_first);
		watch(r_instance);
	}
	void on_trigger_up(uint8_t, uint64_t, uint64_t p_stop) override { stops.push_back(p_stop); }
	void on_foley(const ba::FoleyCue &, ba::PatchInstance *r_instance) override {
		if (r_instance != nullptr) {
			watch(*r_instance);
		}
	}
	void on_one_shot(ba::PatchInstance &r_instance) override { watch(r_instance); }
};

struct Pull {
	sw::SimTime first = 0;
	uint32_t rounds = 1;
	sw::SimTime interval = 0;
	sw::SimTime bolt_after = -1;
};

struct Casing {
	uint64_t frame = 0;
	uint32_t node = 0;
};

struct Played {
	std::vector<uint64_t> shot_frames;
	std::vector<uint64_t> last_frames;
	std::vector<uint64_t> bolt_frames;
	std::vector<uint64_t> stops;
	std::vector<Casing> casings;
	uint64_t dropped = 0;
};

Played play(const CasingFixture &p_f, uint16_t p_def, uint16_t p_sound, const std::vector<Pull> &p_pulls,
		double p_seconds) {
	const ba::AudioTuning tuning;
	const ba::GameMixerSizes sizes = ba::game_mixer_sizes(tuning);
	ba::MixerCore mixer(ba::game_mixer_config(tuning, sizes, 8192));
	ba::OneShotPool pool(sizes.one_shots);
	ba::SimMixerClockConfig cc;
	cc.bias_frames = 2048;
	ba::SimMixerClock clock(mixer, cc);
	ba::WeaponAudioRouter router(mixer, p_f.assets.table, p_f.assets.bank, tuning, pool);
	Recorder rec;
	router.set_observer(&rec);
	router.set_own_slot(0);
	router.set_own_first_person(true);
	router.set_slot_sound(0, p_def, p_sound);
	struct Timed {
		sw::SimTime time = 0;
		bool shot = true;
		bool down_after = false;
	};
	std::vector<Timed> plan;
	const sw::SimTime base = 1000 * kTick;
	Played out;
	for (const Pull &p : p_pulls) {
		for (uint32_t k = 0; k < p.rounds; ++k) {
			plan.push_back(Timed{ base + p.first + sw::SimTime(k) * p.interval, true, k + 1 < p.rounds });
		}
		if (p.bolt_after >= 0) {
			plan.push_back(
					Timed{ base + p.first + sw::SimTime(p.rounds - 1) * p.interval + p.bolt_after, false, false });
		}
	}
	std::stable_sort(plan.begin(), plan.end(), [](const Timed &a, const Timed &b) { return a.time < b.time; });
	std::vector<float> buffer(size_t(kFramesPerTick) * 2);
	sw::SimTime predicted = base;
	size_t next = 0;
	uint32_t id = 1;
	bool down = false;
	const int frames = int(std::ceil(p_seconds * 60.0));
	for (int fi = 0; fi < frames; ++fi) {
		present::PresentClock now;
		now.predicted_valid = now.render_valid = true;
		now.predicted = predicted;
		now.render = predicted - 6 * kTick;
		clock.set_now(now);
		router.set_listener(ba::ListenerState{});
		std::vector<present::PresentEvent> ev;
		while (next < plan.size() && plan[next].time <= predicted) {
			const Timed &t = plan[next++];
			present::PresentEvent e;
			e.timeline = present::PresentTimeline::Predicted;
			e.slot = 0;
			e.def = p_def;
			e.time = t.time;
			if (t.shot) {
				e.kind = present::PresentKind::OwnShot;
				e.id = id++;
				down = t.down_after;
				const uint64_t f = clock.frame_of(t.time, present::PresentTimeline::Predicted);
				out.shot_frames.push_back(f);
				if (!down) {
					out.last_frames.push_back(f);
				}
			} else {
				e.kind = present::PresentKind::OwnTimed;
				e.id = sw::weapon_events::kBoltStarted;
				out.bolt_frames.push_back(clock.frame_of(t.time, present::PresentTimeline::Predicted));
			}
			ev.push_back(e);
		}
		router.set_own_trigger(down);
		router.on_events(ev, clock);
		router.on_others({}, {}, {}, clock);
		router.update(0);
		mixer.mix(buffer.data(), kFramesPerTick);
		predicted += kTick;
	}
	out.stops = rec.stops;
	for (const ba::SamplerTrace &t : rec.traces) {
		out.dropped += t.dropped;
		for (uint32_t k = 0; k < std::min<uint32_t>(t.count, uint32_t(t.entries.size())); ++k) {
			const ba::SamplerTraceEntry &e = t.entries[k];
			if (e.kind == ba::SamplerTraceKind::Trigger && e.wave != ca::kNoWave &&
					casing_wave(p_f.assets.bank->waves()[e.wave].name)) {
				out.casings.push_back(Casing{ e.frame, e.node });
			}
		}
	}
	std::sort(
			out.casings.begin(), out.casings.end(), [](const Casing &a, const Casing &b) { return a.frame < b.frame; });
	return out;
}

bool has_mode(const gp::FireFunction &p_fire, gp::FireLogic p_logic) {
	return p_fire.logic == p_logic ||
			std::find(p_fire.extra_modes.begin(), p_fire.extra_modes.end(), p_logic) != p_fire.extra_modes.end();
}

sw::SimTime units(double p_seconds) {
	return sw::SimTime(std::llround(p_seconds * double(sw::kTimeUnitsPerSecond)));
}

}

TEST_SUITE("audio-casings") {
	TEST_CASE("casings: every primary and sidearm of Metro TDM's catalog, with its accessory sounds, plays one shell "
			  "casing "
			  "per trigger pull, burst or bolt, inside its sampler's delay range") {
		CasingFixture *f = need_casing_install();
		if (f == nullptr) {
			return;
		}
		const host::GunplayLevel &level = *f->level;
		const ba::WeaponAudioTable &table = *f->assets.table;
		std::map<uint16_t, uint16_t> sounds;
		for (const auto &team : level.loadouts.kits) {
			for (const host::LoadoutKit &k : team) {
				for (const gp::WeaponSlot slot : { gp::WeaponSlot::Primary, gp::WeaponSlot::Sidearm }) {
					for (const host::LoadoutItem &item : k.slots[size_t(slot)]) {
						if (item.def < level.defs.size()) {
							sounds[item.def] = item.def;
						}
					}
				}
			}
		}
		const size_t plain = sounds.size();
		for (const ba::KitAudioVariant &v : f->assets.kits.variants) {
			if (sounds.count(v.def) != 0) {
				sounds[v.source] = v.def;
			}
		}
		const sw::SimTime pull_gap = units(2.0);
		size_t with_casing = 0, silent_by_data = 0, checked = 0;
		std::vector<std::string> wrong;
		for (const auto &[sound, def] : sounds) {
			REQUIRE(sound < table.defs.size());
			const gp::WeaponDef &w = level.defs[def];
			const ba::WeaponAudioDef &a = table.defs[sound];
			REQUIRE_MESSAGE(a.program, w.variant);
			const std::vector<CasingSampler> samplers = casing_samplers(*a.program);
			if (samplers.empty()) {
				continue;
			}
			++with_casing;
			REQUIRE(samplers.size() == 1);
			const CasingSampler &c = samplers[0];
			if (c.amplitude.connected() && c.amplitude.constant == 0.0f) {
				++silent_by_data;
				continue;
			}
			const bool bolt = w.fire.logic == gp::FireLogic::SingleWithBoltAction;
			std::vector<Pull> pulls;
			sw::SimTime at = units(0.5);
			for (int k = 0; k < 3; ++k) {
				pulls.push_back(Pull{ at, 1, 0, bolt ? units(w.fire.bolt_action_delay) : -1 });
				at += pull_gap;
			}
			if (has_mode(w.fire, gp::FireLogic::Burst) && w.fire.bullets_per_burst > 1) {
				for (int k = 0; k < 2; ++k) {
					pulls.push_back(
							Pull{ at, uint32_t(w.fire.bullets_per_burst), units(gp::burst_interval(w.fire)), -1 });
					at += pull_gap;
				}
			}
			if (has_mode(w.fire, gp::FireLogic::Automatic)) {
				pulls.push_back(Pull{ at, 10, units(gp::shot_interval(w.fire)), -1 });
				at += units(10.0 * gp::shot_interval(w.fire));
				at += pull_gap;
			}
			const Played p = play(*f, def, sound, pulls, double(at) / double(sw::kTimeUnitsPerSecond) + 1.0);
			++checked;
			CHECK(p.dropped == 0);
			std::string line = w.variant + " [" + a.patch + "] pulls " + std::to_string(pulls.size()) + " casings " +
					std::to_string(p.casings.size()) + " delay " + std::to_string(c.delay_min) + ".." +
					std::to_string(c.delay_max) + ":";
			const uint64_t lo_delay = uint64_t(std::floor(double(c.delay_min) * ba::kMixRate));
			const uint64_t hi_delay = uint64_t(std::ceil(double(c.delay_max) * ba::kMixRate)) + 2 * ba::kBlockFrames;
			bool ok = p.casings.size() == pulls.size();
			size_t first_shot = 0;
			for (size_t k = 0; k < pulls.size() && k < p.casings.size(); ++k) {
				const uint64_t from = bolt ? p.bolt_frames[k] : p.shot_frames[first_shot];
				const uint64_t to = bolt
						? p.bolt_frames[k]
						: (k < p.stops.size() ? std::max(p.stops[k], p.last_frames[k]) : p.last_frames[k]);
				first_shot += pulls[k].rounds;
				const uint64_t heard = p.casings[k].frame;
				line += " " + std::to_string(int64_t(heard) - int64_t(to));
				ok = ok && p.casings[k].node == c.node && heard >= from + lo_delay && heard <= to + hi_delay;
			}
			if (!ok) {
				wrong.push_back(line);
			}
		}
		std::string all;
		for (const std::string &l : wrong) {
			all += "\n  " + l;
		}
		CHECK_MESSAGE(wrong.empty(), wrong.size(), " weapons play their casing wrong:", all);
		MESSAGE("weapon sounds ", sounds.size(), " (", plain, " catalog weapons), with a casing sampler ", with_casing,
				", silent by data ", silent_by_data, ", checked ", checked);
		CHECK(plain == 89);
		CHECK(silent_by_data == 1);
		CHECK(checked + silent_by_data == with_casing);
		check_casing_unchanged(*f);
	}

	TEST_CASE("casing gain: the AK-74M's, M16A4's and LSAT's casing sampler plays at its AreaType table times "
			  "its Amplitude port's UnconnectedValue (R5-AU1), so the LSAT's is silent") {
		CasingFixture *f = need_casing_install();
		if (f == nullptr) {
			return;
		}
		struct Expected {
			const char *patch;
			std::array<float, 7> table;
			float base;
		};
		const std::array<Expected, 3> expected{ {
				{ "sound/weapons/handheld/ak74/weapon_assaultrifle_ak74_02",
						{ 1.0f, 2.0f, 3.0f, 2.0f, 1.2f, 0.8f, 3.0f }, 0.4f },
				{ "sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02",
						{ 1.0f, 3.0f, 2.5f, 2.0f, 1.2f, 0.8f, 3.0f }, 0.4f },
				{ "sound/weapons/handheld/xp2_lsat/weapon_lmg_lsat", { 1.0f, 1.2f, 3.0f, 1.5f, 1.2f, 1.0f, 3.0f },
						0.0f },
		} };
		for (const Expected &e : expected) {
			const ba::WeaponAudioDef *def = nullptr;
			for (const ba::WeaponAudioDef &d : f->assets.table->defs) {
				if (d.patch == e.patch && d.program) {
					def = &d;
				}
			}
			REQUIRE_MESSAGE(def != nullptr, e.patch);
			const std::vector<CasingSampler> samplers = casing_samplers(*def->program);
			REQUIRE(samplers.size() == 1);
			CHECK(samplers[0].amplitude.connected());
			CHECK(samplers[0].amplitude.constant == doctest::Approx(e.base));
			for (size_t area = 0; area < e.table.size(); ++area) {
				Result<std::unique_ptr<ba::PatchInstance>> inst =
						ba::PatchInstance::create(def->program, f->assets.bank, ba::AudioTuning{}, 1, ba::kMixRate);
				REQUIRE(inst.ok());
				(*inst)->set_parameter(ba::kHashIsFirstPerson, 1.0f);
				(*inst)->set_mixer_value(ca::kMixerAreaType, float(area));
				(*inst)->evaluate(0.01);
				const std::span<const ba::SamplerSlotState> slots = (*inst)->sampler_slots(samplers[0].node);
				REQUIRE(!slots.empty());
				CHECK_MESSAGE(slots[0].gain == doctest::Approx(e.table[area] * e.base), std::string(e.patch),
						" AreaType ", area);
			}
		}
		check_casing_unchanged(*f);
	}
}

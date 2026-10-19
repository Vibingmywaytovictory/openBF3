// PT1 rld: reload and weapon foley sounds (docs/design/pt1-rld.md). Rows (tests/CMakeLists.txt "# PT1 rld"):
//   audio.foley.install  Metro TDM's kit patches: the foley inputs read from the data (Reload, IsShortReload, the
//                        reload's release event, Action / ActionType, Bolt) and the waves at each index; the router
//                        driven with the own soldier's and another soldier's weapon events (deploy, tactical, empty
//                        and cancelled reloads, dry fire, fire-mode switch, zoom), checked through the sampler trace:
//                        which event starts which wave, in order, at which frame; the offline mix: the reload alone,
//                        after an emptied magazine and in third person at 5 / 15 / 25 m, loudest 20 ms in dBFS.
// The WAVs of the mix go to <build>/shots/audio.foley (derived data, gitignored).
#include "test_support.h"

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/game_audio_frame.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/voice_cull.h"
#include "bf3/audio/wav_io.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/audio/weapon_foley.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/path_guard.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/presentation_events.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace bf3::core;
namespace ba = bf3::audio;
namespace ca = bf3::core::audio;
namespace host = bf3::host;
namespace sw = bf3::sim::weapons;

namespace {

constexpr uint32_t kFrames = 800; // one 60 Hz display frame at 48 kHz
constexpr double kTicks0 = 600.25; // the rig's first display frame, in ticks
constexpr const char *kM16Patch = "sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02";

struct FoleyFixture {
	InstallReader reader;
	InstallSnapshot before;
	std::shared_ptr<Install> install;
	std::unique_ptr<level::LoadedLevel> metro;
	std::shared_ptr<const host::GunplayLevel> gunplay;
	ba::GameAudioAssets assets;
	ba::MixerA3Assets a3;
	ba::AudioTuning tuning;
};

FoleyFixture *foley_fixture() {
	static std::unique_ptr<FoleyFixture> f;
	static bool tried = false;
	if (tried) {
		return f.get();
	}
	tried = true;
	auto made = std::make_unique<FoleyFixture>();
	if (!made->reader.locate({})) {
		return nullptr;
	}
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
	Result<ba::GameAudioAssets> a = ba::load_game_audio_assets(made->install, *made->metro, *made->gunplay, {}, 0);
	REQUIRE_MESSAGE(a.ok(), (a.ok() ? std::string() : a.error().message));
	made->assets = std::move(*a);
	// The game's A3 inputs (Bf3GameAudio::load): the HdrSetting and the scopes of the kit programs.
	std::vector<std::shared_ptr<const ca::SoundProgram>> programs;
	for (const ba::WeaponAudioDef &d : made->assets.table->defs) {
		programs.push_back(d.program);
		programs.push_back(d.explosion);
	}
	made->a3 = ba::load_mixer_a3(made->metro->db(), programs);
	REQUIRE_MESSAGE(made->a3.hdr.has_value(), made->a3.notes);
	std::ifstream cfg(OPENBF3_AUDIO_TUNING_CFG, std::ios::binary);
	REQUIRE(cfg.good());
	std::stringstream text;
	text << cfg.rdbuf();
	Result<ba::AudioTuning> tuning = ba::parse_audio_tuning(text.str());
	REQUIRE_MESSAGE(tuning.ok(), (tuning.ok() ? std::string() : tuning.error().message));
	made->tuning = *tuning;
	f = std::move(made);
	return f.get();
}

FoleyFixture *need_install() {
	FoleyFixture *f = foley_fixture();
	if (f == nullptr) {
		bf3::test::skip_install_case("no BF3 install found");
	}
	return f;
}

void check_unchanged(const FoleyFixture &p_f) {
	const SnapshotDiff diff = diff_snapshots(p_f.before, p_f.reader.snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

uint16_t def_of_patch(const ba::WeaponAudioTable &p_table, std::string_view p_patch) {
	for (size_t i = 0; i < p_table.defs.size(); ++i) {
		if (p_table.defs[i].patch == p_patch && p_table.defs[i].form != ba::WeaponAudioForm::None) {
			return uint16_t(i);
		}
	}
	return sw::kNoWeapon;
}

sw::SimTime at(double p_seconds) {
	return sw::SimTime(std::llround((kTicks0 + p_seconds * 60.0) * double(sw::kTimeUnitsPerTick)));
}

// The game's mixer (Bf3GameAudio::build) without a stream: the router driven one display frame at a time, 800 frames
// mixed per frame, the whole mix kept by a tap.
struct Rig {
	struct Whole final : ba::MixTap {
		std::vector<float> mix;
		void on_block(const float *p, uint32_t n, uint64_t) override { mix.insert(mix.end(), p, p + size_t(n) * 2); }
	} tap;
	std::unique_ptr<ba::MixerCore> mixer;
	std::unique_ptr<ba::OneShotPool> pool;
	std::unique_ptr<ba::SimMixerClock> clock;
	std::unique_ptr<ba::WeaponAudioRouter> router;
	std::vector<float> buffer = std::vector<float>(size_t(kFrames) * 2);
	ba::ListenerState listener;
	std::vector<float> top_db; // the window top after each display frame
	uint64_t frame_index = 0;
	const host::PresentationFrame *own_frame = nullptr; // the presented own weapons (null: unknown)

	explicit Rig(const FoleyFixture &p_f) {
		ba::MixerConfig mc;
		mc.max_instances = ba::kGameMixerInstances;
		mc.command_capacity = 8192;
		mc.schedule_lead_blocks = uint32_t(std::max(1.0f, p_f.tuning.schedule_lead_blocks));
		mc.max_voices = ba::kGameMixerVoices;
		ba::apply_mixer_a3(p_f.a3, mc);
		mixer = std::make_unique<ba::MixerCore>(mc);
		mixer->set_tap(&tap);
		pool = std::make_unique<ba::OneShotPool>(ba::kMaxOneShotVoices);
		ba::SimMixerClockConfig cc;
		cc.bias_frames = int64_t(mc.schedule_lead_blocks) * int64_t(ba::kBlockFrames) + 2 * int64_t(ba::kBlockFrames) + 800;
		clock = std::make_unique<ba::SimMixerClock>(*mixer, cc);
		router = std::make_unique<ba::WeaponAudioRouter>(*mixer, p_f.assets.table, p_f.assets.bank, p_f.tuning, *pool);
		router->set_own_slot(0);
		router->set_own_first_person(true);
		listener.position = { 0.0f, 1.6f, 0.0f };
		listener.forward = { 0.0f, 0.0f, 1.0f };
	}
	~Rig() { router.reset(); }
	double now_s() const { return double(frame_index) / 60.0; }
	// One display frame at the rig's time: the events, the others, 800 frames mixed.
	void frame(std::span<const host::PresentEvent> p_events, std::span<const host::PresentedSoldier> p_others = {},
			std::span<const host::PresentedOtherWeapon> p_weapons = {}, bool p_trigger = false) {
		host::PresentClock c;
		c.predicted_valid = c.render_valid = true;
		c.predicted = c.render = at(now_s());
		c.now_ns = uint64_t(std::llround((kTicks0 + double(frame_index)) * 1e9 / 60.0));
		clock->set_now(c);
		router->set_own_trigger(p_trigger);
		router->set_listener(listener);
		router->set_own_weapons(own_frame); // as GameAudioDriver::step
		router->on_events(p_events, *clock);
		router->on_others(p_others, p_weapons, {}, *clock);
		router->update(c.now_ns);
		mixer->mix(buffer.data(), kFrames);
		top_db.push_back(mixer->stats().window_top_db.load());
		++frame_index;
	}
	// The mix frame an event at p_seconds is heard at (what frame_of gave it; the mix starts at frame 0).
	uint64_t heard_frame(double p_seconds) const { return uint64_t(std::max(0.0, p_seconds * double(ba::kMixRate))); }
	// The loudest 20 ms (RMS, both channels) of [p_from_s, p_to_s] of the mix, in dBFS.
	double loudest_db(double p_from_s, double p_to_s) const {
		const uint64_t len = uint64_t(0.02 * ba::kMixRate);
		const uint64_t end = std::min<uint64_t>(uint64_t(p_to_s * ba::kMixRate), tap.mix.size() / 2);
		double best = -300.0;
		for (uint64_t a = uint64_t(std::max(0.0, p_from_s) * ba::kMixRate); a + len <= end; a += len / 2) {
			best = std::max(best, double(ba::rms_db(tap.mix, 2, a, len)));
		}
		return best;
	}
	float max_top_db(double p_from_s, double p_to_s) const {
		float m = -300.0f;
		for (size_t i = size_t(std::max(0.0, p_from_s) * 60.0); i < top_db.size() && double(i) / 60.0 < p_to_s; ++i) {
			m = std::max(m, top_db[i]);
		}
		return m;
	}
};

host::PresentEvent own_timed(uint16_t p_def, uint32_t p_event, double p_seconds, uint8_t p_weapon_slot = 0) {
	host::PresentEvent e;
	e.kind = host::PresentKind::OwnTimed;
	e.timeline = host::PresentTimeline::Predicted;
	e.def = p_def;
	e.aux = p_weapon_slot;
	e.id = p_event;
	e.time = at(p_seconds);
	return e;
}

host::PresentEvent own_shot(uint16_t p_def, uint32_t p_id, double p_seconds) {
	host::PresentEvent e;
	e.kind = host::PresentKind::OwnShot;
	e.timeline = host::PresentTimeline::Predicted;
	e.def = p_def;
	e.id = p_id;
	e.time = at(p_seconds);
	return e;
}

// Every foley cue the router posts, each one-shot's sampler trace attached before its Start (the trace outlives the
// instance: the deque never moves its elements).
struct FoleyRecorder final : ba::WeaponAudioObserver {
	struct Cue {
		ba::FoleyCue cue;
		ba::SamplerTrace *trace = nullptr;
	};
	std::vector<Cue> cues;
	std::deque<ba::SamplerTrace> traces;
	void on_driver_instance(uint8_t, uint64_t, uint64_t, ba::PatchInstance &) override {}
	void on_trigger_up(uint8_t, uint64_t, uint64_t) override {}
	void on_foley(const ba::FoleyCue &p_cue, ba::PatchInstance *r_instance) override {
		Cue c;
		c.cue = p_cue;
		if (r_instance != nullptr) {
			traces.emplace_back();
			traces.back().entries.resize(256);
			r_instance->set_sampler_trace(&traces.back());
			c.trace = &traces.back();
		}
		cues.push_back(c);
	}
};

// What one cue's instance played: the first Trigger's wave and frame, and the frame its slot went idle.
struct Played {
	bool triggered = false;
	std::string wave;
	uint64_t start = 0;
	uint64_t end = 0; // the frame the first Trigger's slot went idle
	uint32_t node = 0;
	uint8_t slot = 0;
	uint32_t others = 0;
	std::string other_waves;
};

Played played(const FoleyFixture &p_f, const FoleyRecorder::Cue &p_c) {
	Played p;
	if (p_c.trace == nullptr) {
		return p;
	}
	for (uint32_t i = 0; i < p_c.trace->count && i < p_c.trace->entries.size(); ++i) {
		const ba::SamplerTraceEntry &e = p_c.trace->entries[i];
		if (e.kind == ba::SamplerTraceKind::Trigger && !p.triggered) {
			p.triggered = true;
			p.start = e.frame;
			p.wave = e.wave < p_f.assets.bank->waves().size() ? p_f.assets.bank->waves()[e.wave].name : std::string("?");
			const size_t slash = p.wave.rfind('/');
			p.wave = slash == std::string::npos ? p.wave : p.wave.substr(slash + 1);
			p.node = e.node;
			p.slot = e.slot;
		} else if (e.kind == ba::SamplerTraceKind::End && p.triggered && e.node == p.node && e.slot == p.slot && p.end == 0) {
			p.end = e.frame;
		} else if (e.kind == ba::SamplerTraceKind::Trigger && p.triggered && e.node != p.node) {
			++p.others; // another sampler of the instance (the pool's Stop posts the patch's stop event)
			const std::string n = e.wave < p_f.assets.bank->waves().size() ? p_f.assets.bank->waves()[e.wave].name : std::string("?");
			p.other_waves += (p.other_waves.empty() ? "" : ", ") + n.substr(n.rfind('/') + 1) + " at +" +
					std::to_string(int((double(e.frame) - double(p.start)) * 1000.0 / ba::kMixRate)) + " ms";
		}
	}
	return p;
}

// "2 var x 3 seg, loop -1..-1" of the bank wave whose name ends with p_name.
std::string wave_shape(const FoleyFixture &p_f, const std::string &p_name) {
	for (const ca::SoundWaveInfo &w : p_f.assets.bank->waves()) {
		if (w.name.size() >= p_name.size() && w.name.compare(w.name.size() - p_name.size(), p_name.size(), p_name) == 0) {
			std::string segs;
			for (const ca::WaveVariation &v : w.variations) {
				segs += (segs.empty() ? "" : "/") + std::to_string(v.segments.size());
			}
			const int lf = w.variations.empty() ? -1 : w.variations[0].first_loop_segment;
			const int ll = w.variations.empty() ? -1 : w.variations[0].last_loop_segment;
			return std::to_string(w.variations.size()) + " var, segments " + segs + ", loop " + std::to_string(lf) + ".." + std::to_string(ll);
		}
	}
	return "not in the bank";
}

const char *cue_kind(ba::FoleyCueKind p_k) {
	switch (p_k) {
		case ba::FoleyCueKind::Reload:
			return "Reload";
		case ba::FoleyCueKind::ReloadRelease:
			return "ReloadRelease";
		case ba::FoleyCueKind::Action:
			return "Action";
		case ba::FoleyCueKind::Bolt:
			return "Bolt";
	}
	return "?";
}

std::filesystem::path shots_dir() {
	return std::filesystem::path(OPENBF3_PT1_SHOTS) / "audio.foley";
}

void write_wav(const FoleyFixture &p_f, const char *p_name, const std::vector<float> &p_mix) {
	PathGuard g;
	REQUIRE(g.add_protected_root(p_f.reader.root()));
	const std::filesystem::path dir = shots_dir();
	REQUIRE(g.allows_write(dir));
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const Status s = ba::write_wav(g, dir / p_name, p_mix, 2, ba::kMixRate);
	CHECK_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
}

} // namespace

TEST_SUITE("audio-foley") {
	TEST_CASE("the kit patches' foley inputs, read from the data") {
		FoleyFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const ba::WeaponAudioTable &table = *f->assets.table;
		uint32_t firearms = 0, with_reload = 0, with_short = 0, with_release = 0, with_action = 0, with_bolt = 0;
		for (size_t i = 0; i < table.defs.size(); ++i) {
			const ba::WeaponAudioDef &d = table.defs[i];
			if (!d.program) {
				continue;
			}
			const ba::WeaponFoley fo = ba::read_weapon_foley(*d.program);
			std::string waves;
			for (size_t k = 0; k < fo.reload_waves.size(); ++k) {
				waves += (k ? ", " : "") + std::to_string(k) + "=" + fo.reload_waves[k];
			}
			std::string actions;
			for (size_t k = 0; k < fo.action_waves.size(); ++k) {
				actions += (k ? ", " : "") + std::to_string(k) + "=" + fo.action_waves[k];
			}
			const std::string line = "pt1 rld data: def " + std::to_string(i) + " " + d.patch + ": " + ba::describe_weapon_foley(*d.program, fo) +
					"; IsShortReload waves [" + waves + "]; ActionType waves [" + actions + "]";
			MESSAGE(line);
			std::printf("%s\n", line.c_str());
			if (d.form == ba::WeaponAudioForm::Driver) {
				++firearms;
			}
			with_reload += fo.reload.present ? 1 : 0;
			with_short += fo.short_reload_param ? 1 : 0;
			with_release += fo.reload.release_event != 0 ? 1 : 0;
			with_action += fo.action.present ? 1 : 0;
			with_bolt += fo.bolt.present ? 1 : 0;
			// Every kit firearm: Reload with a long / short pair on IsShortReload, released by its own event, and the
			// handling Action on ActionType in the FoleyAction order.
			if (d.form == ba::WeaponAudioForm::Driver) {
				CHECK_MESSAGE(fo.reload.present, d.patch);
				CHECK_MESSAGE(fo.short_reload_param, d.patch);
				REQUIRE_MESSAGE(fo.reload_waves.size() == 2, d.patch);
				CHECK_MESSAGE(fo.reload_waves[0].find("long") != std::string::npos, d.patch);
				CHECK_MESSAGE(fo.reload_waves[1].find("short") != std::string::npos, d.patch);
				CHECK_MESSAGE(fo.reload.release_event == 3520090073u, d.patch);
				CHECK_MESSAGE(std::isfinite(fo.reload.reach_m), d.patch);
				CHECK_MESSAGE(!fo.reload.outputs.empty(), d.patch);
				CHECK_MESSAGE(fo.action.present, d.patch);
				CHECK_MESSAGE(fo.action_type_param, d.patch);
				REQUIRE_MESSAGE(fo.action_waves.size() >= size_t(ba::kFoleyActionCount), d.patch);
				CHECK_MESSAGE(fo.action_waves[uint8_t(ba::FoleyAction::Deploy)].find("deploy") != std::string::npos, d.patch);
				CHECK_MESSAGE(fo.action_waves[uint8_t(ba::FoleyAction::Zoom)].find("zoom") != std::string::npos, d.patch);
				CHECK_MESSAGE(fo.action_waves[uint8_t(ba::FoleyAction::DryFire)].find("trigger_click") != std::string::npos, d.patch);
				CHECK_MESSAGE(fo.action_waves[uint8_t(ba::FoleyAction::FireMode)].find("firemodeswitch") != std::string::npos, d.patch);
			}
		}
		char line[200];
		std::snprintf(line, sizeof(line), "pt1 rld data: %u firearms; Reload %u, IsShortReload %u, reload release %u, Action %u, Bolt %u", firearms,
				with_reload, with_short, with_release, with_action, with_bolt);
		MESSAGE(line);
		std::printf("%s\n", line);
		CHECK(firearms >= 10);
		check_unchanged(*f);
	}
	TEST_CASE("the own M16A3 reload in the offline mix: alone, after an emptied magazine (1p)") {
		FoleyFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const uint16_t m16 = def_of_patch(*f->assets.table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);
		const ba::WeaponAudioDef &d = f->assets.table->defs[m16];
		// 1) A tactical reload alone at 1 s, 4 s mixed.
		double alone_db = 0.0, alone_floor = 0.0, alone_late = 0.0, walk_late = 0.0;
		float alone_top = 0.0f;
		uint64_t alone_reloads = 0;
		{
			Rig rig(*f);
			for (int fi = 0; fi < 4 * 60; ++fi) {
				std::vector<host::PresentEvent> ev;
				if (fi == 60) {
					ev.push_back(own_timed(m16, sw::weapon_events::kReloadStarted, 1.0));
				}
				rig.frame(ev);
			}
			alone_floor = rig.loudest_db(0.0, 0.9);
			alone_db = rig.loudest_db(1.0, 3.9);
			alone_late = rig.loudest_db(2.5, 3.9);
			alone_top = rig.max_top_db(1.0, 3.9);
			alone_reloads = rig.router->stats().reloads;
			write_wav(*f, "reload_alone.wav", rig.tap.mix);
		}
		// 1b) The same reload while the soldier walks 3 m/s forward (the own 1p sound is the soldier's: it moves with him).
		{
			Rig rig(*f);
			for (int fi = 0; fi < 4 * 60; ++fi) {
				std::vector<host::PresentEvent> ev;
				if (fi == 60) {
					ev.push_back(own_timed(m16, sw::weapon_events::kReloadStarted, 1.0));
				}
				if (fi > 60) {
					rig.listener.position.z += 3.0f / 60.0f;
				}
				rig.frame(ev);
			}
			walk_late = rig.loudest_db(2.5, 3.9);
		}
		// 2) 30 rounds of automatic fire from 1 s, then the empty reload 0.1 s after the last round (5.5 s mixed).
		double fire_db = 0.0, empty_db = 0.0;
		float empty_top = 0.0f;
		{
			Rig rig(*f);
			const double iv = d.shot_interval_s;
			const double last = 1.0 + 29.0 * iv;
			const double reload_s = last + 0.1;
			uint32_t next = 0;
			bool reload_sent = false;
			for (int fi = 0; fi < int(5.5 * 60); ++fi) {
				std::vector<host::PresentEvent> ev;
				const double t = double(fi) / 60.0;
				while (next < 30 && 1.0 + double(next) * iv <= t) {
					ev.push_back(own_shot(m16, next + 1, 1.0 + double(next) * iv));
					++next;
				}
				if (!reload_sent && reload_s <= t) {
					ev.push_back(own_timed(m16, sw::weapon_events::kReloadStarted, reload_s));
					reload_sent = true;
				}
				rig.frame(ev, {}, {}, next > 0 && next < 30);
			}
			fire_db = rig.loudest_db(1.0, last);
			// The reload after the burst's tail (0.6 s on) to its end.
			empty_db = rig.loudest_db(reload_s + 0.6, reload_s + 2.9);
			empty_top = rig.max_top_db(reload_s + 0.6, reload_s + 2.9);
			write_wav(*f, "reload_after_burst.wav", rig.tap.mix);
		}
		// 3) The voice cull's loudness of the reload instance at the listener, against each EnableHdr output's own.
		std::string outs;
		float cull_db = 0.0f, cull_hint_db = 0.0f;
		{
			Result<std::unique_ptr<ba::PatchInstance>> pi = ba::PatchInstance::create(d.program, f->assets.bank, f->tuning, 1, ba::kMixRate);
			REQUIRE(pi.ok());
			ba::EmitterState em;
			em.position = { 0.0f, 1.6f, 0.0f };
			ba::ListenerState li;
			li.position = em.position;
			li.forward = { 0.0f, 0.0f, 1.0f };
			(*pi)->set_geometry(em, li);
			cull_db = (*pi)->cull_info().loudness_db;
			(*pi)->set_loudness_outputs(d.foley.reload.outputs); // what the router's reload one-shot names (PT1 rld [live outputs])
			cull_hint_db = (*pi)->cull_info().loudness_db;
			const ca::SoundProgram &p = *d.program;
			const ba::WeaponFoley fo = ba::read_weapon_foley(p);
			for (size_t o = 0; o < p.outputs.size(); ++o) {
				const ca::SoundOutput &out = p.outputs[o];
				if (!out.enable_hdr) {
					continue;
				}
				const float dist = std::max(0.0f, out.min_distance) * f->tuning.min_distance_clamp;
				const float at = p.loudness_db + out.attenuation.evaluate(dist * f->tuning.curve_x_metres) + out.gain_db;
				const bool handling = std::find(fo.reload.outputs.begin(), fo.reload.outputs.end(), uint16_t(o)) != fo.reload.outputs.end();
				char b[96];
				std::snprintf(b, sizeof(b), "%s#%zu %.1f dB%s", outs.empty() ? "" : ", ", o, double(at), handling ? " (the reload's)" : "");
				outs += b;
			}
		}
		char line[1200];
		std::snprintf(line, sizeof(line),
				"pt1 rld: own M16A3 1p: cull loudness of a reload instance at the listener before its first block %.1f dB without a hint, %.1f dB with "
				"the reload's outputs; EnableHdr outputs at 0 m: %s",
				double(cull_db), double(cull_hint_db), outs.c_str());
		MESSAGE(line);
		std::printf("%s\n", line);
		std::snprintf(line, sizeof(line),
				"pt1 rld: own M16A3 1p: reload alone %.1f dBFS (floor %.1f, window top %.1f dB, %llu reload one-shots); 30-round burst %.1f dBFS; "
				"the reload 0.6-2.9 s after it %.1f dBFS (window top %.1f dB); its 1.5-2.9 s standing %.1f dBFS, walking 3 m/s %.1f dBFS",
				alone_db, alone_floor, double(alone_top), (unsigned long long)alone_reloads, fire_db, empty_db, double(empty_top), alone_late, walk_late);
		MESSAGE(line);
		std::printf("%s\n", line);
		// Audible: the reload alone within the HDR window its own output sets (not 21 dB under the gunshot outputs), and
		// the own 1p reload moves with the soldier.
		CHECK(alone_reloads == 1);
		CHECK(alone_db > -25.0);
		CHECK(alone_top < 81.0f);
		CHECK(std::fabs(cull_hint_db - 79.5f) < 1.0f);
		CHECK(std::fabs(walk_late - alone_late) < 1.0);
		check_unchanged(*f);
	}

	TEST_CASE("the own soldier's foley triggers: which event starts which wave, in order, at which frame (1p)") {
		FoleyFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const uint16_t m16 = def_of_patch(*f->assets.table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);
		const ba::WeaponAudioDef &d = f->assets.table->defs[m16];
		REQUIRE(d.foley.reload.present);
		REQUIRE(d.foley.reload_waves.size() == 2);
		REQUIRE(d.foley.action_waves.size() >= size_t(ba::kFoleyActionCount));
		Rig rig(*f);
		FoleyRecorder rec;
		rig.router->set_observer(&rec);
		auto own = std::make_unique<host::PresentationFrame>();
		own->own_weapons_valid = true;
		own->own_weapons_curr.active = 0;
		own->own_weapons_curr.slots[0].def = m16;
		own->own_weapons_curr.slots[0].magazine = 30;
		own->own_weapons_curr.timers[0].slot = 0;
		rig.own_frame = own.get();
		// The script: (seconds, the weapon event or a presented-state change, the magazine the reload starts with).
		struct Step {
			double t;
			uint32_t event; // 0: a presented-state step (below)
			int32_t magazine;
			int state; // 1 dry fire tick, 2 zoom down, 3 zoom up
		};
		const Step script[] = {
			{ 0.5, sw::weapon_events::kDeployStarted, 30, 0 },
			{ 1.0, sw::weapon_events::kReloadStarted, 12, 0 }, // tactical
			{ 3.5, sw::weapon_events::kReloadStarted, 0, 0 }, // empty
			{ 7.0, sw::weapon_events::kReloadStarted, 5, 0 },
			{ 7.6, sw::weapon_events::kReloadCancelled, 5, 0 }, // a sprint cuts it
			{ 8.5, 0, 0, 1 }, // a trigger pull on an empty magazine, nothing in reserve
			{ 9.0, sw::weapon_events::kFireModeChanged, 30, 0 },
			{ 9.5, 0, 30, 2 },
			{ 10.0, 0, 30, 3 },
			{ 10.5, sw::weapon_events::kReloadStarted, 3, 0 },
			{ 11.0, sw::weapon_events::kDeployStarted, 3, 0 }, // a switch cuts it
		};
		size_t next = 0;
		for (int fi = 0; fi < int(13.5 * 60); ++fi) {
			std::vector<host::PresentEvent> ev;
			own->own_weapons_curr.events = 0;
			while (next < std::size(script) && std::llround(script[next].t * 60.0) == fi) {
				const Step &s = script[next];
				own->own_weapons_curr.slots[0].magazine = s.magazine;
				if (s.event != 0) {
					ev.push_back(own_timed(m16, s.event, s.t));
				} else if (s.state == 1) {
					own->own_weapons_curr.events = sw::weapon_events::kDryFire;
				} else if (s.state == 2) {
					own->own_weapons_curr.timers[0].flags |= sw::slot_flags::kZoomWasDown;
				} else if (s.state == 3) {
					own->own_weapons_curr.timers[0].flags &= uint8_t(~sw::slot_flags::kZoomWasDown);
				}
				++next;
			}
			rig.frame(ev);
		}
		// The expected cues, in order: the kind, the action or the reload's IsShortReload, the cause's time, the wave.
		struct Want {
			ba::FoleyCueKind kind;
			int what; // Action: FoleyAction; Reload: IsShortReload
			double t;
		};
		const Want want[] = {
			{ ba::FoleyCueKind::Action, int(ba::FoleyAction::Deploy), 0.5 },
			{ ba::FoleyCueKind::Reload, 1, 1.0 },
			{ ba::FoleyCueKind::Reload, 0, 3.5 },
			{ ba::FoleyCueKind::Reload, 1, 7.0 },
			{ ba::FoleyCueKind::ReloadRelease, 0, 7.6 },
			{ ba::FoleyCueKind::Action, int(ba::FoleyAction::DryFire), 8.5 },
			{ ba::FoleyCueKind::Action, int(ba::FoleyAction::FireMode), 9.0 },
			{ ba::FoleyCueKind::Action, int(ba::FoleyAction::Zoom), 9.5 },
			{ ba::FoleyCueKind::Reload, 1, 10.5 },
			{ ba::FoleyCueKind::ReloadRelease, 0, 11.0 },
			{ ba::FoleyCueKind::Action, int(ba::FoleyAction::Deploy), 11.0 },
		};
		CHECK(rec.cues.size() == std::size(want));
		std::string table;
		double worst_lag_ms = 0.0;
		uint64_t last_frame = 0;
		for (size_t i = 0; i < std::min(rec.cues.size(), std::size(want)); ++i) {
			const FoleyRecorder::Cue &c = rec.cues[i];
			const Want &w = want[i];
			CAPTURE(i);
			CHECK(c.cue.kind == w.kind);
			CHECK(c.cue.own);
			CHECK(c.cue.first_person);
			CHECK(c.cue.time == at(w.t));
			CHECK(c.cue.frame >= last_frame); // in order on the mix timeline
			last_frame = c.cue.frame;
			const Played p = played(*f, c);
			std::string expect_wave;
			if (w.kind == ba::FoleyCueKind::Action) {
				CHECK(int(c.cue.action) == w.what);
				expect_wave = d.foley.action_waves[size_t(w.what)];
			} else if (w.kind == ba::FoleyCueKind::Reload) {
				CHECK(int(c.cue.short_reload) == w.what);
				expect_wave = d.foley.reload_waves[size_t(w.what)];
			}
			if (!expect_wave.empty()) {
				CHECK(p.triggered);
				CHECK(p.wave == expect_wave);
				// The wave starts with the event: the frame its time maps to (the viewmodel clip's start), plus the
				// data's sampler Offset, within one mix block.
				const double lag_ms = (double(p.start) - double(c.cue.frame)) * 1000.0 / double(ba::kMixRate);
				CHECK(p.start >= c.cue.frame);
				CHECK(lag_ms <= 1000.0 * double(ba::kBlockFrames) / double(ba::kMixRate) + 16.0);
				worst_lag_ms = std::max(worst_lag_ms, lag_ms);
			}
			char row[240];
			if (c.cue.kind == ba::FoleyCueKind::ReloadRelease) {
				std::snprintf(row, sizeof(row), "\n  %5.2f s %-13s           -> #%u to the running reload, at the event's frame %llu", w.t, cue_kind(c.cue.kind),
						d.foley.reload.release_event, (unsigned long long)c.cue.frame);
			} else {
				std::snprintf(row, sizeof(row), "\n  %5.2f s %-13s %-9s -> %s, starts %+.1f ms after the event's frame, %.2f s long", w.t, cue_kind(c.cue.kind),
						c.cue.kind == ba::FoleyCueKind::Action ? ba::foley_action_name(c.cue.action) : (c.cue.short_reload > 0 ? "tactical" : "empty"),
						p.triggered ? p.wave.c_str() : "(nothing started)", p.triggered ? (double(p.start) - double(c.cue.frame)) * 1000.0 / ba::kMixRate : 0.0,
						p.triggered && p.end > p.start ? double(p.end - p.start) / ba::kMixRate : 0.0);
			}
			table += row;
		}
		// The release goes to the running reload; the reload wave is one segment and its sampler LtNone, which a Release
		// ends after its current segment (docs/design/m2a-audio.md section 6.3, as built): it plays to its end.
		if (rec.cues.size() >= 5) {
			const Played full = played(*f, rec.cues[2]);
			const double full_s = full.end > full.start ? double(full.end - full.start) / ba::kMixRate : 0.0;
			CHECK(full_s > 2.5); // the empty reload plays its whole wave (2.9 s)
		}
		CHECK(rig.router->stats().reloads == 4);
		CHECK(rig.router->stats().foley == 5);
		CHECK(rig.router->stats().foley_releases == 2);
		const std::string line = "pt1 rld: own M16A3 1p trigger table (event -> cue -> wave):" + table;
		MESSAGE(line);
		std::printf("%s\n", line.c_str());
		check_unchanged(*f);
	}

	TEST_CASE("every kit firearm's empty and tactical reload play their whole wave (1p)") {
		FoleyFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		std::string table;
		for (size_t di = 0; di < f->assets.table->defs.size(); ++di) {
			const ba::WeaponAudioDef &d = f->assets.table->defs[di];
			if (d.form != ba::WeaponAudioForm::Driver || !d.foley.reload.present) {
				continue;
			}
			double len[2] = { 0.0, 0.0 };
			std::string wave[2];
			std::string others[2];
			for (int tactical = 0; tactical < 2; ++tactical) {
				Rig rig(*f);
				FoleyRecorder rec;
				rig.router->set_observer(&rec);
				auto own = std::make_unique<host::PresentationFrame>();
				own->own_weapons_valid = true;
				own->own_weapons_curr.slots[0].def = uint16_t(di);
				own->own_weapons_curr.slots[0].magazine = tactical ? 7 : 0;
				rig.own_frame = own.get();
				for (int fi = 0; fi < 8 * 60; ++fi) {
					std::vector<host::PresentEvent> ev;
					if (fi == 30) {
						ev.push_back(own_timed(uint16_t(di), sw::weapon_events::kReloadStarted, 0.5));
					}
					rig.frame(ev);
				}
				REQUIRE(rec.cues.size() == 1);
				const Played p = played(*f, rec.cues[0]);
				CHECK_MESSAGE(p.triggered, d.patch);
				CHECK_MESSAGE(p.wave == d.foley.reload_waves[size_t(tactical)], d.patch);
				// Nothing else of the patch plays: the one-shot never gets the trigger release (a round on single fire).
				CHECK_MESSAGE(p.others == 0, (d.patch + ": " + p.other_waves));
				len[tactical] = p.end > p.start ? double(p.end - p.start) / ba::kMixRate : 0.0;
				wave[tactical] = p.wave;
				others[tactical] = p.other_waves;
			}
			// The empty reload is the longer wave; neither is cut by the one-shot pool's Stop (3 s after the start).
			CHECK_MESSAGE(len[0] > len[1], d.patch);
			char row[200];
			std::snprintf(row, sizeof(row), "\n  %-12s empty %-34s %.2f s, tactical %-34s %.2f s", d.patch.substr(d.patch.rfind('/') + 1).c_str(), wave[0].c_str(),
					len[0], wave[1].c_str(), len[1]);
			table += row;
			table += "\n    long: " + wave_shape(*f, wave[0]) + "; short: " + wave_shape(*f, wave[1]) + "; other samplers: [" + others[0] + "] [" + others[1] + "]";
		}
		const std::string line = "pt1 rld: kit reload waves as played:" + table;
		MESSAGE(line);
		std::printf("%s\n", line.c_str());
		check_unchanged(*f);
	}

	TEST_CASE("another soldier's reload in 3p: positioned, within the data's reach, cut by a switch") {
		FoleyFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const uint16_t m16 = def_of_patch(*f->assets.table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);
		const ba::WeaponAudioDef &d = f->assets.table->defs[m16];
		uint16_t m9 = sw::kNoWeapon;
		for (size_t i = 0; i < f->assets.table->defs.size(); ++i) {
			if (f->assets.table->defs[i].patch.find("/m9/") != std::string::npos) {
				m9 = uint16_t(i);
			}
		}
		REQUIRE(m9 != sw::kNoWeapon);
		// One soldier (slot 5) p_metres in front of the listener, facing it; the reload starts at 1 s (reload_seq 0 -> 1);
		// with p_switch_s > 0 he switches to the M9 then.
		struct Result3p {
			double db = 0.0, floor = 0.0;
			uint64_t reloads = 0, out_of_reach = 0, releases = 0;
			std::vector<FoleyRecorder::Cue> cues;
			double played_s = 0.0;
			std::string wave;
		};
		const auto run = [&](float p_metres, double p_switch_s, const char *p_wav) {
			Result3p r;
			Rig rig(*f);
			FoleyRecorder rec;
			rig.router->set_observer(&rec);
			host::PresentedSoldier s;
			s.slot = 5;
			s.generation = 1;
			s.position = { 0.0f, 0.0f, p_metres };
			s.yaw = 3.14159265f; // facing -Z, the listener
			host::PresentedOtherWeapon w;
			w.valid = true;
			w.slot = 5;
			w.generation = 1;
			w.def = m16;
			w.phase = sw::WeaponPhase::Ready;
			for (int fi = 0; fi < 4 * 60; ++fi) {
				if (fi == 60) {
					w.reload_seq = 1;
					w.phase = sw::WeaponPhase::Reloading;
				}
				if (p_switch_s > 0.0 && fi == int(std::llround(p_switch_s * 60.0))) {
					w.def = m9;
					w.phase = sw::WeaponPhase::Deploying;
				}
				w.sample_tick = uint32_t(kTicks0 + double(fi));
				rig.frame({}, std::span<const host::PresentedSoldier>(&s, 1), std::span<const host::PresentedOtherWeapon>(&w, 1));
			}
			r.floor = rig.loudest_db(0.0, 0.9);
			r.db = rig.loudest_db(1.0, p_switch_s > 0.0 ? p_switch_s : 3.9);
			r.reloads = rig.router->stats().reloads;
			r.out_of_reach = rig.router->stats().foley_out_of_reach;
			r.releases = rig.router->stats().foley_releases;
			r.cues = rec.cues;
			for (FoleyRecorder::Cue &c : r.cues) {
				c.trace = nullptr; // the traces end with the recorder
			}
			if (!rec.cues.empty() && rec.cues[0].cue.kind == ba::FoleyCueKind::Reload) {
				const Played p = played(*f, rec.cues[0]);
				r.played_s = p.end > p.start ? double(p.end - p.start) / ba::kMixRate : 0.0;
				r.wave = p.wave;
			}
			write_wav(*f, p_wav, rig.tap.mix);
			return r;
		};
		const Result3p at5 = run(5.0f, 0.0, "reload_3p_5m.wav");
		const Result3p at15 = run(15.0f, 0.0, "reload_3p_15m.wav");
		const Result3p at25 = run(25.0f, 0.0, "reload_3p_25m.wav");
		const Result3p cut = run(5.0f, 1.8, "reload_3p_5m_switch.wav");
		CHECK(at5.reloads == 1);
		CHECK(at15.reloads == 1);
		REQUIRE(!at5.cues.empty());
		CHECK(!at5.cues[0].cue.own);
		CHECK(!at5.cues[0].cue.first_person);
		CHECK(at5.cues[0].cue.short_reload == -1); // the others' magazine is not on the wire: the data's default (long)
		CHECK(at5.wave == d.foley.reload_waves[0]);
		CHECK(std::fabs(at5.cues[0].cue.distance_m - 5.0f) < 0.6f);
		CHECK(at5.db > at15.db + 6.0); // positioned: the Handling curve
		CHECK(at15.db > at15.floor + 20.0);
		// 25 m is past the reload's crossfader (19 m): the data starts nothing there, so no one-shot is spent on it.
		CHECK(at25.reloads == 0);
		CHECK(at25.out_of_reach == 1);
		// The switch at 1.8 s: the release of the running reload, then the M9's deploy.
		CHECK(cut.releases == 1);
		REQUIRE(cut.cues.size() == 3);
		CHECK(cut.cues[1].cue.kind == ba::FoleyCueKind::ReloadRelease);
		CHECK(cut.cues[2].cue.kind == ba::FoleyCueKind::Action);
		CHECK(cut.cues[2].cue.action == ba::FoleyAction::Deploy);
		CHECK(cut.cues[2].cue.def == m9);
		char line[400];
		std::snprintf(line, sizeof(line),
				"pt1 rld: another soldier's M16A3 reload 3p (floor %.1f dBFS): 5 m %.1f dBFS (%.2f s played), 15 m %.1f dBFS, 25 m %llu one-shots "
				"(%llu beyond the data's 19 m reach); switched 0.8 s in: %.2f s played, %llu release",
				at5.floor, at5.db, at5.played_s, at15.db, (unsigned long long)at25.reloads, (unsigned long long)at25.out_of_reach, cut.played_s,
				(unsigned long long)cut.releases);
		MESSAGE(line);
		std::printf("%s\n", line);
		check_unchanged(*f);
	}
	TEST_CASE("read_weapon_foley on 300 corruptions of the M16A3 program: no crash, bounded") {
		FoleyFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const uint16_t m16 = def_of_patch(*f->assets.table, kM16Patch);
		REQUIRE(m16 != sw::kNoWeapon);
		const ca::SoundProgram &base = *f->assets.table->defs[m16].program;
		uint64_t state = 0x9E3779B97F4A7C15ull;
		const auto next = [&state]() {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			return state;
		};
		size_t present = 0;
		for (int it = 0; it < 300; ++it) {
			ca::SoundProgram p = base;
			for (int k = 0; k < 24; ++k) {
				const uint64_t r = next();
				switch (r % 7) {
					case 0:
						if (!p.ports.empty()) {
							p.ports[(r >> 8) % p.ports.size()].reg = uint16_t(r >> 32);
						}
						break;
					case 1:
						if (!p.nodes.empty()) {
							ca::SoundNode &n = p.nodes[(r >> 8) % p.nodes.size()];
							n.ports_begin = uint32_t(r >> 32);
							n.children_count = uint16_t(r >> 48);
						}
						break;
					case 2:
						if (!p.plugins.empty()) {
							p.plugins[(r >> 8) % p.plugins.size()].connection = uint16_t(r >> 32);
						}
						break;
					case 3:
						if (!p.connections.empty()) {
							p.connections[(r >> 8) % p.connections.size()].voice = uint16_t(r >> 32);
						}
						break;
					case 4:
						if (!p.outputs.empty()) {
							p.outputs[(r >> 8) % p.outputs.size()].main_send_plugin.voice = uint16_t(r >> 32);
						}
						break;
					case 5:
						if (!p.children.empty()) {
							p.children[(r >> 8) % p.children.size()] = uint32_t(r >> 32);
						}
						break;
					default:
						if (!p.inputs.empty()) {
							p.inputs[(r >> 8) % p.inputs.size()].reg = uint16_t(r >> 32);
						}
						break;
				}
			}
			const ba::WeaponFoley fo = ba::read_weapon_foley(p);
			present += fo.reload.present ? 1 : 0;
			CHECK(fo.reload.samplers.size() <= p.nodes.size());
			CHECK(fo.reload.outputs.size() <= p.outputs.size());
			CHECK(fo.action.outputs.size() <= p.outputs.size());
			(void)ba::describe_weapon_foley(p, fo);
		}
		MESSAGE("pt1 rld: 300 corrupted M16A3 programs read, Reload still present in " << present);
		check_unchanged(*f);
	}
}

TEST_SUITE("audio-foley-unit") {
	TEST_CASE("read_weapon_foley on a hostile program: out-of-range ranges, registers and a send cycle") {
		ca::SoundProgram p;
		p.registers = 4;
		ca::SoundInput bad;
		bad.name_hash = ba::kHashActionType;
		bad.reg = 9; // past the registers
		p.inputs.push_back(bad);
		ca::SoundInput reload;
		reload.name_hash = 3294210644u; // Reload
		reload.reg = 1;
		reload.event = true;
		p.inputs.push_back(reload);
		ca::SoundNode receive;
		receive.type = ca::SoundNodeType::Receive;
		receive.children_begin = 1000; // past children
		receive.children_count = 7;
		p.nodes.push_back(receive);
		ca::SoundNode sampler;
		sampler.type = ca::SoundNodeType::Sampler;
		sampler.ports_begin = 0xFFFFFFF0u; // past ports
		sampler.ports_count = 40;
		sampler.plugins_begin = 77;
		sampler.plugins_count = 8;
		p.nodes.push_back(sampler);
		ca::SoundNode fader;
		fader.type = ca::SoundNodeType::MultiCrossfader;
		fader.ports_begin = 0;
		fader.ports_count = 3;
		fader.children_count = 5; // no children pool
		p.nodes.push_back(fader);
		ca::SoundPort port;
		port.reg = 3;
		p.ports.push_back(port);
		port.reg = 60000; // past the registers
		p.ports.push_back(port);
		port.reg = 1;
		p.ports.push_back(port);
		// Voices 0 -> 1 -> 0 (a cycle), a send to a missing connection and to a voice past the voices.
		ca::SoundVoice v;
		v.plugins_begin = 0;
		v.plugins_count = 2;
		p.voices.push_back(v);
		v.plugins_begin = 2;
		v.plugins_count = 5; // 2 past the plugins
		p.voices.push_back(v);
		ca::SoundVoicePlugin pl;
		pl.connection = 0;
		p.plugins.push_back(pl);
		pl.connection = 9; // no such connection
		p.plugins.push_back(pl);
		pl.connection = 1;
		p.plugins.push_back(pl);
		pl.connection = 2;
		p.plugins.push_back(pl);
		ca::SoundConnection c;
		c.voice = 1;
		p.connections.push_back(c);
		c.voice = 0;
		p.connections.push_back(c);
		c.voice = 500; // past the voices
		p.connections.push_back(c);
		ca::SoundOutput out;
		out.main_send_plugin.voice = 1;
		p.outputs.push_back(out);
		out.main_send_plugin.voice = 300; // past the voices
		p.outputs.push_back(out);
		const ba::WeaponFoley fo = ba::read_weapon_foley(p);
		CHECK(fo.reload.present);
		CHECK(fo.reload.samplers.empty());
		CHECK(fo.reload.release_event == 0u);
		CHECK(!fo.action.present);
		CHECK(!fo.action_type_param); // its register is out of range: not an input
		CHECK(fo.action_waves.empty());
		const std::vector<uint16_t> o0 = ba::voice_outputs(p, 0);
		CHECK(o0 == std::vector<uint16_t>{ 0 }); // through the cycle to voice 1, which holds output 0's send
		CHECK(ba::voice_outputs(p, 1) == std::vector<uint16_t>{ 0 });
		CHECK(ba::voice_outputs(p, 9).empty());
		CHECK(!ba::describe_weapon_foley(p, fo).empty());
		// An empty program.
		const ba::WeaponFoley none = ba::read_weapon_foley(ca::SoundProgram{});
		CHECK(!none.reload.present);
		CHECK(!none.action.present);
		CHECK(!none.bolt.present);
	}
}

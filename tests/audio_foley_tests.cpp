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
		float cull_db = 0.0f;
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
				"pt1 rld: own M16A3 1p: cull loudness of a reload instance at the listener %.1f dB; EnableHdr outputs at 0 m: %s", double(cull_db),
				outs.c_str());
		MESSAGE(line);
		std::printf("%s\n", line);
		std::snprintf(line, sizeof(line),
				"pt1 rld: own M16A3 1p: reload alone %.1f dBFS (floor %.1f, window top %.1f dB, %llu reload one-shots); 30-round burst %.1f dBFS; "
				"the reload 0.6-2.9 s after it %.1f dBFS (window top %.1f dB); its 1.5-2.9 s standing %.1f dBFS, walking 3 m/s %.1f dBFS",
				alone_db, alone_floor, double(alone_top), (unsigned long long)alone_reloads, fire_db, empty_db, double(empty_top), alone_late, walk_late);
		MESSAGE(line);
		std::printf("%s\n", line);
		check_unchanged(*f);
	}
}

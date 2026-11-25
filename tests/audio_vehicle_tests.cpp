#include "test_support.h"

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/audio_load.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/vehicle_audio.h"
#include "bf3/audio/voice_cull.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/client_audio/game_audio_frame.h"
#include "bf3/client_audio/kit_audio.h"
#include "bf3/client_audio/vehicle_sounds.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/gameplay/vehicle_sound.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/util/finite.h"
#include "bf3/host/level_load.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/presented_vehicle.h"
#include "bf3/sim/vehicles/vehicle_armament.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3;
namespace ba = bf3::audio;
namespace ca = bf3::core::audio;
namespace cg = bf3::core::gameplay;
using P = ba::VehicleSoundParam;

namespace {

constexpr uint32_t kFrames = 800;

struct VehicleFixture {
	test::Mounted mount;
	std::unique_ptr<host::LevelDatabase> db;
	std::shared_ptr<const host::LevelVehicles> level;
	ba::VehicleAudioAssets assets;
	std::shared_ptr<const ca::WaveBank> bank;
	ba::AudioTuning tuning;
	ba::MixerA3Assets a3;
	std::vector<std::string> report;
};

VehicleFixture *vehicle_fixture() {
	static std::unique_ptr<VehicleFixture> f;
	static bool tried = false;
	if (tried) {
		return f.get();
	}
	tried = true;
	auto made = std::make_unique<VehicleFixture>();
	if (!test::open_install(made->mount)) {
		return nullptr;
	}
	core::Result<host::LevelDatabase> db =
			host::open_level_database(made->mount.install, "MP_007", "ConquestSmall0", 0, nullptr);
	REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
	made->db = std::make_unique<host::LevelDatabase>(std::move(*db));
	core::Result<std::shared_ptr<const host::LevelVehicles>> level =
			host::load_level_vehicles(*made->db, sim::VehicleTuning{});
	REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
	made->level = *level;
	core::Result<ba::VehicleAudioAssets> assets =
			ba::load_vehicle_audio(made->db->level->db(), *made->level, made->report);
	REQUIRE_MESSAGE(assets.ok(), (assets.ok() ? std::string() : assets.error().message));
	made->assets = std::move(*assets);
	core::Result<std::vector<ca::SoundWaveInfo>> waves =
			ba::read_program_waves(made->db->level->db(), made->assets.programs);
	REQUIRE_MESSAGE(waves.ok(), (waves.ok() ? std::string() : waves.error().message));
	core::Result<std::shared_ptr<const ca::WaveBank>> bank = ca::WaveBank::load(
			*made->mount.install, made->db->level->mount(), std::move(*waves), ca::WaveBankOptions{});
	REQUIRE_MESSAGE(bank.ok(), (bank.ok() ? std::string() : bank.error().message));
	made->bank = *bank;
	std::ifstream cfg(OPENBF3_AUDIO_TUNING_CFG, std::ios::binary);
	REQUIRE(cfg.good());
	std::stringstream text;
	text << cfg.rdbuf();
	core::Result<ba::AudioTuning> tuning = ba::parse_audio_tuning(text.str());
	REQUIRE_MESSAGE(tuning.ok(), (tuning.ok() ? std::string() : tuning.error().message));
	made->tuning = *tuning;
	made->a3 = ba::load_mixer_a3(made->db->level->db(), made->assets.programs);
	f = std::move(made);
	return f.get();
}

VehicleFixture *need_install() {
	VehicleFixture *f = vehicle_fixture();
	if (f == nullptr) {
		test::skip_install_case("no BF3 install found");
	}
	return f;
}

void check_unchanged(const VehicleFixture &p_f) {
	const core::SnapshotDiff diff = core::diff_snapshots(p_f.mount.before, p_f.mount.reader.snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

std::string connected_inputs(const ca::SoundProgram &p_program) {
	std::string out;
	for (size_t i = 0; i < ba::kVehicleSoundParams; ++i) {
		if (p_program.input_register(ba::vehicle_sound_param_hash(P(i))).has_value()) {
			out += (out.empty() ? "" : ",") + std::to_string(i);
		}
	}
	return out;
}

std::string other_inputs(const ca::SoundProgram &p_program) {
	std::string out;
	for (const ca::SoundInput &in : p_program.inputs) {
		bool known = in.name_hash == uint32_t(cg::vehicle_sound_port::kStart) ||
				in.name_hash == uint32_t(cg::vehicle_sound_port::kStop);
		for (size_t i = 0; i < ba::kVehicleSoundParams; ++i) {
			known = known || in.name_hash == ba::vehicle_sound_param_hash(P(i));
		}
		if (!known) {
			char hex[16];
			std::snprintf(hex, sizeof hex, "%08x%s", in.name_hash, in.event ? " event" : "");
			out += (out.empty() ? "" : ",") + std::string(hex);
		}
	}
	return out;
}

std::optional<uint16_t> kind_of(const host::LevelVehicles &p_level, std::string_view p_blueprint) {
	for (size_t i = 0; i < p_level.kinds.size(); ++i) {
		if (p_level.kinds[i].blueprint == p_blueprint) {
			return uint16_t(i);
		}
	}
	return std::nullopt;
}

std::optional<uint16_t> spawner_of(const host::LevelVehicles &p_level, uint16_t p_kind) {
	for (size_t i = 0; i < p_level.spawners.size(); ++i) {
		if (p_level.spawners[i].kind == p_kind) {
			return uint16_t(i);
		}
	}
	return std::nullopt;
}

struct DriveRig {
	struct Whole final : ba::MixTap {
		std::vector<float> mix;
		void on_block(const float *p, uint32_t n, uint64_t) override { mix.insert(mix.end(), p, p + size_t(n) * 2); }
	} tap;
	std::unique_ptr<ba::MixerCore> mixer;
	std::unique_ptr<ba::VehicleAudio> audio;
	std::vector<float> buffer = std::vector<float>(size_t(kFrames) * 2);
	std::vector<ba::VehicleSoundSource> sources;
	ba::VehicleSoundMemory memory;
	std::vector<ba::MixerCutEvent> cuts;
	uint64_t frame_index = 0;
	float interior_cam = -1.0f;

	explicit DriveRig(const VehicleFixture &p_f) {
		const ba::GameMixerSizes sizes = ba::game_mixer_sizes(p_f.tuning);
		ba::MixerConfig mc = ba::game_mixer_config(p_f.tuning, sizes, 8192);
		ba::apply_mixer_a3(p_f.a3, mc);
		mixer = std::make_unique<ba::MixerCore>(mc);
		mixer->set_tap(&tap);
		mixer->record_cuts(true);
		audio = std::make_unique<ba::VehicleAudio>(p_f.assets.programs, p_f.bank, p_f.tuning);
	}

	void frame(const VehicleFixture &p_f, const host::PresentedVehicle &p_v, const ba::VehicleListenerView &p_view) {
		const host::PresentedVehicle one[1] = { p_v };
		ba::vehicle_sound_sources(one, *p_f.level, p_f.assets, p_view, memory, sources);
		if (interior_cam >= 0.0f) {
			for (ba::VehicleSoundSource &s : sources) {
				s.params[size_t(P::InteriorCam)] = interior_cam;
			}
		}
		ba::MixerCommand listen;
		listen.type = ba::MixerCommandType::SetListener;
		listen.listener = p_view.listener;
		REQUIRE(mixer->push(listen));
		audio->step(*mixer, sources, p_view.listener);
		mixer->mix(buffer.data(), kFrames);
		std::array<ba::MixerCutEvent, 64> batch;
		for (size_t n = mixer->poll_cuts(batch); n > 0; n = mixer->poll_cuts(batch)) {
			for (size_t i = 0; i < n; ++i) {
				audio->on_cut(batch[i]);
				cuts.push_back(batch[i]);
			}
		}
		++frame_index;
	}

	double rms(double p_from_s, double p_to_s) const {
		const uint64_t a = uint64_t(p_from_s * ba::kMixRate);
		const uint64_t b = std::min<uint64_t>(uint64_t(p_to_s * ba::kMixRate), tap.mix.size() / 2);
		return b > a ? double(ba::rms_db(tap.mix, 2, a, b - a)) : -300.0;
	}

	double finished_s() const {
		for (const ba::MixerCutEvent &e : cuts) {
			if (e.kind == ba::MixerCutKind::Finish) {
				return double(e.frame) / double(ba::kMixRate);
			}
		}
		return -1.0;
	}

	double band(double p_from_s, double p_to_s, float p_low, float p_high) const {
		const uint64_t a = uint64_t(p_from_s * ba::kMixRate);
		const uint64_t b = std::min<uint64_t>(uint64_t(p_to_s * ba::kMixRate), tap.mix.size() / 2);
		return double(ba::band_energy_db(tap.mix, 2, ba::kMixRate, a, b - a, p_low, p_high));
	}
};

host::PresentedVehicle parked(const host::LevelVehicles &p_level, uint16_t p_spawner) {
	const sim::VehicleModel &model = *p_level.kinds[p_level.spawners[p_spawner].kind].model;
	host::PresentedVehicle v;
	v.spawner = p_spawner;
	v.generation = 1;
	v.seat_count = uint8_t(std::min<size_t>(p_level.kinds[p_level.spawners[p_spawner].kind].seats, v.occupants.size()));
	v.occupants.fill(net::kNoVehicleOccupant);
	v.com_position = { 0.0f, 100.0f, 0.0f };
	v.wheel_count = model.wheel_count;
	for (size_t i = 0; i < model.wheel_count; ++i) {
		v.suspension_length[i] = 0.5f * (model.wheels[i].suspension_min + model.wheels[i].suspension_max);
		v.wheel_contact |= uint8_t(1u << i);
	}
	return v;
}

uint32_t sound_view_camera(const VehicleFixture &p_f, uint16_t p_kind, size_t p_seat) {
	const std::vector<uint32_t> &wired = p_f.assets.defs[p_kind].engines.front().first_person_cameras;
	for (const cg::VehicleCameraDef &c : p_f.level->kinds[p_kind].seat_defs.at(p_seat).cameras) {
		if (std::find(wired.begin(), wired.end(), c.instance) != wired.end()) {
			return c.instance;
		}
	}
	return cg::kNoVehicleCamera;
}

void drive(host::PresentedVehicle &r_v, const sim::VehicleModel &p_model, float p_speed, float p_throttle) {
	r_v.linear_velocity = { 0.0f, 0.0f, p_speed };
	r_v.throttle = p_throttle;
	for (size_t i = 0; i < p_model.wheel_count; ++i) {
		r_v.wheel_spin[i] = p_model.wheels[i].radius > 0.0f ? p_speed / p_model.wheels[i].radius : 0.0f;
	}
}

}

TEST_SUITE("audio-vehicles-install") {
	TEST_CASE("Caspian Conquest Small's vehicles: engine patches, engine positions and the inputs they declare") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		for (const std::string &line : f->report) {
			std::printf("%s\n", line.c_str());
		}
		struct Expect {
			const char *blueprint;
			const char *engine;
			const char *child;
			const char *bullet_impact;
			const char *inputs;
		};
		const Expect expect[] = {
			{ "vehicles/growleritv/growleritv", "sound/vehicles/land/growler/growler_noboost",
					"sound/weapons/stationary/common/manual_turret_spin",
					"sound/bullet_craft/bullet_impacts/vehicle_interior/lightvehicle_interior_projectile_impact",
					"0,1,2,3,4,5,6,7,8,9,10,11,12,13,16" },
			{ "vehicles/vdv_buggy/vdv_buggy", "sound/vehicles/land/vdv_buggy/vdv_buggy_noboost",
					"sound/weapons/stationary/common/manual_turret_spin",
					"sound/bullet_craft/bullet_impacts/vehicle_interior/lightvehicle_interior_projectile_impact",
					"0,1,2,3,4,5,6,7,8,9,10,11,12,13,16" },
			{ "vehicles/m1a2/m1abrams", "sound/vehicles/land/m1a2_abrams/m1a2_abrams", "",
					"sound/bullet_craft/bullet_impacts/vehicle_interior/vehicle_interior_projectile_impact",
					"0,1,2,3,4,5,7,8,9,10,11,12,14,15,16" },
			{ "vehicles/t90/t90", "sound/vehicles/land/t90/t90", "",
					"sound/bullet_craft/bullet_impacts/vehicle_interior/vehicle_interior_projectile_impact",
					"0,1,2,3,4,5,7,8,9,10,11,12,14,15,16" },
		};
		REQUIRE(f->level->kinds.size() == 4);
		CHECK(f->assets.programs.size() == 5);
		for (const Expect &e : expect) {
			const std::optional<uint16_t> kind = kind_of(*f->level, e.blueprint);
			REQUIRE_MESSAGE(kind.has_value(), e.blueprint);
			const cg::VehicleSoundDef &def = f->assets.defs[*kind];
			REQUIRE(def.engines.size() == 1);
			const cg::VehicleEngineSound &engine = def.engines.front();
			const uint16_t program = f->assets.engine_program[*kind];
			REQUIRE(program < f->assets.programs.size());
			const std::string inputs = connected_inputs(*f->assets.programs[program]);
			std::printf("%s: engine %s at (%.4f, %.4f, %.4f), first person sounds %d, surface `%s`, %zu turret sounds "
						"(first `%s`), impact %s, view switch %s / %s, inputs [%s], disabled fraction %.3f\n",
					e.blueprint, engine.patch.c_str(), double(engine.transform.trans.x),
					double(engine.transform.trans.y), double(engine.transform.trans.z), int(engine.first_person_sounds),
					engine.surface_patch.c_str(), def.turrets.size(),
					def.turrets.empty() ? "" : def.turrets.front().patch.c_str(), def.bullet_impact.c_str(),
					def.switch_to_alternate_view.c_str(), def.switch_to_normal_view.c_str(), inputs.c_str(),
					double(f->assets.disabled_fraction[*kind]));
			CHECK(engine.patch == e.engine);
			CHECK(inputs == e.inputs);
			CHECK(engine.surface_patch.empty());
			CHECK_FALSE(engine.first_person_sounds);
			CHECK(def.bullet_impact == e.bullet_impact);
			CHECK((def.turrets.empty() ? std::string() : def.turrets.front().patch) == e.child);
			for (const cg::VehicleTurretSound &c : def.turrets) {
				const host::LevelVehicleKind &k = f->level->kinds[*kind];
				REQUIRE(k.armament != nullptr);
				REQUIRE(c.turret >= 0);
				REQUIRE(size_t(c.turret) < k.armament->turrets.size());
				std::printf("  turret sound %s on turret %d (seat %d, of %zu turrets) at (%.3f, %.3f, %.3f)\n",
						c.patch.c_str(), int(c.turret), int(k.armament->turrets[size_t(c.turret)].seat),
						k.armament->turrets.size(), double(c.position.x), double(c.position.y), double(c.position.z));
				CHECK(k.armament->turrets[size_t(c.turret)].seat > 0);
			}
			CHECK(f->assets.programs[program]->start_event == uint32_t(cg::vehicle_sound_port::kStart));
			CHECK(f->assets.programs[program]->stop_event == uint32_t(cg::vehicle_sound_port::kStop));
		}
		check_unchanged(*f);
	}

	TEST_CASE("every land vehicle kind of other maps' Conquest Large: its engine patch compiles, declares its inputs "
			  "and gives a running source for a driver") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		struct Expect {
			const char *blueprint;
			const char *engine;
			const char *inputs;
			const char *others;
			size_t turrets;
		};
		const char *jeep = "0,1,2,3,4,5,6,7,8,9,10,11,12,13,16";
		const char *tracked = "0,1,2,3,4,5,7,8,9,10,11,12,14,15,16";
		const Expect expect[] = {
			{ "vehicles/humveearmored/humveearmored", "sound/vehicles/land/humvee/humvee_noboost", jeep, "", 0 },
			{ "vehicles/bmp2/bmp2", "sound/vehicles/land/bmp2/bmp2_02", tracked, "", 0 },
			{ "vehicles/m1a2/m1abrams", "sound/vehicles/land/m1a2_abrams/m1a2_abrams", tracked, "", 0 },
			{ "vehicles/t90/t90", "sound/vehicles/land/t90/t90", tracked, "", 0 },
			{ "vehicles/growleritv/growleritv", "sound/vehicles/land/growler/growler_noboost", jeep, "", 1 },
			{ "vehicles/vdv_buggy/vdv_buggy", "sound/vehicles/land/vdv_buggy/vdv_buggy_noboost", jeep, "", 1 },
			{ "vehicles/9k22_tunguska_m/9k22_tunguska_m", "sound/vehicles/land/tunguska/tunguska", tracked, "", 0 },
			{ "vehicles/aav-7a1/aav-7a1", "sound/vehicles/land/aav-7a1/aav-7a1", tracked, "", 0 },
			{ "vehicles/xpack01/dpv/dpv", "sound/vehicles/land/dpv_faav/dpv_faav_noboost", jeep, "", 1 },
			{ "vehicles/xpack01/skidloader/skidloader", "sound/vehicles/land/constructionloader/constructionloader",
					"0,1,2,3,4,5,7,8,9,10,11,12,16", "", 0 },
			{ "vehicles/xp3/quadbike/quadbike", "sound/vehicles/land/atv_quadbike/atv_quadbike", jeep, "", 0 },
			{ "vehicles/xp3/2s25-sprut-sd/2s25_sprut-sd", "sound/vehicles/land/bmp2/bmp2_02", tracked, "", 0 },
			{ "vehicles/xp4/humveemodified/humveemodified", "sound/vehicles/land/humvee/humvee_noboost_xp4", jeep, "",
					2 },
			{ "vehicles/xp4/vodnikmodified/vodnikmodified_v2", "sound/vehicles/land/vodnik/vodnik_03_noboost_xp4", jeep,
					"", 2 },
			{ "vehicles/xp4/vanmodified/vanmodified", "sound/vehicles/land/van_xp4/van_xp4", jeep, "", 1 },
			{ "vehicles/xp5/humvee_asrad/humvee_asrad", "sound/vehicles/land/xp5/humvee_asrad_xp5_noboost", jeep,
					"3ea2059a event,1163a458 event", 0 },
			{ "vehicles/xp5/bmp2_paradrop/bmp2_paradrop", "sound/vehicles/land/bmp2/bmp2_02", tracked, "", 0 },
			{ "vehicles/lav25/lav25", "sound/vehicles/land/lav_25/lav_25", tracked, "", 0 },
			{ "vehicles/lav25/lav_ad", "sound/vehicles/land/lav_25/lav_25", tracked, "", 0 },
			{ "vehicles/xp5/lav_paradrop/lav25_paradrop", "sound/vehicles/land/lav_25/lav_25", tracked, "", 0 },
			{ "vehicles/xp3/m1128-stryker/m1128-stryker", "sound/vehicles/land/lav_25/lav_25", tracked, "", 0 },
			{ "vehicles/xpack01/btr-90/btr90", "sound/vehicles/land/btr90/btr90", tracked, "", 0 },
			{ "vehicles/gaz-3937_vodnik/gaz-3937_vodnik", "sound/vehicles/land/vodnik/vodnik_03_noboost", jeep, "", 0 },
			{ "vehicles/xp5/vodnikphoenix/vodnikphoenix", "sound/vehicles/land/xp5/vodnikphoenix_xp5", jeep,
					"3ea2059a event,1163a458 event", 0 },
			{ "vehicles/xp3/star_1466/star_1466", "sound/vehicles/land/bm23/bm23",
					"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", "", 1 },
			{ "vehicles/xp3/himars/himars", "sound/vehicles/land/himars/himars",
					"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", "", 1 },
			{ "vehicles/xp5/klr650/klr650", "sound/vehicles/land/motorcycle/dirtbike", jeep,
					"40db8171,255c4d9f event,d19f5904 event", 0 },
		};
		const char *levels[] = { "MP_001", "MP_012", "MP_017", "XP1_002", "XP3_Desert", "XP4_Quake", "XP5_001" };
		std::vector<std::string> seen;
		for (const char *name : levels) {
			CAPTURE(name);
			core::Result<host::LevelDatabase> db =
					host::open_level_database(f->mount.install, name, "ConquestLarge0", 0, nullptr);
			REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
			core::Result<std::shared_ptr<const host::LevelVehicles>> level =
					host::load_level_vehicles(*db, sim::VehicleTuning{});
			REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
			std::vector<std::string> report;
			core::Result<ba::VehicleAudioAssets> assets = ba::load_vehicle_audio(db->level->db(), **level, report);
			REQUIRE_MESSAGE(assets.ok(), (assets.ok() ? std::string() : assets.error().message));
			for (const std::string &line : report) {
				std::printf("%s ConquestLarge0 %s\n", name, line.c_str());
				CHECK(line.find("NOT COMPILED") == std::string::npos);
			}
			for (size_t k = 0; k < (*level)->kinds.size(); ++k) {
				const host::LevelVehicleKind &kind = (*level)->kinds[k];
				CAPTURE(kind.blueprint);
				const Expect *e = std::find_if(std::begin(expect), std::end(expect),
						[&](const Expect &p_x) { return kind.blueprint == p_x.blueprint; });
				REQUIRE_MESSAGE(e != std::end(expect), kind.blueprint);
				const cg::VehicleSoundDef &def = assets->defs[k];
				REQUIRE(def.engines.size() == 1);
				const uint16_t program = assets->engine_program[k];
				REQUIRE(program < assets->programs.size());
				const std::string inputs = connected_inputs(*assets->programs[program]);
				const std::string others = other_inputs(*assets->programs[program]);
				CHECK(def.engines.front().patch == e->engine);
				CHECK(inputs == e->inputs);
				CHECK(others == e->others);
				CHECK(def.turrets.size() == e->turrets);
				CHECK(def.engines.front().surface_patch.empty());
				const uint16_t spawner = spawner_of(**level, uint16_t(k)).value();
				host::PresentedVehicle v = parked(**level, spawner);
				v.occupants[0] = 3;
				ba::VehicleListenerView view;
				view.listener.position = { 10.0f, 101.6f, 0.0f };
				ba::VehicleSoundMemory memory;
				std::vector<ba::VehicleSoundSource> sources;
				const host::PresentedVehicle one[1] = { v };
				ba::vehicle_sound_sources(one, **level, *assets, view, memory, sources);
				REQUIRE(sources.size() == 1);
				CHECK(sources.front().program == program);
				CHECK(sources.front().running);
				CHECK(core::finite(sources.front().emitter.position));
				if (std::find(seen.begin(), seen.end(), kind.blueprint) == seen.end()) {
					seen.push_back(kind.blueprint);
					std::printf("%s: %s, inputs [%s], other inputs [%s], %zu turret sounds\n", kind.blueprint.c_str(),
							def.engines.front().patch.c_str(), inputs.c_str(), others.c_str(), def.turrets.size());
				}
			}
		}
		CHECK(seen.size() == std::size(expect));
		check_unchanged(*f);
	}
}

TEST_SUITE("audio-vehicles-weapons") {
	TEST_CASE("vehicle weapons: every MP_007 ConquestSmall0 vehicle weapon gets a sound source after the soldier "
			  "sources, and its fire sound compiles to a voice") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::vector<core::gameplay::WeaponDef> &weapons = f->level->weapons;
		REQUIRE(!weapons.empty());
		ba::KitAudioSources kits;
		kits.sources.resize(3);
		ba::add_vehicle_weapon_sounds(kits, weapons);
		REQUIRE(kits.sources.size() == 3 + weapons.size());
		core::Result<ba::WeaponAudioTable> table = ba::load_weapon_audio(f->db->level->db(), kits.sources, 48000);
		REQUIRE_MESSAGE(table.ok(), (table.ok() ? std::string() : table.error().message));
		size_t voiced = 0;
		for (size_t i = 0; i < weapons.size(); ++i) {
			const ba::WeaponAudioDef &d = table->defs[3 + i];
			std::printf("vehicle weapon %s [%s]: %s\n", weapons[i].variant.c_str(), weapons[i].sound.c_str(),
					d.form == ba::WeaponAudioForm::None ? "no voice" : d.patch.c_str());
			CHECK(kits.sources[3 + i].sound == weapons[i].sound);
			voiced += !weapons[i].sound.empty() && d.form != ba::WeaponAudioForm::None ? 1u : 0u;
		}
		std::printf("vehicle weapons: %zu of %zu voiced\n", voiced, weapons.size());
		CHECK(voiced == weapons.size());
		CHECK(table->failed == 0);
	}
}
TEST_SUITE("audio-vehicles-drive") {
	TEST_CASE("a scripted Growler drive: engine off parked, started by a driver, louder under throttle and speed, "
			  "the interior camera, stopped when the driver leaves") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/growleritv/growleritv");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		const sim::VehicleModel &model = *f->level->kinds[*kind].model;
		DriveRig rig(*f);
		host::PresentedVehicle v = parked(*f->level, *spawner);
		ba::VehicleListenerView outside;
		outside.listener.position = { 10.0f, 101.6f, 0.0f };
		outside.listener.forward = { -1.0f, 0.0f, 0.0f };
		outside.own_slot = 7;
		for (int i = 0; i < 60; ++i) {
			rig.frame(*f, v, outside);
		}
		CHECK(rig.audio->stats().starts == 0);
		const double parked_db = rig.rms(0.0, 1.0);
		v.occupants[0] = 3;
		for (int i = 0; i < 120; ++i) {
			rig.frame(*f, v, outside);
		}
		CHECK(rig.audio->stats().starts == 1);
		const double idle_db = rig.rms(2.0, 3.0);
		for (int i = 0; i < 180; ++i) {
			const float t = float(i) / 180.0f;
			drive(v, model, 15.0f * t, 1.0f);
			v.com_position.z = 4.0f * t;
			rig.frame(*f, v, outside);
		}
		const double drive_db = rig.rms(5.0, 6.0);
		const ba::VehicleAudioStats driving = rig.audio->stats();
		CHECK(driving.peak_velocity > 14.0f);
		CHECK(driving.peak_throttle == doctest::Approx(1.0f));
		CHECK(driving.updates > 0);
		const double outside_high = rig.band(5.0, 6.0, 2000.0f, 8000.0f);
		ba::VehicleListenerView inside = outside;
		const core::Transform rot = core::Transform::from_rotation_translation(v.rotation, core::Vec3{});
		const core::Vec3 origin = core::vec::sub(v.com_position, rot.transform_vector(model.center_of_mass));
		inside.listener.position = core::vec::add(origin, f->level->kinds[*kind].box.centre);
		inside.listener.forward = { 0.0f, 0.0f, 1.0f };
		inside.own_camera = sound_view_camera(*f, *kind, 0);
		REQUIRE(inside.own_camera != cg::kNoVehicleCamera);
		for (int i = 0; i < 30; ++i) {
			rig.frame(*f, v, inside);
		}
		CHECK(rig.audio->stats().interior_frames == 0);
		const double exterior_high = rig.band(6.25, 6.5, 2000.0f, 8000.0f);
		const double exterior_db = rig.rms(6.25, 6.5);
		v.occupants[0] = 7;
		for (int i = 0; i < 30; ++i) {
			rig.frame(*f, v, inside);
		}
		const double inside_high = rig.band(6.75, 7.0, 2000.0f, 8000.0f);
		const double inside_db = rig.rms(6.75, 7.0);
		std::printf("growler at the hull centre, someone else driving: %.1f dB (2-8 kHz %.1f dB); the own soldier "
					"driving, interior camera: %.1f dB (2-8 kHz %.1f dB)\n",
				exterior_db, exterior_high, inside_db, inside_high);
		CHECK(rig.audio->stats().interior_frames >= 29);
		CHECK(rig.audio->stats().starts == 1);
		rig.interior_cam = 0.0f;
		for (int i = 0; i < 30; ++i) {
			rig.frame(*f, v, inside);
		}
		rig.interior_cam = -1.0f;
		const double camera_off_high = rig.band(7.25, 7.5, 2000.0f, 8000.0f);
		const double camera_off_db = rig.rms(7.25, 7.5);
		std::printf("growler at the hull centre, the own soldier driving, InteriorCam alone set to 0: %.1f dB (2-8 kHz "
					"%.1f dB)\n",
				camera_off_db, camera_off_high);
		CHECK(std::fabs(camera_off_db - inside_db) > 1.0);
		v.occupants[0] = net::kNoVehicleOccupant;
		drive(v, model, 0.0f, 0.0f);
		for (int i = 0; i < 1200; ++i) {
			rig.frame(*f, v, outside);
		}
		const ba::VehicleAudioStats end = rig.audio->stats();
		for (int second = 8; second < 27; second += 2) {
			std::printf("growler after the driver left, %d..%d s: %.1f dB\n", second, second + 1,
					rig.rms(double(second), double(second + 1)));
		}
		std::printf("growler engine finished %.2f s after the driver left\n", rig.finished_s() - 7.5);
		CHECK(end.stops == 1);
		CHECK(end.ended == 1);
		CHECK(rig.finished_s() > 7.5);
		CHECK(end.live == 0);
		CHECK(end.push_failures == 0);
		CHECK(end.create_failures == 0);
		const double tail_db = rig.rms(26.5, 27.5);
		std::printf("growler drive: parked %.1f dB, idle %.1f dB, 15 m/s full throttle %.1f dB (2-8 kHz %.1f dB), "
					"interior %.1f dB (2-8 kHz %.1f dB), after the driver left %.1f dB; %llu starts, %llu updates, "
					"%llu stops, %llu ended, %llu interior frames\n",
				parked_db, idle_db, drive_db, outside_high, inside_db, inside_high, tail_db,
				(unsigned long long)end.starts, (unsigned long long)end.updates, (unsigned long long)end.stops,
				(unsigned long long)end.ended, (unsigned long long)end.interior_frames);
		CHECK(parked_db < -100.0);
		CHECK(idle_db > -60.0);
		CHECK(drive_db > idle_db + 3.0);
		CHECK(tail_db < -100.0);
		check_unchanged(*f);
	}

	TEST_CASE("the Growler's gunner turret: its traverse sound starts while it turns, manned or not, reads its speed "
			  "as RPM and stops when it stops") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/growleritv/growleritv");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		REQUIRE(f->assets.turret_programs[*kind].size() == 1);
		const auto [turret, program] = f->assets.turret_programs[*kind].front();
		DriveRig rig(*f);
		host::PresentedVehicle v = parked(*f->level, *spawner);
		v.turret_count = uint8_t(f->level->kinds[*kind].armament->turrets.size());
		ba::VehicleListenerView view;
		view.listener.position = { 3.0f, 101.6f, 0.0f };
		view.listener.forward = { -1.0f, 0.0f, 0.0f };
		const uint32_t key = ba::vehicle_sound_key(*spawner, v.generation, 1u + uint32_t(turret));
		for (int i = 0; i < 30; ++i) {
			v.tick = sim::Tick(100 + i);
			v.turret_angle[size_t(turret)] = float(i - 30) * (std::numbers::pi_v<float> / 60.0f);
			rig.frame(*f, v, view);
		}
		CHECK(rig.audio->stats().starts == 1);
		const double still_db = rig.rms(0.0, 0.5);
		const int8_t seat = f->level->kinds[*kind].armament->turrets[size_t(turret)].seat;
		REQUIRE(seat > 0);
		v.occupants[size_t(seat)] = 5;
		float peak_rpm = 0.0f;
		for (int i = 0; i < 60; ++i) {
			v.tick = sim::Tick(130 + i);
			v.turret_angle[size_t(turret)] = float(i) * (std::numbers::pi_v<float> / 60.0f);
			rig.frame(*f, v, view);
			for (const ba::VehicleSoundSource &src : rig.sources) {
				if (src.key == key) {
					peak_rpm = std::max(peak_rpm, src.params[size_t(P::Rpm)]);
				}
			}
		}
		CHECK(rig.audio->handle_of(key, program) != ba::kNoInstance);
		CHECK(rig.audio->stats().starts == 1);
		const double turning_db = rig.rms(1.0, 1.5);
		for (int i = 0; i < 900; ++i) {
			v.tick = sim::Tick(190 + i);
			rig.frame(*f, v, view);
		}
		const ba::VehicleAudioStats end = rig.audio->stats();
		std::printf("growler turret finished %.2f s after it stopped turning\n", rig.finished_s() - 1.5);
		const double after_db = rig.rms(15.5, 16.5);
		std::printf("growler turret: unmanned %.1f dB, turning at %.2f rpm %.1f dB, stopped %.1f dB; %llu starts, %llu "
					"stops, %llu ended\n",
				still_db, double(peak_rpm), turning_db, after_db, (unsigned long long)end.starts,
				(unsigned long long)end.stops, (unsigned long long)end.ended);
		CHECK(peak_rpm == doctest::Approx(30.0f).epsilon(0.001));
		CHECK(end.stops == 1);
		CHECK(end.ended == 1);
		CHECK(rig.finished_s() > 1.5);
		CHECK(still_db > -70.0);
		CHECK(turning_db > -70.0);
		CHECK(after_db < -100.0);
		check_unchanged(*f);
	}
}

TEST_SUITE("audio-vehicles-drive") {
	TEST_CASE("the M1A2 and the T-90 at idle heard from their hull centres: the interior camera of their own driver "
			  "cuts the engine's high band") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		struct Tank {
			const char *blueprint;
			double interior_drop_db;
		};
		for (const Tank &tank : { Tank{ "vehicles/m1a2/m1abrams", 6.0 }, Tank{ "vehicles/t90/t90", 4.0 } }) {
			const char *blueprint = tank.blueprint;
			CAPTURE(blueprint);
			const std::optional<uint16_t> kind = kind_of(*f->level, blueprint);
			REQUIRE(kind.has_value());
			const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
			REQUIRE(spawner.has_value());
			const sim::VehicleModel &model = *f->level->kinds[*kind].model;
			DriveRig rig(*f);
			host::PresentedVehicle v = parked(*f->level, *spawner);
			v.occupants[0] = 3;
			ba::VehicleListenerView view;
			const core::Transform rot = core::Transform::from_rotation_translation(v.rotation, core::Vec3{});
			const core::Vec3 origin = core::vec::sub(v.com_position, rot.transform_vector(model.center_of_mass));
			view.listener.position = core::vec::add(origin, f->level->kinds[*kind].box.centre);
			view.own_slot = 7;
			view.own_camera = sound_view_camera(*f, *kind, 0);
			REQUIRE(view.own_camera != cg::kNoVehicleCamera);
			for (int i = 0; i < 120; ++i) {
				rig.frame(*f, v, view);
			}
			const double exterior_high = rig.band(1.0, 2.0, 4000.0f, 12000.0f);
			const double exterior_db = rig.rms(1.0, 2.0);
			v.occupants[0] = 7;
			for (int i = 0; i < 120; ++i) {
				rig.frame(*f, v, view);
			}
			const double interior_high = rig.band(3.0, 4.0, 4000.0f, 12000.0f);
			const double interior_db = rig.rms(3.0, 4.0);
			std::printf(
					"%s idle at the hull centre: another driver %.1f dB (4-12 kHz %.1f dB), the own soldier driving "
					"%.1f dB (4-12 kHz %.1f dB)\n",
					blueprint, exterior_db, exterior_high, interior_db, interior_high);
			CHECK(rig.audio->stats().starts == 1);
			CHECK(rig.audio->stats().interior_frames >= 119);
			CHECK(interior_db < exterior_db - tank.interior_drop_db);
			CHECK(interior_high < exterior_high - 6.0);
			rig.interior_cam = 0.0f;
			for (int i = 0; i < 120; ++i) {
				rig.frame(*f, v, view);
			}
			const double camera_off_high = rig.band(5.0, 6.0, 4000.0f, 12000.0f);
			const double camera_off_db = rig.rms(5.0, 6.0);
			std::printf(
					"%s idle at the hull centre, the own soldier driving, InteriorCam alone set to 0: %.1f dB (4-12 "
					"kHz %.1f dB)\n",
					blueprint, camera_off_db, camera_off_high);
			CHECK(camera_off_high > interior_high + 6.0);
		}
		check_unchanged(*f);
	}

	TEST_CASE("the engine and turret sounds take the presented vehicle's health as their Health input") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/m1a2/m1abrams");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		host::PresentedVehicle v = parked(*f->level, *spawner);
		v.occupants[0] = 3;
		ba::VehicleListenerView view;
		view.own_slot = 7;
		ba::VehicleSoundMemory memory;
		std::vector<ba::VehicleSoundSource> sources;
		for (const float health : { 1.0f, 0.4f, 0.0f }) {
			CAPTURE(health);
			v.health = health;
			const host::PresentedVehicle one[1] = { v };
			ba::vehicle_sound_sources(one, *f->level, f->assets, view, memory, sources);
			REQUIRE_FALSE(sources.empty());
			for (const ba::VehicleSoundSource &s : sources) {
				CHECK(s.params[size_t(P::Health)] == health);
			}
		}
		check_unchanged(*f);
	}

	TEST_CASE("present_vehicle keeps the wire's throttle filter and a bit per wheel in contact") {
		net::VehicleWire w;
		w.state.wheel_count = 4;
		w.state.wheels[0].in_contact = 1;
		w.state.wheels[2].in_contact = 1;
		w.state.wheels[3].in_contact = 1;
		w.state.driver.throttle_filter = -0.75f;
		const host::PresentedVehicle v = host::present_vehicle(w, 40);
		CHECK(v.throttle == -0.75f);
		CHECK(v.wheel_contact == 0b1101);
	}
}

namespace {

std::optional<float> patch_input(const ba::VehicleSoundSource &p_source, std::string_view p_name) {
	const uint32_t hash = uint32_t(cg::logic_port_id(p_name));
	for (size_t i = 0; i < ba::kVehicleSoundParams; ++i) {
		if (ba::vehicle_sound_param_hash(P(i)) == hash) {
			return p_source.params[i];
		}
	}
	return std::nullopt;
}

struct InputVector {
	const char *name;
	float value;
};

void check_inputs(const ba::VehicleSoundSource &p_source, std::initializer_list<InputVector> p_vector) {
	for (const InputVector &e : p_vector) {
		CAPTURE(e.name);
		const std::optional<float> got = patch_input(p_source, e.name);
		std::printf(
				"  %s = %s (notes %.6g)\n", e.name, got ? std::to_string(*got).c_str() : "not sent", double(e.value));
		REQUIRE(got.has_value());
		CHECK(*got == doctest::Approx(e.value).epsilon(1e-4).scale(1.0));
	}
}

std::array<float, 4> axis_rotation(int p_axis, float p_radians) {
	std::array<float, 4> q{ 0.0f, 0.0f, 0.0f, std::cos(0.5f * p_radians) };
	q[size_t(p_axis)] = std::sin(0.5f * p_radians);
	return q;
}

const ba::VehicleSoundSource &engine_source(const std::vector<ba::VehicleSoundSource> &p_sources, uint32_t p_key) {
	const auto it = std::find_if(
			p_sources.begin(), p_sources.end(), [&](const ba::VehicleSoundSource &s) { return s.key == p_key; });
	REQUIRE(it != p_sources.end());
	return *it;
}

void set_spring(host::PresentedVehicle &r_v, const sim::VehicleModel &p_model, float p_compression) {
	for (size_t i = 0; i < p_model.wheel_count; ++i) {
		r_v.suspension_length[i] = p_model.wheels[i].suspension_max * (1.0f - p_compression);
	}
}

void roll_wheels(host::PresentedVehicle &r_v, const sim::VehicleModel &p_model, float p_speed) {
	for (size_t i = 0; i < p_model.wheel_count; ++i) {
		r_v.wheel_spin[i] = p_model.wheels[i].radius > 0.0f ? p_speed / p_model.wheels[i].radius : 0.0f;
	}
}

}

TEST_SUITE("audio-vehicles-inputs") {
	TEST_CASE(
			"R5-VS4 A: the M1A2's engine patch inputs for its own driver at idle and climbing 10 degrees at 20 km/h") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/m1a2/m1abrams");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		const sim::VehicleModel &model = *f->level->kinds[*kind].model;
		host::PresentedVehicle v = parked(*f->level, *spawner);
		v.occupants[0] = 7;
		set_spring(v, model, 0.2f);
		const host::LevelVehicleKind &tank = f->level->kinds[*kind];
		const std::vector<uint32_t> &wired = f->assets.defs[*kind].engines.front().first_person_cameras;
		std::printf("M1A2 cameras feeding UseFirstPersonSounds:");
		for (const uint32_t c : wired) {
			std::printf(" %u", c);
		}
		std::printf("\n");
		CHECK(wired.size() == 4);
		uint32_t driver_view = cg::kNoVehicleCamera, driver_chase = cg::kNoVehicleCamera;
		uint32_t gunner_view = cg::kNoVehicleCamera;
		uint8_t gunner_seat = 0;
		for (size_t s = 0; s < tank.seat_defs.size(); ++s) {
			for (const cg::VehicleCameraDef &c : tank.seat_defs[s].cameras) {
				const bool feeds = std::find(wired.begin(), wired.end(), c.instance) != wired.end();
				std::printf("  seat %zu camera %u %s, %s\n", s, c.instance, c.first_person ? "first person" : "chase",
						feeds ? "feeds the engine" : "does not feed the engine");
				CHECK(feeds == c.first_person);
				if (s == 0 && c.first_person && driver_view == cg::kNoVehicleCamera) {
					driver_view = c.instance;
				}
				if (s == 0 && !c.first_person && driver_chase == cg::kNoVehicleCamera) {
					driver_chase = c.instance;
				}
				if (s > 0 && feeds && gunner_view == cg::kNoVehicleCamera) {
					gunner_view = c.instance;
					gunner_seat = uint8_t(s);
				}
			}
		}
		REQUIRE(driver_view != cg::kNoVehicleCamera);
		REQUIRE(driver_chase != cg::kNoVehicleCamera);
		REQUIRE(gunner_view != cg::kNoVehicleCamera);
		ba::VehicleListenerView view;
		view.own_slot = 7;
		view.own_camera = driver_view;
		const core::Transform rot = core::Transform::from_rotation_translation(v.rotation, core::Vec3{});
		view.listener.position =
				core::vec::add(core::vec::sub(v.com_position, rot.transform_vector(model.center_of_mass)),
						f->level->kinds[*kind].box.centre);
		ba::VehicleSoundMemory memory;
		std::vector<ba::VehicleSoundSource> sources;
		const uint32_t key = ba::vehicle_sound_key(*spawner, v.generation, ba::kVehicleEngineSound);
		for (int i = 0; i < 60; ++i) {
			v.tick = sim::Tick(100 + i);
			const host::PresentedVehicle one[1] = { v };
			ba::vehicle_sound_sources(one, *f->level, f->assets, view, memory, sources);
		}
		std::printf("M1A2 A1 idle, own driver in the first-person view:\n");
		check_inputs(engine_source(sources, key),
				{ { "Velocity", 0.0f }, { "Throttle", 0.0f }, { "Load", 0.0f }, { "Gear", 0.0f }, { "RPM", 0.0f },
						{ "Slip", 0.0f }, { "SpringCompression", 0.2f }, { "Contact", 1.0f },
						{ "AngularVelocity", 0.0f }, { "Roll", 0.0f }, { "Tilt", 0.0f }, { "GForce", 1.0f },
						{ "Health", 1.0f }, { "DisabledDamageThreshold", 0.5f }, { "InDriverPos", 1.0f },
						{ "LocalPlayerInVehicle", 1.0f }, { "InteriorCam", 1.0f }, { "RelativeVelocity", 0.0f },
						{ "Boost", 0.0f } });
		const auto one_view = [&](uint32_t p_camera) {
			view.own_camera = p_camera;
			const host::PresentedVehicle one[1] = { v };
			ba::vehicle_sound_sources(one, *f->level, f->assets, view, memory, sources);
			return engine_source(sources, key);
		};
		std::printf("M1A2, the same driver in the chase camera:\n");
		check_inputs(one_view(driver_chase), { { "InteriorCam", 0.0f }, { "LocalPlayerInVehicle", 1.0f } });
		v.occupants[0] = 3;
		v.occupants[gunner_seat] = 7;
		std::printf(
				"M1A2, the own soldier in seat %u's first-person view, someone else driving:\n", unsigned(gunner_seat));
		check_inputs(one_view(gunner_view),
				{ { "InteriorCam", 1.0f }, { "LocalPlayerInVehicle", 1.0f }, { "InDriverPos", 1.0f } });
		v.occupants[gunner_seat] = net::kNoVehicleOccupant;
		v.occupants[0] = 7;
		view.own_camera = driver_view;
		const float pitch = 10.0f * std::numbers::pi_v<float> / 180.0f;
		v.rotation = axis_rotation(0, -pitch);
		const core::Transform climb = core::Transform::from_rotation_translation(v.rotation, core::Vec3{});
		const float speed = 20.0f / 3.6f;
		v.linear_velocity = core::vec::scale(climb.forward, speed);
		view.listener.velocity = v.linear_velocity;
		roll_wheels(v, model, speed);
		v.throttle = 1.0f;
		v.gear = 1;
		v.engine_rpm = 700.0f;
		for (int i = 0; i < 60; ++i) {
			v.tick = sim::Tick(200 + i);
			const host::PresentedVehicle one[1] = { v };
			ba::vehicle_sound_sources(one, *f->level, f->assets, view, memory, sources);
		}
		std::printf("M1A2 A5 full throttle, 20 km/h, climbing 10 degrees:\n");
		check_inputs(engine_source(sources, key),
				{ { "Velocity", speed }, { "Throttle", 1.0f }, { "Gear", 1.0f }, { "RPM", 700.0f },
						{ "Tilt", std::sin(pitch) }, { "Roll", 0.5f * (1.0f - std::cos(pitch)) },
						{ "Load", std::asin(std::sin(pitch)) * 4.0f / std::numbers::pi_v<float> }, { "Slip", 0.0f },
						{ "Contact", 1.0f }, { "GForce", 1.0f }, { "RelativeVelocity", 0.0f } });
		check_unchanged(*f);
	}

	TEST_CASE("R5-VS4 B: the Growler's engine patch inputs with wheels in the air, yawing on its side, and passing an "
			  "outside listener") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/growleritv/growleritv");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		const sim::VehicleModel &model = *f->level->kinds[*kind].model;
		REQUIRE(model.wheel_count == 4);
		host::PresentedVehicle v = parked(*f->level, *spawner);
		v.occupants[0] = 3;
		set_spring(v, model, 0.25f);
		ba::VehicleListenerView view;
		view.listener.position = { 30.0f, 100.0f, 0.0f };
		view.own_slot = 7;
		ba::VehicleSoundMemory memory;
		std::vector<ba::VehicleSoundSource> sources;
		const uint32_t key = ba::vehicle_sound_key(*spawner, v.generation, ba::kVehicleEngineSound);
		const auto run = [&](sim::Tick p_from) {
			for (int i = 0; i < 60; ++i) {
				v.tick = p_from + sim::Tick(i);
				const host::PresentedVehicle one[1] = { v };
				ba::vehicle_sound_sources(one, *f->level, f->assets, view, memory, sources);
			}
			return engine_source(sources, key);
		};
		float speed = 30.0f / 3.6f;
		v.linear_velocity = { 0.0f, 0.0f, speed };
		roll_wheels(v, model, speed);
		v.throttle = 0.5f;
		v.wheel_contact = 0b1110;
		std::printf("Growler B5, one wheel in the air at 30 km/h, someone else driving:\n");
		check_inputs(run(100),
				{ { "Contact", 1.0f }, { "Slip", 1.0f }, { "SpringCompression", 0.25f }, { "InDriverPos", 1.0f },
						{ "LocalPlayerInVehicle", 0.0f }, { "InteriorCam", 0.0f },
						{ "DisabledDamageThreshold", 0.6f } });
		speed = 50.0f / 3.6f;
		v.linear_velocity = { 0.0f, 0.0f, speed };
		roll_wheels(v, model, speed);
		v.wheel_contact = 0;
		std::printf("Growler B6, all four wheels in the air at 50 km/h:\n");
		check_inputs(run(200), { { "Contact", 0.0f }, { "Slip", 4.0f } });
		const float roll = 20.0f * std::numbers::pi_v<float> / 180.0f;
		v.rotation = axis_rotation(2, roll);
		v.linear_velocity = {};
		roll_wheels(v, model, 0.0f);
		v.angular_velocity = { 0.0f, 0.5f, 0.0f };
		v.wheel_contact = 0b1111;
		std::printf("Growler on its side by 20 degrees, yawing at 0.5 rad/s:\n");
		check_inputs(run(300),
				{ { "AngularVelocity", 0.5f }, { "Roll", 0.5f * (1.0f - std::cos(roll)) }, { "Tilt", 0.0f },
						{ "Slip", 0.0f } });
		v.rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
		v.angular_velocity = {};
		speed = 60.0f / 3.6f;
		v.linear_velocity = { 0.0f, 0.0f, speed };
		roll_wheels(v, model, speed);
		std::printf("Growler B7, passing a standing outside listener at 60 km/h:\n");
		check_inputs(run(400),
				{ { "RelativeVelocity", speed }, { "Velocity", speed }, { "InteriorCam", 0.0f },
						{ "LocalPlayerInVehicle", 0.0f }, { "InDriverPos", 1.0f } });
		check_unchanged(*f);
	}

	TEST_CASE("R5-VS1 Rule 1: the engine sound lives from the driver's entry to the end of its stop sound; a re-entry "
			  "in the tail posts Start on the same sound, and every input change is sent") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/growleritv/growleritv");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		const sim::VehicleModel &model = *f->level->kinds[*kind].model;
		DriveRig rig(*f);
		host::PresentedVehicle v = parked(*f->level, *spawner);
		ba::VehicleListenerView outside;
		outside.listener.position = { 10.0f, 101.6f, 0.0f };
		outside.own_slot = 7;
		v.occupants[0] = 3;
		for (int i = 0; i < 120; ++i) {
			rig.frame(*f, v, outside);
		}
		CHECK(rig.audio->stats().starts == 1);
		const uint64_t updates = rig.audio->stats().updates;
		drive(v, model, 1.0f, 0.5f);
		rig.frame(*f, v, outside);
		const uint64_t moved = rig.audio->stats().updates;
		v.linear_velocity.z += 0.01f;
		rig.frame(*f, v, outside);
		std::printf("Growler inputs sent: %llu at idle, %llu after a 1 m/s move, %llu after a further 0.01 m/s\n",
				(unsigned long long)updates, (unsigned long long)moved, (unsigned long long)rig.audio->stats().updates);
		CHECK(rig.audio->stats().updates > moved);
		v.occupants[0] = net::kNoVehicleOccupant;
		for (int i = 0; i < 6; ++i) {
			rig.frame(*f, v, outside);
		}
		CHECK(rig.audio->stats().stops == 1);
		v.occupants[0] = 3;
		for (int i = 0; i < 60; ++i) {
			rig.frame(*f, v, outside);
		}
		std::printf("Growler engine after a re-entry 0.1 s into its stop sound: %llu sounds started, %llu stops, %llu "
					"ended, %u live\n",
				(unsigned long long)rig.audio->stats().starts, (unsigned long long)rig.audio->stats().stops,
				(unsigned long long)rig.audio->stats().ended, rig.audio->stats().live);
		CHECK(rig.audio->stats().starts == 1);
		CHECK(rig.audio->stats().live == 1);
		check_unchanged(*f);
	}

	TEST_CASE("R5-VS1 Rule 7: the Growler mount's spin sound starts when it turns at 0.1 rpm or more and stops when it "
			  "slows below 0.1 rpm, manned or not") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/growleritv/growleritv");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		REQUIRE(f->assets.turret_programs[*kind].size() == 1);
		const auto [turret, program] = f->assets.turret_programs[*kind].front();
		DriveRig rig(*f);
		host::PresentedVehicle v = parked(*f->level, *spawner);
		v.turret_count = uint8_t(f->level->kinds[*kind].armament->turrets.size());
		ba::VehicleListenerView view;
		view.listener.position = { 3.0f, 101.6f, 0.0f };
		const uint32_t key = ba::vehicle_sound_key(*spawner, v.generation, 1u + uint32_t(turret));
		float angle = 0.0f;
		const auto turn = [&](float p_rpm, int p_ticks, sim::Tick p_from) {
			float rpm = 0.0f;
			for (int i = 0; i < p_ticks; ++i) {
				v.tick = p_from + sim::Tick(i);
				angle += p_rpm / 9.549296f / float(sim::kSimHz);
				v.turret_angle[size_t(turret)] = angle;
				rig.frame(*f, v, view);
				for (const ba::VehicleSoundSource &src : rig.sources) {
					if (src.key == key) {
						rpm = src.params[size_t(P::Rpm)];
					}
				}
			}
			return rpm;
		};
		turn(0.05f, 30, 100);
		const uint64_t slow = rig.audio->stats().starts;
		const float rpm = turn(0.5f, 30, 130);
		std::printf("Growler mount, unmanned: 0.05 rpm gives %llu starts; 0.5 rpm gives %llu starts, RPM input %.4f\n",
				(unsigned long long)slow, (unsigned long long)rig.audio->stats().starts, double(rpm));
		CHECK(slow == 0);
		CHECK(rig.audio->stats().starts == 1);
		CHECK(rig.audio->handle_of(key, program) != ba::kNoInstance);
		CHECK(rpm == doctest::Approx(0.5f).epsilon(0.01));
		turn(0.0f, 10, 160);
		CHECK(rig.audio->stats().stops == 1);
		check_unchanged(*f);
	}

	TEST_CASE("an engine instance the mixer culls while its driver is in is made again and Started; one that finished "
			  "is not") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/growleritv/growleritv");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		DriveRig rig(*f);
		host::PresentedVehicle v = parked(*f->level, *spawner);
		v.occupants[0] = 3;
		ba::VehicleListenerView view;
		view.own_slot = 7;
		view.listener.position = { 10.0f, 101.6f, 0.0f };
		for (int i = 0; i < 30; ++i) {
			rig.frame(*f, v, view);
		}
		const uint32_t key = ba::vehicle_sound_key(*spawner, v.generation, ba::kVehicleEngineSound);
		const uint16_t program = f->assets.engine_program[*kind];
		const auto lose = [&](ba::MixerCutKind p_kind) {
			ba::MixerCutEvent e;
			e.kind = p_kind;
			e.handle = rig.audio->handle_of(key, program);
			REQUIRE(e.handle != ba::kNoInstance);
			rig.audio->on_cut(e);
			for (int i = 0; i < 3; ++i) {
				rig.frame(*f, v, view);
			}
			return rig.audio->handle_of(key, program) != ba::kNoInstance;
		};
		CHECK(lose(ba::MixerCutKind::Cull));
		CHECK(rig.audio->stats().starts == 2);
		CHECK(rig.audio->stats().start_events == 2);
		CHECK_FALSE(lose(ba::MixerCutKind::Finish));
		CHECK(rig.audio->stats().starts == 2);
		check_unchanged(*f);
	}

	TEST_CASE("the M1A2 engine patch's DeltaGroup reads 0.4 the update SpringCompression steps from 0.2 to 0.6") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/m1a2/m1abrams");
		REQUIRE(kind.has_value());
		const std::shared_ptr<const ca::SoundProgram> &program = f->assets.programs[f->assets.engine_program[*kind]];
		std::vector<uint16_t> deltas;
		for (const ca::SoundNode &n : program->nodes) {
			if (n.type != ca::SoundNodeType::Delta) {
				continue;
			}
			for (uint16_t c = 0; c < n.children_count; ++c) {
				const ca::SoundNode &g = program->child_nodes[program->children[n.children_begin + c]];
				deltas.push_back(program->ports[g.ports_begin + ca::port::DeltaGroupDelta].reg);
			}
		}
		REQUIRE(deltas.size() == 1);
		REQUIRE(deltas.front() != ca::kNoRegister);
		core::Result<std::unique_ptr<ba::PatchInstance>> inst =
				ba::PatchInstance::create(program, f->bank, f->tuning, 1, ba::kMixRate);
		REQUIRE(inst.ok());
		const uint32_t spring = ba::vehicle_sound_param_hash(P::SpringCompression);
		(*inst)->set_parameter(spring, 0.2f);
		(*inst)->evaluate(1.0 / 60.0);
		(*inst)->evaluate(1.0 / 60.0);
		const float before = (*inst)->value(deltas.front());
		(*inst)->set_parameter(spring, 0.6f);
		(*inst)->evaluate(1.0 / 60.0);
		const float step = (*inst)->value(deltas.front());
		std::printf(
				"M1A2 engine patch Delta: %.3f at rest, %.3f on the 0.2 -> 0.6 update\n", double(before), double(step));
		CHECK(before == 0.0f);
		CHECK(step == doctest::Approx(0.4f));
	}
}

namespace {

using Op = ba::WeaponSoundOp;
using OpKind = ba::WeaponSoundOpKind;

Op post_op(uint32_t p_hash) {
	return Op{ OpKind::Post, p_hash, 0.0f };
}

Op set_op(uint32_t p_hash, float p_value) {
	return Op{ OpKind::Set, p_hash, p_value };
}

std::vector<uint32_t> posts(const std::vector<Op> &p_ops) {
	std::vector<uint32_t> out;
	for (const Op &o : p_ops) {
		if (o.kind == OpKind::Post) {
			out.push_back(o.hash);
		}
	}
	return out;
}

std::optional<float> set_value(const std::vector<Op> &p_ops, uint32_t p_hash) {
	std::optional<float> v;
	for (const Op &o : p_ops) {
		if (o.kind == OpKind::Set && o.hash == p_hash) {
			v = o.value;
		}
	}
	return v;
}

bool has_kind(const std::vector<Op> &p_ops, OpKind p_kind) {
	return std::any_of(p_ops.begin(), p_ops.end(), [&](const Op &o) { return o.kind == p_kind; });
}

std::vector<Op> step(ba::WeaponSoundState &r_s, bool p_playing) {
	std::vector<Op> out;
	r_s.update(p_playing, out);
	return out;
}

std::vector<Op> playing_inputs(float p_logic, bool p_p, bool p_a, bool p_b, float p_heat = 0.0f) {
	return { set_op(ba::kHashFireLogicType, p_logic), set_op(ba::kHashIsFirstPerson, p_p ? 1.0f : 0.0f),
		set_op(ba::kHashOwnerSameTeam, p_a ? 1.0f : 0.0f), set_op(ba::kHashOwnerLocal, p_b ? 1.0f : 0.0f),
		set_op(ba::kHashOverheatLevel, p_heat), set_op(ba::kHashLockAmount, 0.0f), set_op(ba::kHashWeaponFlag, 1.0f) };
}

std::vector<Op> joined(std::vector<Op> p_a, const std::vector<Op> &p_b) {
	p_a.insert(p_a.end(), p_b.begin(), p_b.end());
	return p_a;
}

}

namespace {

struct WeaponRig {
	struct Starts final : ba::MixTap {
		void on_block(const float *, uint32_t, uint64_t) override {}
	} tap;
	ba::KitAudioSources kits;
	std::shared_ptr<const ba::WeaponAudioTable> table;
	std::shared_ptr<const ca::WaveBank> bank;
	std::unique_ptr<ba::MixerCore> mixer;
	std::unique_ptr<ba::OneShotPool> pool;
	std::unique_ptr<ba::SimMixerClock> clock;
	std::unique_ptr<ba::WeaponAudioRouter> router;
	ba::ListenerState listener;
	sim::weapons::SimTime render = 2000 * sim::weapons::kTimeUnitsPerTick;
	std::vector<float> buffer = std::vector<float>(size_t(ba::kMixRate / sim::kSimHz) * 2);
	std::vector<ba::MixerCutEvent> cuts;
	bool crews = false;
	uint8_t own_team = 0;
	std::vector<uint32_t> vehicles;
	std::vector<ba::VehicleCrewSeat> seats;
	std::vector<ba::VehicleWeaponPose> poses;
	std::vector<ba::VehicleWeaponOp> ops;

	explicit WeaponRig(const VehicleFixture &p_f) {
		kits.sources.resize(3);
		ba::add_vehicle_weapon_sounds(kits, p_f.level->weapons);
		core::Result<ba::WeaponAudioTable> t = ba::load_weapon_audio(p_f.db->level->db(), kits.sources, ba::kMixRate);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		table = std::make_shared<const ba::WeaponAudioTable>(std::move(*t));
		std::vector<std::shared_ptr<const ca::SoundProgram>> programs;
		for (const ba::WeaponAudioDef &d : table->defs) {
			if (d.program) {
				programs.push_back(d.program);
			}
		}
		core::Result<std::vector<ca::SoundWaveInfo>> waves = ba::read_program_waves(p_f.db->level->db(), programs);
		REQUIRE_MESSAGE(waves.ok(), (waves.ok() ? std::string() : waves.error().message));
		core::Result<std::shared_ptr<const ca::WaveBank>> b = ca::WaveBank::load(
				*p_f.mount.install, p_f.db->level->mount(), std::move(*waves), ca::WaveBankOptions{});
		REQUIRE_MESSAGE(b.ok(), (b.ok() ? std::string() : b.error().message));
		bank = *b;
		const ba::GameMixerSizes sizes = ba::game_mixer_sizes(p_f.tuning);
		ba::MixerConfig mc = ba::game_mixer_config(p_f.tuning, sizes, 8192);
		mixer = std::make_unique<ba::MixerCore>(mc);
		mixer->set_tap(&tap);
		mixer->record_cuts(true);
		pool = std::make_unique<ba::OneShotPool>(sizes.one_shots);
		clock = std::make_unique<ba::SimMixerClock>(*mixer);
		router = std::make_unique<ba::WeaponAudioRouter>(*mixer, table, bank, p_f.tuning, *pool);
		listener.position = { 0.0f, 101.6f, 0.0f };
	}
	~WeaponRig() { router.reset(); }

	uint16_t def_of(const VehicleFixture &p_f, std::string_view p_variant) const {
		for (size_t i = 0; i < p_f.level->weapons.size(); ++i) {
			if (p_f.level->weapons[i].variant == p_variant) {
				return uint16_t(3 + i);
			}
		}
		FAIL("no vehicle weapon " << p_variant);
		return 0;
	}

	present::PresentEvent shot(uint8_t p_slot, uint16_t p_def, uint32_t p_id) const {
		present::PresentEvent e;
		e.kind = present::PresentKind::RemoteShot;
		e.timeline = present::PresentTimeline::Render;
		e.flags = present::flags::kVehicle;
		e.slot = p_slot;
		e.def = p_def;
		e.id = p_id;
		e.time = render;
		e.pos[0] = 0.0f;
		e.pos[1] = 102.0f;
		e.pos[2] = 20.0f;
		e.dir[2] = 1.0f;
		return e;
	}

	void frame(std::span<const present::PresentEvent> p_events = {}) {
		present::PresentClock now;
		now.predicted_valid = now.render_valid = true;
		now.predicted = render;
		now.render = render;
		clock->set_now(now);
		router->set_listener(listener);
		if (crews) {
			router->set_vehicle_crews(own_team, vehicles, seats, poses);
		}
		router->on_events(p_events, *clock);
		router->on_others({}, {}, {}, *clock);
		router->update(0);
		router->take_vehicle_weapon_ops(ops);
		mixer->mix(buffer.data(), uint32_t(buffer.size() / 2));
		std::array<ba::MixerCutEvent, 64> batch;
		for (size_t n = mixer->poll_cuts(batch); n > 0; n = mixer->poll_cuts(batch)) {
			cuts.insert(cuts.end(), batch.begin(), batch.begin() + std::ptrdiff_t(n));
		}
		render += sim::weapons::kTimeUnitsPerTick;
	}

	std::vector<Op> ops_of(uint8_t p_slot, uint16_t p_def) {
		std::vector<Op> out;
		for (const ba::VehicleWeaponOp &o : ops) {
			if (o.slot == p_slot && o.def == p_def) {
				out.push_back(o.op);
			}
		}
		return out;
	}

	std::vector<Op> take(uint8_t p_slot, uint16_t p_def) {
		std::vector<Op> out = ops_of(p_slot, p_def);
		ops.clear();
		return out;
	}

	size_t requests(std::string_view p_program) const {
		return size_t(std::count_if(cuts.begin(), cuts.end(), [&](const ba::MixerCutEvent &e) {
			return e.kind == ba::MixerCutKind::Request &&
					std::string_view(e.program).find(p_program) != std::string_view::npos;
		}));
	}
};

}

TEST_SUITE("audio-vehicles-weapon-events") {
	TEST_CASE("R5-VS3 Rule 2: one update posts Reload, ReloadEnd, Action, Fire, Bolt, one ShotFired, LockOn and "
			  "FireStopped in that order, then sets the playing inputs") {
		ba::WeaponSoundState s;
		const ba::WeaponSoundOwner own{ true, true, true };
		s.reload_start(true, true);
		s.reload_end();
		s.action(ba::kFireModeAction, 0.0f, 0);
		s.fire_start(own);
		s.bolt();
		s.shot();
		s.lock_on(own, 0.25f);
		s.fire_end();
		REQUIRE(s.wants_handle());
		const std::vector<Op> ops = step(s, true);
		const std::vector<Op> expect =
				joined({ set_op(ba::kHashIsShortReload, 1.0f), post_op(ba::kHashReload), post_op(ba::kHashReloadEnd),
							   set_op(ba::kHashActionType, 6.0f), set_op(ba::kHashZoomLevel, 0.0f),
							   post_op(ba::kHashAction), post_op(ba::kHashFire), post_op(ba::kHashBolt),
							   post_op(ba::kHashShotFired), post_op(ba::kHashLockOn), post_op(ba::kHashFireStopped) },
						{ set_op(ba::kHashFireLogicType, 0.0f), set_op(ba::kHashIsFirstPerson, 1.0f),
								set_op(ba::kHashOwnerSameTeam, 1.0f), set_op(ba::kHashOwnerLocal, 1.0f),
								set_op(ba::kHashOverheatLevel, 0.0f), set_op(ba::kHashLockAmount, 0.25f),
								set_op(ba::kHashWeaponFlag, 1.0f) });
		CHECK(ops == expect);
		CHECK_FALSE(s.wants_handle());
		CHECK(posts(step(s, true)).empty());
	}

	TEST_CASE("R5-VS3 test vector: the M1A2 cannon fired once by its local gunner in first person posts Fire, "
			  "ShotFired and FireStopped in one update; a reload reports short only in first person") {
		ba::WeaponSoundState s;
		CHECK_FALSE(s.wants_handle());
		s.set_fire_logic(0);
		s.fire_start({ true, true, true });
		s.shot();
		s.fire_end();
		CHECK(s.queued_shots() == 1);
		const std::vector<Op> ops = step(s, true);
		CHECK(ops ==
				joined({ post_op(ba::kHashFire), post_op(ba::kHashShotFired), post_op(ba::kHashFireStopped) },
						playing_inputs(0.0f, true, true, true)));
		CHECK_FALSE(s.firing());
		s.reload_start(true, true);
		CHECK(step(s, true) ==
				joined({ set_op(ba::kHashIsShortReload, 1.0f), post_op(ba::kHashReload) },
						playing_inputs(0.0f, true, true, true)));
		s.reload_start(false, true);
		CHECK(set_value(step(s, true), ba::kHashIsShortReload) == 0.0f);
		s.reload_end();
		CHECK(posts(step(s, true)) == std::vector<uint32_t>{ ba::kHashReloadEnd });
	}

	TEST_CASE("R5-VS3 test vector: a remote coax with three shots before one update and a trigger release spreads "
			  "one ShotFired per update and posts FireStopped after the last") {
		ba::WeaponSoundState s;
		const ba::WeaponSoundOwner remote{ false, true, false };
		s.fire_start(remote);
		s.shot();
		s.shot();
		s.shot();
		s.trigger_release();
		CHECK(s.ending());
		CHECK(posts(step(s, true)) == std::vector<uint32_t>{ ba::kHashFire, ba::kHashShotFired });
		CHECK(s.queued_shots() == 2);
		CHECK(posts(step(s, true)) == std::vector<uint32_t>{ ba::kHashShotFired });
		const std::vector<Op> last = step(s, true);
		CHECK(posts(last) == std::vector<uint32_t>{ ba::kHashShotFired, ba::kHashFireStopped });
		CHECK(set_value(last, ba::kHashFireLogicType) == 2.0f);
		CHECK(set_value(last, ba::kHashOwnerLocal) == 0.0f);
		CHECK(posts(step(s, true)).empty());
	}

	TEST_CASE("R5-VS3 test vector: the local gunner's own coax is not ended by the trigger release once it fired; "
			  "the fire end does, and FireStopped follows once every ShotFired is out") {
		ba::WeaponSoundState s;
		s.fire_start({ true, true, true });
		s.shot();
		s.shot();
		s.trigger_release();
		CHECK_FALSE(s.ending());
		CHECK(posts(step(s, true)) == std::vector<uint32_t>{ ba::kHashFire, ba::kHashShotFired });
		s.fire_end();
		CHECK(posts(step(s, true)) == std::vector<uint32_t>{ ba::kHashShotFired, ba::kHashFireStopped });
		s.fire_start({ true, true, true });
		s.trigger_release();
		CHECK(s.ending());
		CHECK(posts(step(s, true)) == std::vector<uint32_t>{ ba::kHashFire, ba::kHashFireStopped });
	}

	TEST_CASE("R5-VS3 test vector: a remote enemy HMG sends P, A and B as 0 and gives its handle up once it stops "
			  "playing; the local gunner keeps his; a destroyed weapon still playing posts FireStopped and its end "
			  "event and plays out detached") {
		ba::WeaponSoundState enemy;
		enemy.fire_start({ false, false, false });
		enemy.shot();
		enemy.trigger_release();
		const std::vector<Op> ops = step(enemy, true);
		CHECK(ops ==
				joined({ post_op(ba::kHashFire), post_op(ba::kHashShotFired), post_op(ba::kHashFireStopped) },
						playing_inputs(2.0f, false, false, false)));
		CHECK(step(enemy, false) == std::vector<Op>{ Op{ OpKind::Release, 0, 0.0f } });
		ba::WeaponSoundState own;
		own.fire_start({ false, true, true });
		CHECK(step(own, false) == std::vector<Op>{ post_op(ba::kHashFire) });
		ba::WeaponSoundState gone;
		gone.fire_start({ false, true, false });
		gone.shot();
		std::vector<Op> out;
		gone.destroyed(true, out);
		CHECK(out ==
				std::vector<Op>{
						post_op(ba::kHashFireStopped), post_op(ba::kHashWeaponGone), Op{ OpKind::Detach, 0, 0.0f } });
		CHECK(gone.queued_shots() == 0);
		CHECK_FALSE(gone.firing());
		out.clear();
		gone.destroyed(false, out);
		CHECK(out == std::vector<Op>{ Op{ OpKind::Release, 0, 0.0f } });
	}

	TEST_CASE("R5-VS3 through the router: the local gunner's remote HMG keeps one patch instance across three bursts "
			  "fired while its tail plays") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		WeaponRig rig(*f);
		const uint16_t hmg = rig.def_of(*f, "vehicles/common/weapondata/tank_remote_hmg_firing");
		rig.router->set_own_slot(4);
		rig.router->set_own_first_person(true);
		const int every = std::max(1, int(std::lround(rig.table->defs[hmg].shot_interval_s * double(sim::kSimHz))));
		uint32_t id = 100;
		for (int burst = 0; burst < 3; ++burst) {
			for (int round = 0; round < 5; ++round) {
				const present::PresentEvent e = rig.shot(4, hmg, id++);
				rig.frame(std::span<const present::PresentEvent>(&e, 1));
				for (int i = 1; i < every; ++i) {
					rig.frame();
				}
			}
			for (int i = 0; i < int(sim::kSimHz) / 2; ++i) {
				rig.frame();
			}
		}
		std::printf("remote HMG instance requests over three bursts: %zu\n", rig.requests("hmg_remotegun"));
		CHECK(rig.requests("hmg_remotegun") == 1);
	}

	TEST_CASE("R5-VS3 through the router: P, A and B from the crews, the data's fire logic, one ShotFired per update, "
			  "FireStopped after the queue drains, the release after the tail and a destroyed vehicle's detached "
			  "handle") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		WeaponRig rig(*f);
		const uint16_t cannon = rig.def_of(*f, "vehicles/common/weapondata/mbt_cannon_firing");
		const uint16_t hmg = rig.def_of(*f, "vehicles/common/weapondata/tank_remote_hmg_firing");
		const float cannon_logic = float(uint8_t(f->level->weapons[cannon - 3].fire.logic));
		const float hmg_logic = float(uint8_t(f->level->weapons[hmg - 3].fire.logic));
		std::printf("M1A2 cannon FireLogicType %.0f, remote HMG %.0f\n", double(cannon_logic), double(hmg_logic));
		REQUIRE(cannon_logic == 0.0f);
		REQUIRE(hmg_logic == 2.0f);
		for (const core::gameplay::WeaponDef &w : f->level->weapons) {
			CHECK_MESSAGE(w.fire.logic != core::gameplay::FireLogic::SingleWithBoltAction, w.variant);
			CHECK_MESSAGE(!w.locking.present, w.variant);
			CHECK_MESSAGE(!w.fire.hold_off_reload_until_fire_release, w.variant);
		}
		rig.router->set_trace(true);
		rig.router->set_own_slot(4);
		rig.router->set_own_first_person(false);
		const uint32_t ours = ba::vehicle_crew_key(10, 1), theirs = ba::vehicle_crew_key(11, 1);
		rig.crews = true;
		rig.own_team = 1;
		rig.vehicles = { ours, theirs };
		rig.seats = { { 4, 1, ours }, { 6, 1, ours }, { 9, 2, theirs } };
		rig.frame();
		rig.ops.clear();

		const present::PresentEvent shell = rig.shot(6, cannon, 1);
		rig.frame(std::span<const present::PresentEvent>(&shell, 1));
		const uint64_t fired = rig.ops.empty() ? 0 : rig.ops.back().update;
		const std::vector<Op> first = rig.take(6, cannon);
		CHECK(first ==
				joined({ post_op(ba::kHashFire), post_op(ba::kHashShotFired), post_op(ba::kHashFireStopped) },
						playing_inputs(cannon_logic, false, true, false)));
		CHECK(rig.router->stats().vehicle_instances == 1);
		CHECK(rig.router->stats().vehicle_events == 3);
		int frames = 0;
		std::vector<ba::VehicleWeaponOp> tail;
		while (rig.router->stats().vehicle_releases == 0 && frames < 30 * 30) {
			rig.frame();
			tail.insert(tail.end(), rig.ops.begin(), rig.ops.end());
			rig.ops.clear();
			++frames;
		}
		std::printf("M1A2 cannon shot released after %d updates\n", frames);
		CHECK(rig.router->stats().vehicle_releases == 1);
		const auto update_of = [&](OpKind p_kind, uint32_t p_hash) -> int64_t {
			for (const ba::VehicleWeaponOp &o : tail) {
				if (o.slot == 6 && o.def == cannon && o.op.kind == p_kind && o.op.hash == p_hash) {
					return int64_t(o.update) - int64_t(fired);
				}
			}
			return -1;
		};
		const core::gameplay::FireFunction &cannon_fire = f->level->weapons[cannon - 3].fire;
		const int64_t interval = std::llround(core::gameplay::shot_interval(cannon_fire) * double(sim::kSimHz));
		const int64_t reload = std::llround(
				(double(cannon_fire.reload_delay) + double(cannon_fire.reload_time)) * double(sim::kSimHz));
		std::printf("M1A2 cannon: Reload %lld updates after the shot (interval %lld), ReloadEnd %lld (reload %lld)\n",
				(long long)update_of(OpKind::Post, ba::kHashReload), (long long)interval,
				(long long)update_of(OpKind::Post, ba::kHashReloadEnd), (long long)reload);
		CHECK(update_of(OpKind::Post, ba::kHashReload) == interval);
		CHECK(update_of(OpKind::Set, ba::kHashIsShortReload) == interval);
		CHECK(update_of(OpKind::Post, ba::kHashReloadEnd) == interval + reload);
		const auto short_reload = std::find_if(tail.begin(), tail.end(), [](const ba::VehicleWeaponOp &o) {
			return o.op.kind == OpKind::Set && o.op.hash == ba::kHashIsShortReload;
		});
		CHECK((short_reload != tail.end() && short_reload->op.value == 0.0f));
		CHECK(std::any_of(tail.begin(), tail.end(), [&](const ba::VehicleWeaponOp &o) {
			return o.slot == 6 && o.def == cannon && o.op.kind == OpKind::Release;
		}));

		std::vector<present::PresentEvent> burst;
		for (uint32_t i = 0; i < 3; ++i) {
			burst.push_back(rig.shot(9, hmg, 50 + i));
		}
		rig.frame(burst);
		CHECK(posts(rig.ops_of(9, hmg)) == std::vector<uint32_t>{ ba::kHashFire, ba::kHashShotFired });
		const std::vector<Op> enemy = rig.take(9, hmg);
		CHECK(set_value(enemy, ba::kHashIsFirstPerson) == 0.0f);
		CHECK(set_value(enemy, ba::kHashOwnerSameTeam) == 0.0f);
		CHECK(set_value(enemy, ba::kHashOwnerLocal) == 0.0f);
		CHECK(set_value(enemy, ba::kHashFireLogicType) == hmg_logic);
		CHECK(set_value(enemy, ba::kHashWeaponFlag) == 1.0f);
		rig.frame();
		CHECK(posts(rig.take(9, hmg)) == std::vector<uint32_t>{ ba::kHashShotFired });
		rig.frame();
		CHECK(posts(rig.take(9, hmg)) == std::vector<uint32_t>{ ba::kHashShotFired });
		bool stopped = false;
		for (int i = 0; i < 30 && !stopped; ++i) {
			rig.frame();
			const std::vector<uint32_t> p = posts(rig.take(9, hmg));
			CHECK((p.empty() || p == std::vector<uint32_t>{ ba::kHashFireStopped }));
			stopped = !p.empty();
		}
		CHECK(stopped);
		for (int i = 0; i < 30 * 30 && rig.router->stats().vehicle_releases < 2; ++i) {
			rig.frame();
		}
		CHECK(rig.router->stats().vehicle_releases == 2);

		const present::PresentEvent again = rig.shot(9, hmg, 60);
		rig.frame(std::span<const present::PresentEvent>(&again, 1));
		rig.ops.clear();
		rig.vehicles = { ours };
		rig.seats = { { 4, 1, ours }, { 6, 1, ours } };
		rig.frame();
		CHECK(posts(rig.ops_of(9, hmg)) == std::vector<uint32_t>{ ba::kHashFireStopped, ba::kHashWeaponGone });
		CHECK(has_kind(rig.take(9, hmg), OpKind::Detach));
		CHECK(rig.router->stats().vehicle_detached == 1);

		const ba::WeaponAudioDef &own_hmg = rig.table->defs[hmg];
		for (const bool first_person : { false, true }) {
			rig.router->set_own_first_person(first_person);
			std::vector<present::PresentEvent> own;
			for (uint32_t i = 0; i < 4; ++i) {
				own.push_back(rig.shot(4, hmg, (first_person ? 300 : 200) + i));
			}
			rig.frame(own);
			const std::vector<Op> mine = rig.take(4, hmg);
			CHECK(set_value(mine, ba::kHashIsFirstPerson) == (first_person ? 1.0f : 0.0f));
			CHECK(set_value(mine, ba::kHashOwnerSameTeam) == 1.0f);
			CHECK(set_value(mine, ba::kHashOwnerLocal) == 1.0f);
			const float heat = set_value(mine, ba::kHashOverheatLevel).value_or(-1.0f);
			std::printf("own remote HMG OverheatLevel after 4 rounds (%.4f per round): %.4f\n",
					double(own_hmg.heat.heat_per_bullet), double(heat));
			if (!first_person) {
				CHECK(heat == doctest::Approx(std::min(1.0f, 4.0f * own_hmg.heat.heat_per_bullet)));
			}
			CHECK(heat >= std::min(1.0f, 4.0f * own_hmg.heat.heat_per_bullet) - 1e-6f);
			CHECK(heat <= 1.0f);
			for (int i = 0; i < 40; ++i) {
				rig.frame();
			}
			rig.ops.clear();
		}
	}

	TEST_CASE("R5-VS3 Rule 2 step 12 through the router: a vehicle weapon's handle sits at its weapon's muzzle and "
			  "follows it") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		WeaponRig rig(*f);
		struct Last final : ba::WeaponAudioObserver {
			ba::PatchInstance *instance = nullptr;
			void on_driver_instance(uint8_t, uint64_t, uint64_t, ba::PatchInstance &r_instance) override {
				instance = &r_instance;
			}
			void on_trigger_up(uint8_t, uint64_t, uint64_t) override {}
		} last;
		rig.router->set_observer(&last);
		const uint16_t hmg = rig.def_of(*f, "vehicles/common/weapondata/tank_remote_hmg_firing");
		rig.router->set_own_slot(4);
		const uint32_t ours = ba::vehicle_crew_key(10, 1);
		rig.crews = true;
		rig.own_team = 1;
		rig.vehicles = { ours };
		rig.seats = { { 4, 1, ours } };
		rig.poses = { { ours, hmg, { 30.0f, 103.0f, 40.0f }, { 0.0f, 0.0f, 1.0f } } };
		const present::PresentEvent round = rig.shot(4, hmg, 1);
		rig.frame(std::span<const present::PresentEvent>(&round, 1));
		REQUIRE(last.instance != nullptr);
		CHECK(last.instance->cull_info().position.x == doctest::Approx(30.0f));
		CHECK(last.instance->cull_info().position.z == doctest::Approx(40.0f));
		for (int i = 1; i <= 5; ++i) {
			rig.poses[0].position.x = 30.0f + float(i);
			rig.frame();
		}
		CHECK(last.instance->cull_info().position.x == doctest::Approx(35.0f));
		CHECK(last.instance->cull_info().position.y == doctest::Approx(103.0f));
	}

	TEST_CASE("R5-VS3 Rule 2 step 13 through the router: a destroyed vehicle's detached weapon handle keeps the "
			  "weapon's last transform and gets zero velocity") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		WeaponRig rig(*f);
		struct Last final : ba::WeaponAudioObserver {
			ba::PatchInstance *instance = nullptr;
			void on_driver_instance(uint8_t, uint64_t, uint64_t, ba::PatchInstance &r_instance) override {
				instance = &r_instance;
			}
			void on_trigger_up(uint8_t, uint64_t, uint64_t) override {}
		} last;
		rig.router->set_observer(&last);
		const uint16_t hmg = rig.def_of(*f, "vehicles/common/weapondata/tank_remote_hmg_firing");
		rig.router->set_own_slot(4);
		const uint32_t ours = ba::vehicle_crew_key(10, 1), theirs = ba::vehicle_crew_key(11, 1);
		rig.crews = true;
		rig.own_team = 1;
		rig.vehicles = { ours, theirs };
		rig.seats = { { 4, 1, ours }, { 9, 2, theirs } };
		rig.poses = { { theirs, hmg, { 30.0f, 103.0f, 40.0f }, { 0.0f, 0.0f, 1.0f } } };
		for (uint32_t i = 0; i < 4; ++i) {
			rig.poses[0].position.x = 30.0f + float(i);
			const present::PresentEvent round = rig.shot(9, hmg, 1 + i);
			rig.frame(std::span<const present::PresentEvent>(&round, 1));
		}
		REQUIRE(last.instance != nullptr);
		const float moving = last.instance->cull_info().relative_velocity;
		std::printf(
				"remote HMG on a moving tank: relative velocity %.2f m/s before the tank is gone\n", double(moving));
		CHECK(std::fabs(moving) > 1.0f);
		const uint64_t emitters_before = rig.router->stats().emitter_updates;
		rig.vehicles = { ours };
		rig.seats = { { 4, 1, ours } };
		rig.poses.clear();
		rig.frame();
		rig.frame();
		REQUIRE(rig.router->stats().vehicle_detached == 1);
		const bool finished = std::any_of(rig.cuts.begin(), rig.cuts.end(), [](const ba::MixerCutEvent &e) {
			return e.kind == ba::MixerCutKind::Finish &&
					std::string_view(e.program).find("remotegun") != std::string_view::npos;
		});
		REQUIRE_FALSE(finished);
		const ba::VoiceCullInfo detached = last.instance->cull_info();
		std::printf("detached remote HMG tail: relative velocity %.2f m/s at x %.2f, %llu emitter updates after the "
					"detach\n",
				double(detached.relative_velocity), double(detached.position.x),
				(unsigned long long)(rig.router->stats().emitter_updates - emitters_before));
		CHECK(rig.router->stats().emitter_updates > emitters_before);
		CHECK(detached.relative_velocity == 0.0f);
		CHECK(detached.position.x == doctest::Approx(33.0f));
		CHECK(detached.position.y == doctest::Approx(103.0f));
		CHECK(detached.position.z == doctest::Approx(40.0f));
	}

	TEST_CASE("the own view is first person on foot and in a vehicle only through a camera whose IsFirstPerson is "
			  "set: the M1A2 driver's first-person and chase cameras") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/m1a2/m1abrams");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		host::PresentedVehicle v = parked(*f->level, *spawner);
		v.occupants[0] = 4;
		const host::PresentedVehicle one[1] = { v };
		uint32_t first_person = cg::kNoVehicleCamera, chase = cg::kNoVehicleCamera;
		for (const cg::VehicleCameraDef &c : f->level->kinds[*kind].seat_defs.at(0).cameras) {
			(c.first_person ? first_person : chase) = c.instance;
		}
		REQUIRE(first_person != cg::kNoVehicleCamera);
		REQUIRE(chase != cg::kNoVehicleCamera);
		CHECK(ba::own_view_first_person(one, f->level.get(), 4, first_person));
		CHECK_FALSE(ba::own_view_first_person(one, f->level.get(), 4, chase));
		CHECK_FALSE(ba::own_view_first_person(one, f->level.get(), 4, cg::kNoVehicleCamera));
		CHECK(ba::own_view_first_person(one, f->level.get(), 5, chase));
		CHECK(ba::own_view_first_person({}, f->level.get(), 4, cg::kNoVehicleCamera));
	}

	TEST_CASE("the crews the router gets: each occupant's team from the player rows and each vehicle weapon at its "
			  "muzzle from the presented hull and turret angles") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/m1a2/m1abrams");
		REQUIRE(kind.has_value());
		const std::optional<uint16_t> spawner = spawner_of(*f->level, *kind);
		REQUIRE(spawner.has_value());
		const host::LevelVehicleKind &k = f->level->kinds[*kind];
		REQUIRE(k.armament != nullptr);
		auto frame = std::make_unique<host::PresentationFrame>();
		host::PresentedVehicle v = parked(*f->level, *spawner);
		v.team = 1;
		v.occupants[1] = 5;
		v.turret_count = uint8_t(std::min<size_t>(k.armament->turrets.size(), v.turret_angle.size()));
		frame->vehicle_count = 1;
		frame->vehicles[0] = v;
		frame->player_count = 1;
		frame->players[0].slot = 5;
		frame->players[0].team = 2;
		ba::VehicleCrews crews;
		ba::vehicle_crews(*frame, f->level.get(), 3, crews);
		const uint32_t key = ba::vehicle_crew_key(*spawner, v.generation);
		CHECK(crews.vehicles == std::vector<uint32_t>{ key });
		REQUIRE(crews.seats.size() == 1);
		CHECK(crews.seats[0].slot == 5);
		CHECK(crews.seats[0].team == 2);
		REQUIRE(!crews.poses.empty());
		const size_t w = 0;
		REQUIRE(k.weapon_defs.size() > w);
		const uint16_t def = uint16_t(3 + k.weapon_defs[w]);
		const auto pose_of = [&](const ba::VehicleCrews &p_c) {
			const auto it = std::find_if(p_c.poses.begin(), p_c.poses.end(),
					[&](const ba::VehicleWeaponPose &p) { return p.vehicle == key && p.def == def; });
			REQUIRE(it != p_c.poses.end());
			return *it;
		};
		const core::Transform rot = core::Transform::from_rotation_translation(v.rotation, core::Vec3{});
		const core::Transform hull = sim::vehicles::hull_transform(
				v.rotation, core::vec::sub(v.com_position, rot.transform_vector(k.model->center_of_mass)));
		const core::Vec3 muzzle = sim::vehicles::weapon_muzzle(*k.armament, sim::vehicles::ArmamentState{}, hull, w);
		const ba::VehicleWeaponPose rest = pose_of(crews);
		std::printf("M1A2 weapon %zu (%s) muzzle at %.3f %.3f %.3f, hull centre of mass at %.3f %.3f %.3f\n", w,
				f->level->weapons[k.weapon_defs[w]].variant.c_str(), double(rest.position.x), double(rest.position.y),
				double(rest.position.z), double(v.com_position.x), double(v.com_position.y), double(v.com_position.z));
		CHECK(rest.position.x == doctest::Approx(muzzle.x));
		CHECK(rest.position.y == doctest::Approx(muzzle.y));
		CHECK(rest.position.z == doctest::Approx(muzzle.z));
		frame->vehicles[0].com_position.x += 10.0f;
		ba::vehicle_crews(*frame, f->level.get(), 3, crews);
		CHECK(pose_of(crews).position.x == doctest::Approx(rest.position.x + 10.0f));
		CHECK(pose_of(crews).position.z == doctest::Approx(rest.position.z));
		if (k.armament->weapons[w].frame_turret >= 0) {
			frame->vehicles[0].turret_angle[size_t(k.armament->weapons[w].frame_turret)] = 0.5f;
			ba::vehicle_crews(*frame, f->level.get(), 3, crews);
			CHECK(pose_of(crews).position.z != doctest::Approx(rest.position.z));
		}
	}
}

#include "test_support.h"

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/audio_load.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/vehicle_audio.h"
#include "bf3/audio/voice_cull.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/client_audio/vehicle_sounds.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/gameplay/vehicle_sound.h"
#include "bf3/host/level_load.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/presented_vehicle.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
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

void drive(host::PresentedVehicle &r_v, const sim::VehicleModel &p_model, float p_speed, float p_throttle) {
	r_v.linear_velocity = { 0.0f, 0.0f, p_speed };
	r_v.throttle = p_throttle;
	for (size_t i = 0; i < p_model.wheel_count; ++i) {
		r_v.wheel_spin[i] = p_model.wheels[i].radius > 0.0f ? p_speed / p_model.wheels[i].radius : 0.0f;
	}
}

}

TEST_SUITE("audio-vehicles-install") {
	TEST_CASE("Caspian Conquest Small's vehicles: engine patches, engine positions and the inputs their graphs read") {
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
					e.blueprint, engine.patch.c_str(), double(engine.position.x), double(engine.position.y),
					double(engine.position.z), int(engine.first_person_sounds), engine.surface_patch.c_str(),
					def.turrets.size(), def.turrets.empty() ? "" : def.turrets.front().patch.c_str(),
					def.bullet_impact.c_str(), def.switch_to_alternate_view.c_str(), def.switch_to_normal_view.c_str(),
					inputs.c_str(), double(f->assets.disabled_fraction[*kind]));
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

	TEST_CASE("the Growler's gunner turret: silent turning with nobody in its seat; manned, its traverse sound starts "
			  "while it turns, reads its speed as RPM and stops when it stops") {
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
		CHECK(rig.audio->stats().starts == 0);
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
		CHECK(still_db < -100.0);
		CHECK(turning_db > -70.0);
		CHECK(after_db < -100.0);
		check_unchanged(*f);
	}
}

TEST_SUITE("audio-vehicles-drive") {
	TEST_CASE("the M1A2 at idle heard from its hull centre: the interior camera of its own driver cuts the engine's "
			  "high band") {
		VehicleFixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::optional<uint16_t> kind = kind_of(*f->level, "vehicles/m1a2/m1abrams");
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
		std::printf("m1a2 idle at the hull centre: another driver %.1f dB (4-12 kHz %.1f dB), the own soldier driving "
					"%.1f dB (4-12 kHz %.1f dB)\n",
				exterior_db, exterior_high, interior_db, interior_high);
		CHECK(rig.audio->stats().starts == 1);
		CHECK(rig.audio->stats().interior_frames >= 119);
		CHECK(interior_db < exterior_db - 6.0);
		CHECK(interior_high < exterior_high - 6.0);
		rig.interior_cam = 0.0f;
		for (int i = 0; i < 120; ++i) {
			rig.frame(*f, v, view);
		}
		const double camera_off_high = rig.band(5.0, 6.0, 4000.0f, 12000.0f);
		const double camera_off_db = rig.rms(5.0, 6.0);
		std::printf(
				"m1a2 idle at the hull centre, the own soldier driving, InteriorCam alone set to 0: %.1f dB (4-12 kHz "
				"%.1f dB)\n",
				camera_off_db, camera_off_high);
		CHECK(camera_off_high > interior_high + 6.0);
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

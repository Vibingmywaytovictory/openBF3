#include "bf3/client_audio/game_audio_frame.h"

#include "bf3/audio/audio_load.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/level/level.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>

namespace bf3::audio {

namespace ca = core::audio;
namespace sw = sim::weapons;

namespace {

uint8_t player_team(const host::PresentationFrame &p_frame, uint8_t p_slot, uint8_t p_fallback) {
	const size_t n = std::min<size_t>(p_frame.player_count, p_frame.players.size());
	for (size_t i = 0; i < n; ++i) {
		if (p_frame.players[i].slot == p_slot) {
			return p_frame.players[i].team;
		}
	}
	return p_fallback;
}

void vehicle_weapon_poses(const host::PresentedVehicle &p_vehicle, const host::LevelVehicles &p_level,
		uint16_t p_weapon_base, uint32_t p_key, std::vector<VehicleWeaponPose> &r_out) {
	if (p_vehicle.spawner >= p_level.spawners.size()) {
		return;
	}
	const uint16_t kind = p_level.spawners[p_vehicle.spawner].kind;
	if (kind >= p_level.kinds.size() || !p_level.kinds[kind].model || !p_level.kinds[kind].armament) {
		return;
	}
	const host::LevelVehicleKind &k = p_level.kinds[kind];
	const sim::vehicles::VehicleArmament &a = *k.armament;
	const core::Transform rot = core::Transform::from_rotation_translation(p_vehicle.rotation, core::Vec3{});
	const core::Transform hull = sim::vehicles::hull_transform(
			p_vehicle.rotation, core::vec::sub(p_vehicle.com_position, rot.transform_vector(k.model->center_of_mass)));
	sim::vehicles::ArmamentState state;
	for (size_t t = 0; t < p_vehicle.turret_count && t < state.bodies.size(); ++t) {
		state.bodies[t].angle = p_vehicle.turret_angle[t];
	}
	for (size_t w = 0; w < a.weapons.size() && w < k.weapon_defs.size(); ++w) {
		if (k.weapon_defs[w] == host::kNoVehicleWeapon) {
			continue;
		}
		const uint16_t def = uint16_t(p_weapon_base + k.weapon_defs[w]);
		if (std::any_of(r_out.begin(), r_out.end(),
					[&](const VehicleWeaponPose &p) { return p.vehicle == p_key && p.def == def; })) {
			continue;
		}
		const core::Transform at = sim::vehicles::weapon_world(a, state, hull, w);
		r_out.push_back(VehicleWeaponPose{ p_key, def, at.transform_point(a.weapons[w].spawn_offset), at.forward });
	}
}

}

void vehicle_crews(const host::PresentationFrame &p_frame, const host::LevelVehicles *p_level, uint16_t p_weapon_base,
		VehicleCrews &r_out) {
	r_out.vehicles.clear();
	r_out.seats.clear();
	r_out.poses.clear();
	const size_t n = std::min<size_t>(p_frame.vehicle_count, p_frame.vehicles.size());
	for (size_t i = 0; i < n; ++i) {
		const host::PresentedVehicle &v = p_frame.vehicles[i];
		if (v.exploded) {
			continue;
		}
		const uint32_t key = vehicle_crew_key(v.spawner, v.generation);
		r_out.vehicles.push_back(key);
		for (size_t s = 0; s < v.seat_count && s < v.occupants.size(); ++s) {
			if (v.occupants[s] != net::kNoVehicleOccupant) {
				r_out.seats.push_back(
						VehicleCrewSeat{ v.occupants[s], player_team(p_frame, v.occupants[s], v.team), key });
			}
		}
		if (p_level != nullptr) {
			vehicle_weapon_poses(v, *p_level, p_weapon_base, key, r_out.poses);
		}
	}
}

void GameAudioDriver::step(WeaponAudioRouter &r_router, ImpactAudio *r_impacts, SimMixerClock &r_clock,
		const GameAudioInputs &p_in, std::span<const ImpactSoundRequest> p_impacts,
		std::span<const EffectSoundRequest> p_effects, FlyByAudio *r_flybys) {
	r_router.set_own_slot(p_in.own_slot);
	r_router.set_own_first_person(p_in.own_first_person);
	if (p_in.frame != nullptr && p_in.frame->own_weapons_valid) {
		const net::WeaponTimersWire &active = p_in.frame->own_weapons_curr.timers[0];
		// WHY: for the local player's weapon the fire end, not the trigger release, ends a burst once it has fired (notes
		// R5-VS3 1), so the trigger counts as held while rounds of the burst are left.
		r_router.set_own_trigger((active.flags & sw::slot_flags::kTriggerDown) != 0 || active.burst_left > 0);
	}
	r_router.set_listener(p_in.listener);
	if (r_impacts != nullptr) {
		r_impacts->set_listener(p_in.listener);
	}
	if (p_in.frame != nullptr && p_in.frame->valid) {
		const size_t n = host::present_other_weapons(*p_in.frame, double(p_in.frame->newest_tick), newest_);
		r_router.set_others_newest(std::span<const host::PresentedOtherWeapon>(newest_.data(), n),
				sw::SimTime(p_in.frame->newest_tick + 1) * sw::kTimeUnitsPerTick);
	}
	if (p_in.frame != nullptr) {
		for (const net::SoldierGear &g : p_in.frame->gear) {
			r_router.set_slot_sound(g.slot, g.def, accessory_sound_source(p_in.sound_variants, g));
		}
	}
	r_router.set_own_weapons(own_weapons_.present(p_in.frame));
	if (p_in.frame != nullptr) {
		vehicle_crews(*p_in.frame, p_in.vehicles, p_in.vehicle_weapon_base, crews_);
		r_router.set_vehicle_crews(p_in.frame->own_team, crews_.vehicles, crews_.seats, crews_.poses);
	}
	r_router.on_events(p_in.events, r_clock);
	r_router.on_others(p_in.others, p_in.other_weapons, p_in.muzzles, r_clock);
	if (r_impacts != nullptr) {
		for (const ImpactSoundRequest &r : p_impacts) {
			r_impacts->play_impact(r, r_clock);
		}
		for (const EffectSoundRequest &e : p_effects) {
			r_impacts->on_effect_sound(e, r_clock);
		}
	}
	if (r_flybys != nullptr) {
		r_flybys->set_listener(p_in.listener);
		r_flybys->on_events(p_in.events);
		if (p_in.frame != nullptr && p_in.frame->valid) {
			const size_t n = std::min<size_t>(p_in.frame->bullet_count, p_in.frame->bullets.size());
			r_flybys->on_bullets(std::span<const host::PresentedBullet>(p_in.frame->bullets.data(), n),
					p_in.frame->match_seed, r_clock);
		}
	}
	r_router.update(p_in.now_ns);
}

core::Result<std::shared_ptr<const ca::SoundProgram>> compile_patch_lenient(const core::EbxResolver &p_db, std::string_view p_patch) {
	core::Result<std::shared_ptr<const ca::SoundProgram>> strict = compile_patch(p_db, p_patch);
	if (strict) {
		return strict;
	}
	// STAND-IN: a patch with node types that are not built plays without them. Settle: build those node types.
	ca::CompileOptions lenient;
	lenient.allow_unimplemented = true;
	return compile_patch(p_db, p_patch, lenient);
}

namespace {

core::Result<GameAuditionAsset> load_library_audition(const core::Install &p_install, const core::EbxLibrary &p_lib, std::string_view p_patch,
		unsigned p_threads, size_t *r_missing) {
	core::Result<std::shared_ptr<const ca::SoundProgram>> p = compile_patch_lenient(p_lib, p_patch);
	if (!p) {
		return p.error();
	}
	const std::optional<core::EbxLibrary::Origin> origin = p_lib.origin(p_patch);
	size_t sb = p_lib.superbundle_count();
	for (size_t i = 0; origin && i < p_lib.superbundle_count(); ++i) {
		if (p_lib.superbundle(i) == origin->superbundle) {
			sb = i;
		}
	}
	if (sb >= p_lib.superbundle_count()) {
		return core::make_error(std::string(p_patch) + ": compiled, but its superbundle is unknown");
	}
	size_t missing = 0;
	core::Result<std::vector<ca::SoundWaveInfo>> waves = read_program_waves(p_lib, std::span<const std::shared_ptr<const ca::SoundProgram>>(&*p, 1), &missing);
	if (!waves) {
		return waves.error();
	}
	if (r_missing != nullptr) {
		*r_missing += missing;
	}
	ca::WaveBankOptions bo;
	bo.threads = p_threads;
	core::Result<std::shared_ptr<const ca::WaveBank>> bank = ca::WaveBank::load(p_install, p_lib.mount(sb), std::move(*waves), bo);
	if (!bank) {
		return bank.error();
	}
	GameAuditionAsset a;
	a.program = *p;
	a.bank = *bank;
	a.source = origin->superbundle;
	return a;
}

}

std::vector<FlyBySound> load_flyby_sounds(
		const core::EbxResolver &p_db, const host::GunplayLevel &p_gunplay, std::vector<std::string> &r_report) {
	std::vector<FlyBySound> out(p_gunplay.defs.size());
	std::map<std::string, std::shared_ptr<const ca::SoundProgram>, std::less<>> compiled;
	size_t defs = 0, failed = 0;
	for (size_t i = 0; i < p_gunplay.defs.size(); ++i) {
		const core::gameplay::ProjectileDef &p = p_gunplay.defs[i].projectile;
		if (p.fly_by_sound.empty() || !(p.fly_by_radius > 0.0f)) {
			continue;
		}
		auto it = compiled.find(p.fly_by_sound);
		if (it == compiled.end()) {
			core::Result<std::shared_ptr<const ca::SoundProgram>> prog = compile_patch_lenient(p_db, p.fly_by_sound);
			if (!prog) {
				++failed;
				r_report.push_back("fly-by " + p.fly_by_sound + ": NOT LOADED (" + prog.error().message + ")");
			}
			it = compiled.emplace(p.fly_by_sound, prog ? *prog : nullptr).first;
		}
		if (it->second) {
			out[i] = FlyBySound{ it->second, p.fly_by_radius, p.fly_by_speed };
			++defs;
		}
	}
	r_report.push_back("fly-bys: " + std::to_string(defs) + " defs, " + std::to_string(compiled.size() - failed) +
			" patches, " + std::to_string(failed) + " failures");
	return out;
}

core::Result<GameAudioAssets> load_game_audio_assets(const std::shared_ptr<const core::Install> &p_install, const core::level::LoadedLevel &p_level,
		const host::GunplayLevel &p_gunplay, std::span<const std::string> p_impact_sounds, unsigned p_threads, const std::atomic<bool> *p_stop,
		const GameAudioLoadOptions &p_options) {
	if (!p_install) {
		return core::make_error("load_game_audio_assets: no install");
	}
	const core::EbxDb &db = p_level.db();
	const auto stopped = [&]() { return p_stop != nullptr && p_stop->load(std::memory_order_relaxed); };
	GameAudioAssets a;

	a.kits = kit_audio_sources(p_gunplay);
	if (p_options.vehicles != nullptr) {
		add_vehicle_weapon_sounds(a.kits, p_options.vehicles->weapons);
	}
	for (const std::string &note : add_accessory_sounds(a.kits, db, p_gunplay)) {
		a.report.push_back("accessory sounds " + note);
	}
	core::Result<WeaponAudioTable> table = load_weapon_audio(db, a.kits.sources, kMixRate);
	if (!table) {
		return table.error();
	}
	auto shared_table = std::make_shared<WeaponAudioTable>(std::move(*table));
	std::vector<std::string> kit_report = kit_audio_report(a.kits, *shared_table);
	a.report.insert(a.report.end(), kit_report.begin(), kit_report.end());
	a.programs += shared_table->compiled;
	a.program_failures += shared_table->failed;
	a.table = shared_table;
	if (stopped()) {
		return core::make_error("load_game_audio_assets: stopped");
	}

	core::Result<std::shared_ptr<const ImpactSoundCache>> cache = ImpactSoundCache::build(db, p_impact_sounds);
	if (!cache) {
		return cache.error();
	}
	a.impacts = *cache;
	a.programs += uint32_t((*cache)->compiled());
	a.program_failures += uint32_t((*cache)->failed());
	for (const std::string &e : (*cache)->errors()) {
		a.report.push_back("impact " + e);
	}
	a.flybys = load_flyby_sounds(db, p_gunplay, a.report);
	if (p_gunplay.materials) {
		core::Result<SoldierAudioAssets> soldier = load_soldier_audio(db, *p_gunplay.materials, a.report);
		if (soldier) {
			a.soldier = std::move(*soldier);
			a.programs += 2;
		} else {
			++a.program_failures;
			a.report.push_back("footsteps: NOT LOADED (" + soldier.error().message + ")");
		}
	}
	if (p_options.vehicles) {
		core::Result<VehicleAudioAssets> vehicles = load_vehicle_audio(db, *p_options.vehicles, a.report);
		if (vehicles) {
			a.programs += uint32_t(vehicles->programs.size());
			a.vehicles = std::move(*vehicles);
		} else {
			++a.program_failures;
			a.report.push_back("vehicle sounds: NOT LOADED (" + vehicles.error().message + ")");
		}
	}

	std::vector<std::string> from_library;
	for (const GameAuditionItem &item : game_audition_items()) {
		core::Result<std::shared_ptr<const ca::SoundProgram>> p = compile_patch_lenient(db, item.patch);
		if (p) {
			a.auditions.emplace(item.patch, GameAuditionAsset{ *p, nullptr, "level" });
			a.report.push_back("audition " + item.name + " [" + item.patch + "]: compiled from the level");
		} else if (!p_options.audition_superbundles.empty()) {
			from_library.push_back(item.patch);
		} else {
			a.audition_failures.push_back(item.patch);
			a.report.push_back("audition " + item.name + " [" + item.patch + "]: NOT LOADED (" + p.error().message + "; no audition superbundles)");
		}
	}
	if (!from_library.empty()) {
		const auto t0 = std::chrono::steady_clock::now();
		const std::vector<std::string> sbs = core::expand_superbundles(*p_install, p_options.audition_superbundles);
		core::Result<std::unique_ptr<core::EbxLibrary>> lib = core::EbxLibrary::open(p_install, sbs, p_threads);
		for (const std::string &patch : from_library) {
			core::Result<GameAuditionAsset> r = lib ? load_library_audition(*p_install, **lib, patch, p_threads, &a.audition_waves_missing)
													: core::Result<GameAuditionAsset>(lib.error());
			if (r) {
				char waves_line[96];
				std::snprintf(waves_line, sizeof(waves_line), ", %zu resident waves of its own (%.1f MiB)", r->bank->stats().resident_waves,
						double(r->bank->stats().resident_bytes) / double(1 << 20));
				a.report.push_back("audition [" + patch + "]: compiled from " + r->source + waves_line);
				a.auditions.emplace(patch, std::move(*r));
			} else {
				a.audition_failures.push_back(patch);
				a.report.push_back("audition [" + patch + "]: NOT LOADED (not on the level's mount; " + p_options.audition_superbundles + ": " + r.error().message + ")");
			}
			if (stopped()) {
				return core::make_error("load_game_audio_assets: stopped");
			}
		}
		char line[160];
		std::snprintf(line, sizeof(line), "auditions from %s: %zu patches in %.3f s (loader thread)", p_options.audition_superbundles.c_str(),
				from_library.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
		a.report.push_back(line);
	}
	if (stopped()) {
		return core::make_error("load_game_audio_assets: stopped");
	}

	std::vector<std::shared_ptr<const ca::SoundProgram>> all;
	for (const WeaponAudioDef &d : a.table->defs) {
		if (d.program) {
			all.push_back(d.program);
		}
		if (d.explosion) {
			all.push_back(d.explosion);
		}
	}
	for (const std::shared_ptr<const ca::SoundProgram> &p : a.impacts->programs()) {
		all.push_back(p);
	}
	for (const FlyBySound &s : a.flybys) {
		if (s.program && std::find(all.begin(), all.end(), s.program) == all.end()) {
			all.push_back(s.program);
		}
	}
	if (a.soldier) {
		all.push_back(a.soldier->footsteps);
		all.push_back(a.soldier->own_hit);
		all.push_back(a.soldier->movement);
	}
	if (a.vehicles) {
		all.insert(all.end(), a.vehicles->programs.begin(), a.vehicles->programs.end());
	}
	for (const auto &[name, au] : a.auditions) {
		if (!au.bank) {
			all.push_back(au.program);
		}
	}
	core::Result<std::vector<ca::SoundWaveInfo>> waves = read_program_waves(db, all, &a.waves_missing);
	if (!waves) {
		return waves.error();
	}
	ca::WaveBankOptions bo;
	bo.threads = p_threads;
	core::Result<std::shared_ptr<const ca::WaveBank>> bank = ca::WaveBank::load(*p_install, p_level.mount(), std::move(*waves), bo);
	if (!bank) {
		return bank.error();
	}
	a.bank = *bank;
	for (auto &[name, au] : a.auditions) {
		if (!au.bank) {
			au.bank = a.bank;
		}
	}
	char line[160];
	std::snprintf(line, sizeof(line), "bank: %zu programs, %u resident waves, %zu waves not in the level", all.size(),
			unsigned(a.bank->stats().resident_waves), a.waves_missing);
	a.report.push_back(line);
	return a;
}

bool start_audition(OneShotPool &r_pool, MixerCore &r_mixer, std::shared_ptr<const ca::SoundProgram> p_program, std::shared_ptr<const ca::WaveBank> p_bank,
		const AudioTuning &p_tuning, const ListenerState &p_listener, const Vec3 &p_position, uint64_t p_seed, uint64_t p_frame, GameAudioCheck *p_check) {
	if (!p_program || !std::isfinite(p_position.x) || !std::isfinite(p_position.y) || !std::isfinite(p_position.z)) {
		return false;
	}
	EmitterState em;
	em.position = p_position;
	const Vec3 d{ p_listener.position.x - em.position.x, p_listener.position.y - em.position.y, p_listener.position.z - em.position.z };
	const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (len > 0.0f && std::isfinite(len)) {
		em.forward = { d.x / len, d.y / len, d.z / len };
	}
	OneShotOptions o;
	o.set_first_person = true;
	o.first_person = false;
	if (p_check != nullptr && p_check->current_step() >= 0) {
		const uint32_t step = uint32_t(p_check->current_step());
		o.before_start = [p_check, step, p_seed, p_frame](PatchInstance &r_instance, InstanceHandle) { p_check->on_audition(step, p_seed, p_frame, r_instance); };
	}
	return r_pool.start(r_mixer, std::move(p_program), std::move(p_bank), p_tuning, p_seed, em, p_listener, p_frame, o);
}

}

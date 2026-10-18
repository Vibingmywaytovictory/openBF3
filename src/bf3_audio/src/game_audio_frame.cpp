// One display frame of the game's audio (game_audio_frame.h).

#include "bf3/audio/game_audio_frame.h"

#include "bf3/audio/audio_load.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/level/level.h"
#include "bf3/host/gunplay.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>

namespace bf3::audio {

namespace ca = core::audio;
namespace sw = sim::weapons;

void GameAudioDriver::step(WeaponAudioRouter &r_router, ImpactAudio *r_impacts, SimMixerClock &r_clock, const GameAudioInputs &p_in,
		std::span<const ImpactSoundRequest> p_impacts, std::span<const EffectSoundRequest> p_effects) {
	r_router.set_own_slot(p_in.own_slot);
	r_router.set_own_first_person(p_in.own_first_person);
	if (p_in.frame != nullptr && p_in.frame->own_weapons_valid) {
		r_router.set_own_trigger((p_in.frame->own_weapons_curr.timers[0].flags & sw::slot_flags::kTriggerDown) != 0);
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
	r_router.update(p_in.now_ns);
}

core::Result<std::shared_ptr<const ca::SoundProgram>> compile_patch_lenient(const core::EbxResolver &p_db, std::string_view p_patch) {
	core::Result<std::shared_ptr<const ca::SoundProgram>> strict = compile_patch(p_db, p_patch);
	if (strict) {
		return strict;
	}
	ca::CompileOptions lenient;
	lenient.allow_unimplemented = true;
	return compile_patch(p_db, p_patch, lenient);
}

namespace {

// E7 item 4 from GameAudioLoadOptions::audition_superbundles: the program (compiled from a lazily mounted library of
// those superbundles) and a bank over its waves from the mount of the superbundle that holds the patch.
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

} // namespace

core::Result<GameAudioAssets> load_game_audio_assets(const std::shared_ptr<const core::Install> &p_install, const core::level::LoadedLevel &p_level,
		const host::GunplayLevel &p_gunplay, std::span<const std::string> p_impact_sounds, unsigned p_threads, const std::atomic<bool> *p_stop,
		const GameAudioLoadOptions &p_options) {
	if (!p_install) {
		return core::make_error("load_game_audio_assets: no install");
	}
	const core::EbxDb &db = p_level.db();
	const auto stopped = [&]() { return p_stop != nullptr && p_stop->load(std::memory_order_relaxed); };
	GameAudioAssets a;
	// E7 item 1: the kit items' patches, strict, and their binding.
	a.kits = kit_audio_sources(p_gunplay);
	core::Result<WeaponAudioTable> table = load_weapon_audio(db, a.kits.sources, kMixRate);
	if (!table) {
		return table.error();
	}
	auto shared_table = std::make_shared<WeaponAudioTable>(std::move(*table));
	a.report = kit_audio_report(a.kits, *shared_table);
	a.programs += shared_table->compiled;
	a.program_failures += shared_table->failed;
	a.table = shared_table;
	if (stopped()) {
		return core::make_error("load_game_audio_assets: stopped");
	}
	// Impact and effect sounds (fx3's names).
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
	// E7 item 4: the audition patches from the level's mount, else from the audition superbundles (their own banks).
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
	// One bank over every program's waves.
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
	for (const auto &[name, au] : a.auditions) {
		if (!au.bank) {
			all.push_back(au.program); // from the level: its waves go in the level bank
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
		em.forward = { d.x / len, d.y / len, d.z / len }; // facing the listener (on the cone axis)
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

} // namespace bf3::audio

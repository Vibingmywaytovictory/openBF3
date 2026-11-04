#include "bf3/client_audio/soldier_audio.h"

#include "bf3/audio/audio_load.h"
#include "bf3/core/util/finite.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace ca = core::audio;

namespace {

constexpr float kGroundProbeUpM = 0.5f;
constexpr float kGroundProbeDropM = 1.5f;

uint64_t hit_key(const host::PresentEvent &p_e) {
	return (uint64_t(p_e.slot) << 32) | uint64_t(p_e.id);
}

}

core::Result<SoldierAudioAssets> load_soldier_audio(const core::EbxResolver &p_db,
		const sim::weapons::SurfaceMaterialTable &p_surfaces, std::vector<std::string> &r_report) {
	if (!p_surfaces.has_grid()) {
		return core::make_error("soldier audio: the surface table has no material grid");
	}
	core::Result<core::gameplay::SoldierSoundDef> def = core::gameplay::read_soldier_sounds(p_db);
	if (!def) {
		return def.error();
	}
	core::Result<core::gameplay::MaterialSounds> materials =
			core::gameplay::read_material_sounds(p_db, p_surfaces.grid().report().grid);
	if (!materials) {
		return materials.error();
	}
	SoldierAudioAssets a;
	a.def = std::move(*def);
	a.materials = std::move(*materials);
	core::Result<std::shared_ptr<const ca::SoundProgram>> steps = compile_patch(p_db, a.def.footsteps);
	if (!steps) {
		return steps.error();
	}
	a.footsteps = *steps;
	a.params = footstep_params(a.def, *a.footsteps);
	if (!(a.params.radius_m > 0.0f)) {
		return core::make_error(a.def.footsteps + ": no attenuation curve gives an audible radius");
	}
	core::Result<std::shared_ptr<const ca::SoundProgram>> hit = compile_patch(p_db, a.def.bullet_impact);
	if (!hit) {
		return hit.error();
	}
	a.own_hit = *hit;
	r_report.push_back("footsteps [" + a.def.footsteps + "]: " + std::to_string(a.materials.rows_with_sound) + " of " +
			std::to_string(a.materials.rows) + " grid rows with a MaterialSoundId, audible to " +
			std::to_string(a.params.radius_m) + " m; own hit [" + a.def.bullet_impact + "]");
	return a;
}

core::Result<std::shared_ptr<FootstepGroundProbe>> FootstepGroundProbe::create(
		std::shared_ptr<const sim::StaticWorldShapes> p_shapes,
		std::shared_ptr<const sim::weapons::SurfaceMaterialTable> p_surfaces,
		core::gameplay::MaterialSounds p_materials) {
	if (!p_shapes || !p_surfaces) {
		return core::make_error("FootstepGroundProbe: no static world or surface table");
	}
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(std::move(p_shapes));
	if (!world) {
		return world.error();
	}
	auto out = std::make_shared<FootstepGroundProbe>();
	out->world_ = std::move(*world);
	out->surfaces_ = std::move(p_surfaces);
	out->materials_ = std::move(p_materials);
	return out;
}

std::optional<float> FootstepGroundProbe::sound_id(const Vec3 &p_feet) const {
	if (!core::finite(p_feet)) {
		return std::nullopt;
	}
	sim::StaticHit hit;
	if (!world_->ground_below(sim::Vec3{ p_feet.x, p_feet.y + kGroundProbeUpM, p_feet.z },
				kGroundProbeUpM + kGroundProbeDropM, sim::query_mask::kProjectile, &hit)) {
		return std::nullopt;
	}
	const sim::weapons::SurfacePair pair = surfaces_->static_pair(hit.tag.material);
	if (!pair.resolved) {
		return std::nullopt;
	}
	return materials_.sound_id(pair.physics_material);
}

void footstep_soldiers(const host::PresentationFrame *p_frame, std::span<const host::PresentedSoldier> p_others,
		uint8_t p_own_slot, bool p_own_first_person, std::vector<FootstepSoldier> &r_out) {
	r_out.clear();
	if (p_frame != nullptr && p_frame->valid && p_own_slot != 0xFF &&
			(p_frame->own_curr.flags & sim::soldier_flags::kAlive) != 0) {
		const sim::SoldierState &o = p_frame->own_curr;
		FootstepSoldier s;
		s.slot = p_own_slot;
		s.generation = uint8_t(o.life);
		s.position = Vec3{ o.position[0], o.position[1], o.position[2] };
		s.velocity = Vec3{ o.velocity[0], o.velocity[1], o.velocity[2] };
		s.pose = uint8_t(o.pose);
		s.mode = o.mode;
		s.first_person = p_own_first_person;
		r_out.push_back(s);
	}
	for (const host::PresentedSoldier &p : p_others) {
		if (p.slot == p_own_slot) {
			continue;
		}
		FootstepSoldier s;
		s.slot = p.slot;
		s.generation = p.generation;
		s.position = Vec3{ p.position[0], p.position[1], p.position[2] };
		s.velocity = Vec3{ p.velocity[0], p.velocity[1], p.velocity[2] };
		s.pose = uint8_t(p.pose);
		s.mode = p.mode;
		r_out.push_back(s);
	}
}

OwnHitAudio::OwnHitAudio(MixerCore &r_mixer, std::shared_ptr<const ca::SoundProgram> p_program,
		std::shared_ptr<const ca::WaveBank> p_bank, const AudioTuning &p_tuning, OneShotPool &r_pool) :
		mixer_(r_mixer), program_(std::move(p_program)), bank_(std::move(p_bank)), tuning_(p_tuning), pool_(r_pool) {}

void OwnHitAudio::on_events(
		std::span<const host::PresentEvent> p_events, const ListenerState &p_listener, uint64_t p_frame) {
	heard_.clear();
	for (const host::PresentEvent &e : p_events) {
		if (e.kind != host::PresentKind::Hit || (e.flags & host::present_flags::kOwnVictim) == 0) {
			continue;
		}
		++stats_.hits;
		const uint64_t key = hit_key(e);
		if (std::find(heard_.begin(), heard_.end(), key) != heard_.end() || !program_) {
			continue;
		}
		heard_.push_back(key);
		EmitterState em;
		em.position = p_listener.position;
		const Vec3 dir{ e.dir[0], e.dir[1], e.dir[2] };
		if (core::finite(dir) && (dir.x != 0.0f || dir.y != 0.0f || dir.z != 0.0f)) {
			em.forward = dir;
		}
		OneShotOptions o;
		o.set_first_person = true;
		o.first_person = true;
		if (!pool_.start(mixer_, program_, bank_, tuning_, e.seed ^ key, em, p_listener, p_frame, o)) {
			++stats_.refused;
			continue;
		}
		++stats_.starts;
	}
}

}

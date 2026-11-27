#include "bf3/audio/vehicle_weapon_audio.h"

#include "bf3/core/math.h"
#include "bf3/core/util/finite.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace ca = core::audio;
namespace sw = sim::weapons;

namespace {

bool declares(const ca::SoundProgram &p_program, uint32_t p_hash, bool p_event) {
	return std::any_of(p_program.inputs.begin(), p_program.inputs.end(),
			[&](const ca::SoundInput &in) { return in.name_hash == p_hash && in.event == p_event; });
}

bool single_fire(uint8_t p_fire_logic) {
	return p_fire_logic == 0 || p_fire_logic == 1;
}

sw::SimTime shot_units(const WeaponAudioDef &p_def) {
	return sw::SimTime(std::llround(p_def.shot_interval_s * double(sw::kTimeUnitsPerSecond)));
}

bool same_place(const EmitterState &p_a, const EmitterState &p_b) {
	return p_a.position.x == p_b.position.x && p_a.position.y == p_b.position.y && p_a.position.z == p_b.position.z &&
			p_a.forward.x == p_b.forward.x && p_a.forward.y == p_b.forward.y && p_a.forward.z == p_b.forward.z;
}

}

VehicleWeaponAudio::VehicleWeaponAudio(MixerCore &r_mixer, std::shared_ptr<const WeaponAudioTable> p_table,
		std::shared_ptr<const ca::WaveBank> p_bank, const AudioTuning &p_tuning, uint32_t p_mix_rate,
		WeaponAudioStats &r_stats) :
		mixer_(r_mixer), table_(std::move(p_table)), bank_(std::move(p_bank)), tuning_(p_tuning),
		mix_rate_(p_mix_rate != 0 ? p_mix_rate : kMixRate), stats_(r_stats) {}

VehicleWeaponAudio::~VehicleWeaponAudio() {
	for (const Weapon &w : weapons_) {
		if (w.handle != kNoInstance) {
			(void)push(MixerCommandType::Kill, w.handle, 0, 0.0f);
		}
	}
	for (const Detached &d : detached_) {
		(void)push(MixerCommandType::Kill, d.handle, 0, 0.0f);
	}
}

void VehicleWeaponAudio::set_crews(uint8_t p_own_team, std::span<const uint32_t> p_vehicles,
		std::span<const VehicleCrewSeat> p_seats, std::span<const VehicleWeaponPose> p_poses) {
	crews_known_ = true;
	own_team_ = p_own_team;
	vehicles_.assign(p_vehicles.begin(), p_vehicles.end());
	seats_.assign(p_seats.begin(), p_seats.end());
	poses_.assign(p_poses.begin(), p_poses.end());
	for (Weapon &w : weapons_) {
		if (w.crewed && std::find(vehicles_.begin(), vehicles_.end(), w.vehicle) == vehicles_.end()) {
			w.gone = true;
		}
	}
}

VehicleWeaponAudio::Weapon *VehicleWeaponAudio::weapon_of(uint64_t p_key) {
	for (Weapon &w : weapons_) {
		if (w.key == p_key && !w.gone) {
			return &w;
		}
	}
	return nullptr;
}

void VehicleWeaponAudio::on_shot(const present::PresentEvent &p_event, uint8_t p_own_slot, bool p_own_first_person) {
	const WeaponAudioDef *d =
			table_ != nullptr && p_event.def < table_->defs.size() ? &table_->defs[p_event.def] : nullptr;
	if (d == nullptr || !d->program) {
		++stats_.unbound_defs;
		return;
	}
	const VehicleCrewSeat *seat = nullptr;
	if (crews_known_) {
		for (const VehicleCrewSeat &s : seats_) {
			if (s.slot == p_event.slot) {
				seat = &s;
				break;
			}
		}
	}
	const uint64_t key = seat != nullptr ? (uint64_t(seat->vehicle) << 16) | p_event.def
										 : (uint64_t(1) << 63) | (uint64_t(p_event.slot) << 16) | p_event.def;
	Weapon *w = weapon_of(key);
	if (w == nullptr) {
		weapons_.emplace_back();
		w = &weapons_.back();
		w->key = key;
		w->crewed = seat != nullptr;
		w->vehicle = seat != nullptr ? seat->vehicle : 0;
		w->def = p_event.def;
		w->sound = d;
		// INFERRED: the handle takes the def's FireLogicType when it is made, where notes R5-VS3 1 start at 2 and change
		// it on a fire-mode action, which clients do not see. Settle: a note on remote fire-mode actions.
		if (d->fire_logic <= 5) {
			w->state.set_fire_logic(d->fire_logic);
		}
		w->magazine = d->magazine;
	}
	w->slot = p_event.slot;
	// SOURCE: notes R5-VS3 1 (IsFirstPerson for the local player's weapon seen in first person, #3576779520 the same
	// team, #4198762218 the local player's weapon).
	WeaponSoundOwner owner;
	owner.local = p_event.slot == p_own_slot;
	owner.first_person = owner.local && p_own_first_person;
	owner.same_team = owner.local || (seat != nullptr && seat->team == own_team_);
	w->due.push_back(Due{ p_event.time, owner });
	if (!w->pose_known && core::finite(p_event.pos)) {
		EmitterState e = w->emitter;
		e.position = { p_event.pos[0], p_event.pos[1], p_event.pos[2] };
		if (core::finite(p_event.dir)) {
			e.forward = { p_event.dir[0], p_event.dir[1], p_event.dir[2] };
		}
		w->emitter_dirty = w->emitter_dirty || !same_place(e, w->emitter);
		w->emitter = e;
	}
	++stats_.vehicle_shots;
}

void VehicleWeaponAudio::slot_gone(uint8_t p_slot) {
	for (Weapon &w : weapons_) {
		if (!w.crewed && w.slot == p_slot) {
			w.gone = true;
		}
	}
}

void VehicleWeaponAudio::heat_to(Weapon &r_w, sw::SimTime p_time) {
	if (!r_w.heat_known) {
		r_w.heat_known = true;
		r_w.heat_time = p_time;
		return;
	}
	if (p_time > r_w.heat_time) {
		sim::vehicles::overheat_tick(
				r_w.heat, r_w.sound->heat, float(double(p_time - r_w.heat_time) / double(sw::kTimeUnitsPerSecond)));
		r_w.heat_time = p_time;
	}
}

// INFERRED: clients see only shots, so a single-fire shot is a fire start, a shot and a fire end, OverheatLevel is the
// server's overheat rule run on the shots seen, and the empty shot schedules the reload. Settle: a note on remote fire.
void VehicleWeaponAudio::fire(Weapon &r_w, const Due &p_due) {
	heat_to(r_w, p_due.time);
	(void)sim::vehicles::overheat_shot(r_w.heat, r_w.sound->heat);
	r_w.last_owner = p_due.owner;
	if (single_fire(r_w.sound->fire_logic)) {
		r_w.state.fire_start(p_due.owner);
		r_w.state.shot();
		r_w.state.fire_end();
	} else {
		if (!r_w.pulling) {
			r_w.state.fire_start(p_due.owner);
			r_w.pulling = true;
		}
		r_w.state.shot();
	}
	r_w.last_shot = std::max(r_w.last_shot, p_due.time);
	if (r_w.sound->magazine <= 0) {
		return;
	}
	if (r_w.reloading || r_w.reload_pending) {
		r_w.reloading = r_w.reload_pending = false;
		r_w.magazine = r_w.sound->magazine;
	}
	r_w.magazine = std::max(0, r_w.magazine - 1);
	if (r_w.magazine == 0 && r_w.sound->auto_reload && r_w.sound->reload_s > 0.0) {
		r_w.reload_pending = true;
		r_w.reload_at = p_due.time + shot_units(*r_w.sound);
	}
}

// SOURCE: notes R-A3 F (AlwaysAutoReload at magazine 0; an empty magazine waits ReloadDelay, then the reload time).
// INFERRED: Reload a shot interval after the emptying shot and ReloadEnd reload_s later. Settle: a note on the timing.
void VehicleWeaponAudio::reload(Weapon &r_w, const present::PresentClock &p_now) {
	if (!p_now.render_valid) {
		return;
	}
	if (r_w.reload_pending && p_now.render >= r_w.reload_at) {
		r_w.reload_pending = false;
		r_w.reloading = true;
		r_w.state.reload_start(r_w.last_owner.first_person, r_w.magazine > 0);
		r_w.reload_at += sw::SimTime(std::llround(r_w.sound->reload_s * double(sw::kTimeUnitsPerSecond)));
	}
	if (r_w.reloading && p_now.render >= r_w.reload_at) {
		r_w.reloading = false;
		r_w.magazine = r_w.sound->magazine;
		r_w.state.reload_end();
	}
}

// SOURCE: notes R5-VS3 2 steps 10 and 12 (the handle sits at the weapon's transform, its velocity the movement over dt).
void VehicleWeaponAudio::follow(Weapon &r_w, const present::PresentClock &p_now) {
	if (!r_w.crewed) {
		return;
	}
	const auto pose = std::find_if(poses_.begin(), poses_.end(),
			[&](const VehicleWeaponPose &p) { return p.vehicle == r_w.vehicle && p.def == r_w.def; });
	if (pose == poses_.end() || !core::finite(pose->position)) {
		return;
	}
	EmitterState e = r_w.emitter;
	e.position = pose->position;
	e.forward = pose->forward;
	if (r_w.pose_known && p_now.render_valid && p_now.render > r_w.pose_time) {
		const float dt = float(double(p_now.render - r_w.pose_time) / double(sw::kTimeUnitsPerSecond));
		e.velocity = core::vec::scale(core::vec::sub(pose->position, r_w.emitter.position), 1.0f / dt);
	}
	r_w.pose_known = true;
	r_w.pose_time = p_now.render;
	r_w.emitter_dirty = r_w.emitter_dirty || !same_place(e, r_w.emitter) || e.velocity.x != r_w.emitter.velocity.x ||
			e.velocity.y != r_w.emitter.velocity.y || e.velocity.z != r_w.emitter.velocity.z;
	r_w.emitter = e;
}

bool VehicleWeaponAudio::push(MixerCommandType p_type, InstanceHandle p_handle, uint32_t p_hash, float p_value) {
	MixerCommand c;
	c.type = p_type;
	c.handle = p_handle;
	c.name_hash = p_hash;
	c.value = p_value;
	c.at_frame = mixer_.earliest_frame();
	if (!mixer_.push(c)) {
		++stats_.mixer_refusals;
		return false;
	}
	return true;
}

bool VehicleWeaponAudio::create(Weapon &r_w, const ListenerState &p_listener) {
	const uint64_t seed = sound_seed(r_w.slot, 0x50000000u | (created_++ & 0x0FFFFFFFu));
	core::Result<std::unique_ptr<PatchInstance>> inst =
			PatchInstance::create(r_w.sound->program, bank_, tuning_, seed, mix_rate_);
	if (!inst) {
		++stats_.mixer_refusals;
		return false;
	}
	PatchInstance &pi = **inst;
	ops_.clear();
	r_w.state.inputs(ops_);
	r_w.sent.clear();
	for (const WeaponSoundOp &op : ops_) {
		if (declares(*r_w.sound->program, op.hash, false)) {
			pi.set_parameter(op.hash, op.value);
			r_w.sent.emplace_back(op.hash, op.value);
		}
	}
	pi.set_geometry(r_w.emitter, p_listener);
	if (observer_ != nullptr) {
		observer_->on_driver_instance(r_w.slot, seed, mixer_.earliest_frame(), pi);
	}
	MixerCommand start;
	start.type = MixerCommandType::Start;
	start.handle = mixer_.next_handle();
	start.at_frame = mixer_.earliest_frame();
	start.instance = &pi;
	if (!mixer_.push(start)) {
		++stats_.mixer_refusals;
		return false;
	}
	(void)inst->release();
	r_w.handle = start.handle;
	r_w.playing = true;
	r_w.emitter_dirty = false;
	++stats_.vehicle_instances;
	return true;
}

void VehicleWeaponAudio::apply(Weapon &r_w, std::span<const WeaponSoundOp> p_ops) {
	for (const WeaponSoundOp &op : p_ops) {
		if (trace_ && trace_ops_.size() < kMaxWeaponSoundStarts) {
			trace_ops_.push_back(VehicleWeaponOp{ updates_, r_w.slot, r_w.def, op });
		} else if (trace_) {
			++stats_.trace_dropped;
		}
		switch (op.kind) {
			case WeaponSoundOpKind::Post:
				if (r_w.handle != kNoInstance && declares(*r_w.sound->program, op.hash, true) &&
						push(MixerCommandType::PostEvent, r_w.handle, op.hash, 0.0f)) {
					++stats_.vehicle_events;
					stats_.vehicle_fires += op.hash == kHashFire ? 1u : 0u;
					stats_.vehicle_shot_posts += op.hash == kHashShotFired ? 1u : 0u;
				}
				break;
			case WeaponSoundOpKind::Set: {
				if (r_w.handle == kNoInstance || !declares(*r_w.sound->program, op.hash, false)) {
					break;
				}
				auto it = std::find_if(
						r_w.sent.begin(), r_w.sent.end(), [&](const auto &s) { return s.first == op.hash; });
				if (it != r_w.sent.end() && it->second == op.value) {
					break;
				}
				if (push(MixerCommandType::SetParameter, r_w.handle, op.hash, op.value)) {
					if (it != r_w.sent.end()) {
						it->second = op.value;
					} else {
						r_w.sent.emplace_back(op.hash, op.value);
					}
				}
				break;
			}
			case WeaponSoundOpKind::Release:
				if (r_w.handle != kNoInstance) {
					r_w.handle = kNoInstance;
					r_w.playing = false;
					r_w.sent.clear();
					++stats_.vehicle_releases;
				}
				break;
			case WeaponSoundOpKind::Detach:
				if (r_w.handle != kNoInstance) {
					Detached d{ r_w.handle, r_w.emitter, true };
					d.emitter.velocity = {};
					detached_.push_back(d);
					r_w.handle = kNoInstance;
					r_w.playing = false;
					++stats_.vehicle_detached;
				}
				break;
		}
	}
}

void VehicleWeaponAudio::destroy(Weapon &r_w) {
	ops_.clear();
	r_w.state.destroyed(r_w.handle != kNoInstance && r_w.playing, ops_);
	apply(r_w, ops_);
}

// STAND-IN: Bolt, LockOn and Action are not driven (no MP_007 vehicle weapon is bolt-action or locks on, and notes
// R5-VS3 do not identify action types 0 and 2 to 5). Settle: drive them from the vehicle weapon's fire logic.
void VehicleWeaponAudio::step(const present::PresentClock &p_now, const ListenerState &p_listener) {
	++updates_;
	for (size_t i = 0; i < weapons_.size();) {
		Weapon &w = weapons_[i];
		if (w.gone) {
			destroy(w);
			weapons_.erase(weapons_.begin() + std::ptrdiff_t(i));
			continue;
		}
		std::stable_sort(w.due.begin(), w.due.end(), [](const Due &a, const Due &b) { return a.time < b.time; });
		size_t done = 0;
		for (; done < w.due.size() && (!p_now.render_valid || w.due[done].time <= p_now.render); ++done) {
			fire(w, w.due[done]);
		}
		w.due.erase(w.due.begin(), w.due.begin() + std::ptrdiff_t(done));
		// INFERRED: an automatic pull ends when no shot follows within its shot interval plus a tick. Settle: a note on
		// how a client sees a remote fire end.
		if (w.pulling && p_now.render_valid &&
				p_now.render - w.last_shot > shot_units(*w.sound) + sw::kTimeUnitsPerTick) {
			w.state.trigger_release();
			w.state.fire_end();
			w.pulling = false;
		}
		reload(w, p_now);
		if (p_now.render_valid) {
			heat_to(w, p_now.render);
		}
		w.state.set_overheat(w.heat.heat);
		follow(w, p_now);
		if (w.state.wants_handle() && (w.handle == kNoInstance || !w.playing)) {
			(void)create(w, p_listener);
		}
		ops_.clear();
		w.state.update(w.handle != kNoInstance && w.playing, ops_);
		apply(w, ops_);
		if (w.handle != kNoInstance && w.emitter_dirty) {
			MixerCommand c;
			c.type = MixerCommandType::SetEmitter;
			c.handle = w.handle;
			c.emitter = w.emitter;
			if (mixer_.push(c)) {
				w.emitter_dirty = false;
				++stats_.emitter_updates;
			}
		}
		++i;
	}
	for (Detached &d : detached_) {
		if (!d.dirty) {
			continue;
		}
		MixerCommand c;
		c.type = MixerCommandType::SetEmitter;
		c.handle = d.handle;
		c.emitter = d.emitter;
		if (mixer_.push(c)) {
			d.dirty = false;
			++stats_.emitter_updates;
		}
	}
}

bool VehicleWeaponAudio::on_finished(InstanceHandle p_handle) {
	for (Weapon &w : weapons_) {
		if (w.handle == p_handle) {
			w.playing = false;
			return true;
		}
	}
	const auto it =
			std::find_if(detached_.begin(), detached_.end(), [&](const Detached &d) { return d.handle == p_handle; });
	if (it != detached_.end()) {
		detached_.erase(it);
		return true;
	}
	return false;
}

void VehicleWeaponAudio::set_observer(WeaponAudioObserver *p_observer) {
	observer_ = p_observer;
}

void VehicleWeaponAudio::set_trace(bool p_on) {
	trace_ = p_on;
	if (!p_on) {
		trace_ops_.clear();
	}
}

void VehicleWeaponAudio::take_ops(std::vector<VehicleWeaponOp> &r_out) {
	r_out.insert(r_out.end(), trace_ops_.begin(), trace_ops_.end());
	trace_ops_.clear();
}

}

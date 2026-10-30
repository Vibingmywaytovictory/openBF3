#include "client_gunplay.h"

#include "bf3/host/gunplay.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bf3::host {

namespace {

void set_bit(std::array<uint64_t, 2> &r_mask, uint32_t p_slot) {
	if (p_slot < 128) {
		r_mask[p_slot >> 6] |= uint64_t(1) << (p_slot & 63);
	}
}

void copy3(float (&r_to)[3], const sim::Vec3 &p_from) {
	for (size_t k = 0; k < 3; ++k) {
		r_to[k] = p_from[k];
	}
}

}

void ClientGunplay::Records::add(uint16_t p_seq, uint32_t p_tick, bool p_main, std::span<const net::OtherSoldier> p_records) {
	Seq &e = ring[p_seq % kSeqs];
	if (e.valid && e.seq != p_seq) {
		if (net::seq_newer(e.seq, p_seq)) {
			return;
		}
		e.valid = false;
	}
	if (!e.valid) {
		e.valid = true;
		e.seq = p_seq;
		e.tick = p_tick;
		e.main = e.others = false;
		e.main_records.clear();
		e.others_records.clear();
		e.main_at.fill(0xFF);
		e.others_at.fill(0xFF);
		e.main_slots = {};
		e.others_slots = {};
	}
	bool &arrived = p_main ? e.main : e.others;
	if (arrived) {
		return;
	}
	arrived = true;
	std::vector<net::OtherSoldier> &records = p_main ? e.main_records : e.others_records;
	std::array<uint8_t, net::kMaxSlots> &at = p_main ? e.main_at : e.others_at;
	std::array<uint64_t, 2> &slots = p_main ? e.main_slots : e.others_slots;
	for (const net::OtherSoldier &o : p_records) {
		if (o.slot >= net::kMaxSlots || at[o.slot] != 0xFF || records.size() >= 0xFF) {
			continue;
		}
		at[o.slot] = uint8_t(records.size());
		records.push_back(o);
		set_bit(slots, o.slot);
	}
}

uint32_t ClientGunplay::Records::mask(uint16_t p_newest, bool p_main) const {
	uint32_t bits = 0;
	for (uint32_t i = 0; i < 32; ++i) {
		if (arrived(uint16_t(p_newest - i), p_main)) {
			bits |= 1u << i;
		}
	}
	return bits;
}

size_t ClientGunplay::Records::candidates(const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther> r_out) const {
	size_t n = 0;
	if (p_target >= net::kMaxSlots) {
		return 0;
	}
	for (uint32_t i = 0; i < 32 && n < r_out.size(); ++i) {
		const Seq *e = find(uint16_t(p_view.view_seq - i));
		if (e == nullptr) {
			continue;
		}
		if (((p_view.main_bits >> i) & 1u) != 0 && e->main && e->main_at[p_target] != 0xFF) {
			r_out[n++] = { e->tick, e->main_records[e->main_at[p_target]] };
		}
		if (n < r_out.size() && ((p_view.others_bits >> i) & 1u) != 0 && e->others && e->others_at[p_target] != 0xFF) {
			r_out[n++] = { e->tick, e->others_records[e->others_at[p_target]] };
		}
	}
	return n;
}

bool ClientGunplay::Records::view_tick(uint16_t p_seq, uint32_t &r_tick) const {
	const Seq *e = find(p_seq);
	if (e == nullptr) {
		return false;
	}
	r_tick = e->tick;
	return true;
}

void ClientGunplay::Records::slots_in(const ShotView &p_view, std::array<uint64_t, 2> &r_slots) const {
	r_slots = {};
	for (uint32_t i = 0; i < 32; ++i) {
		const Seq *e = find(uint16_t(p_view.view_seq - i));
		if (e == nullptr) {
			continue;
		}
		for (size_t w = 0; w < 2; ++w) {
			if (((p_view.main_bits >> i) & 1u) != 0 && e->main) {
				r_slots[w] |= e->main_slots[w];
			}
			if (((p_view.others_bits >> i) & 1u) != 0 && e->others) {
				r_slots[w] |= e->others_slots[w];
			}
		}
	}
}

void ClientGunplay::Emit::emitted(const sim::Command &p_command, const sim::weapons::WeaponTickOutput &p_output, const sim::WeaponPredictionEntry &p_after) {
	ClientGunplay &s = *m;
	const CommandView &v = s.views_[p_command.tick % kCommandViews];
	const bool have_view = v.valid && v.tick == p_command.tick && v.sequence == p_command.sequence;
	const uint8_t own = s.own_slot_;
	for (size_t i = 0; i < p_output.shot_count && i < p_output.shots.size(); ++i) {
		const sim::weapons::ShotEvent &shot = p_output.shots[i];
		PresentEvent e;
		e.kind = PresentKind::OwnShot;
		e.timeline = PresentTimeline::Predicted;
		e.slot = own;
		e.id = shot.shot_index;
		e.def = shot.def;
		copy3(e.pos, shot.origin);
		copy3(e.dir, shot.direction);
		e.aux = shot.weapon_slot;
		e.flags = shot.tracer ? present_flags::kTracer : uint8_t(0);
		e.seed = effect_seed(s.match_seed_, own, shot.shot_index);
		e.value = shot.power;
		e.extra = shot.pellets;
		e.time = shot.time;
		s.events_.push(e);
		++s.predicted_shots_;
		if (!have_view) {
			++s.no_view_shots_;
		} else {
			s.fired_ = true;
			s.fired_view_seq_ = v.view.view_seq;
			s.fired_ns_ = s.clock_();
		}
		if (s.projectiles_ != nullptr) {
			s.projectiles_->spawn_own(shot, have_view ? v.view : ShotView{});
		}
	}
	for (size_t i = 0; i < p_output.timed_count && i < p_output.timed.size(); ++i) {
		const sim::weapons::TimedWeaponEvent &t = p_output.timed[i];
		PresentEvent e;
		e.kind = PresentKind::OwnTimed;
		e.timeline = PresentTimeline::Predicted;
		e.slot = own;
		e.aux = t.weapon_slot;
		const sim::weapons::WeaponSlotState &ws = p_after.state.slots[t.weapon_slot < sim::weapons::kMaxWeaponSlots ? t.weapon_slot : 0];
		e.def = ws.def;
		e.id = t.event;
		e.time = t.time;
		s.events_.push(e);
		++s.predicted_timed_;
	}
	if (s.config_.record_predicted && (p_output.shot_count > 0 || p_output.timed_count > 0)) {
		std::lock_guard lock(s.log_mutex_);
		for (size_t i = 0; i < p_output.shot_count && s.shot_log_.size() < kLogLimit; ++i) {
			s.shot_log_.push_back({ p_command.tick, p_output.shots[i] });
		}
		for (size_t i = 0; i < p_output.timed_count && s.timed_log_.size() < kLogLimit; ++i) {
			s.timed_log_.push_back({ p_command.tick, p_output.timed[i] });
		}
	}
}

std::optional<GunplayRefusal> ClientGunplay::match_info(const net::MatchInfo &p_match, const SharedLevel &p_level) {
	if (p_match.weapon_hash == 0) {
		return std::nullopt;
	}
	inputs_ = config_.gunplay;
	if (inputs_ == nullptr && config_.gunplay_from_level && p_level.gunplay != nullptr) {
		core::Result<std::shared_ptr<const ClientGunplayInputs>> own = client_gunplay_inputs(p_level);
		if (!own) {
			++weapon_hash_mismatch_;
			return GunplayRefusal{ net::mismatch_bits::kWeapon, "weapon table mismatch (" + own.error().message + ")" };
		}
		inputs_ = *own;
	}
	const ClientGunplayInputs *gp = inputs_.get();
	if (gp == nullptr || gp->table == nullptr || gp->capsules == nullptr || !core::gameplay::capsule_pose_table_valid(*gp->capsules)) {
		++weapon_hash_mismatch_;
		return GunplayRefusal{ net::mismatch_bits::kWeapon, "weapon table mismatch (this client has no weapon table)" };
	}
	core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> table = build_client_weapon_table(*gp, config_.mechanics_cfg, p_match.weapon_flags);
	if (!table) {
		++weapon_hash_mismatch_;
		return GunplayRefusal{ net::mismatch_bits::kWeapon, "weapon table mismatch (" + table.error().message + ")" };
	}
	if (client_weapon_hash(**table, *gp->capsules) != p_match.weapon_hash) {
		++weapon_hash_mismatch_;
		return GunplayRefusal{ net::mismatch_bits::kWeapon, "weapon table mismatch" };
	}
	if (p_match.kit_hash != 0 && p_match.kit_hash != gp->kits.hash) {
		++kit_hash_mismatch_;
		return GunplayRefusal{ net::mismatch_bits::kKit, "kit table mismatch" };
	}
	table_ = *table;
	match_seed_ = p_match.match_seed;
	frame_ = gp->shot_frame;
	lane_ = std::make_unique<sim::WeaponLane>(config_.prediction_ring, table_, p_level.motor, match_seed_);
	emit_.m = this;
	lane_->set_sink(&emit_);
	ClientProjectilesConfig pc;
	pc.own_slot = p_match.slot;
	pc.max_rewind_ticks = config_.own_test_max_rewind_ticks >= 0 ? uint32_t(config_.own_test_max_rewind_ticks) : uint32_t(p_match.max_rewind_ticks);
	for (size_t k = 0; k < sim::kPoseCount; ++k) {
		pc.pose_eye_heights[k] = p_level.motor.poses[k].eye_height;
	}
	pc.extrapolate_max_ms = config_.extrapolate_max_ms;
	core::Result<std::unique_ptr<ClientProjectiles>> p = ClientProjectiles::create(table_, gp->capsules, match_seed_, pc);
	if (!p) {
		return GunplayRefusal{ net::mismatch_bits::kWeapon, "weapon table mismatch (" + p.error().message + ")" };
	}
	projectiles_ = std::move(*p);
	on_ = true;
	return std::nullopt;
}

void ClientGunplay::spawned(const net::Spawned &p_spawned, bool p_have_own, uint32_t p_own_life) {
	Life &l = lives_[p_spawned.state.life % lives_.size()];
	l.valid = true;
	l.life = p_spawned.state.life;
	l.kit = p_spawned.kit;
	l.team = p_spawned.team;
	l.defs = p_spawned.defs;
	if (p_have_own && p_spawned.state.life == p_own_life) {
		kit_ = p_spawned.kit;
		team_ = p_spawned.team;
	}
}

void ClientGunplay::note_arrival(uint16_t p_seq, uint64_t p_now) {
	if (fired_ && p_now - fired_ns_ < 1'000'000'000ull && !net::seq_newer(p_seq, fired_view_seq_)) {
		++late_sample_;
	}
}

void ClientGunplay::snapshot(const net::Snapshot &p_snapshot, bool p_newer) {
	records_.add(p_snapshot.seq, p_snapshot.server_tick, true, p_snapshot.others);
	if (p_newer) {
		newest_tick_ = p_snapshot.server_tick;
		wire_valid_ = p_snapshot.own_weapons.has_value();
		if (wire_valid_) {
			wire_ = *p_snapshot.own_weapons;
		}
		wire_tick_ = p_snapshot.server_tick;
	}
}

void ClientGunplay::others(const net::SnapshotOthers &p_others, uint64_t p_now) {
	++snapshot_others_received_;
	note_arrival(p_others.seq, p_now);
	records_.add(p_others.seq, p_others.server_tick, false, p_others.others);
}

bool ClientGunplay::def_ok(uint32_t p_def) const {
	return table_ != nullptr && p_def < table_->defs.size();
}

void ClientGunplay::shot_events(const net::NetSim::Datagram &p_d) {
	core::Result<net::ShotEvents> e = net::decode_shot_events(p_d.bytes, frame_);
	if (!e) {
		++bad_datagrams_;
		return;
	}
	for (const net::RemoteShot &r : e->shots) {
		if (r.slot == own_slot_) {
			continue;
		}
		if (!valid_slot(r.slot) || !def_ok(r.def)) {
			++bad_events_;
			continue;
		}
		PresentEvent ev;
		ev.kind = PresentKind::RemoteShot;
		ev.timeline = PresentTimeline::Render;
		ev.slot = r.slot;
		ev.id = r.shot_index;
		ev.def = r.def;
		copy3(ev.pos, r.origin);
		copy3(ev.dir, r.direction);
		ev.flags = r.tracer ? present_flags::kTracer : uint8_t(0);
		ev.seed = effect_seed(match_seed_, r.slot, r.shot_index);
		ev.value = r.power;
		ev.extra = r.pellets;
		ev.time = r.time;
		events_.push(ev);
		++remote_shots_;
		if (projectiles_ != nullptr) {
			projectiles_->queue_remote(r);
		}
	}
}

bool ClientGunplay::combat(const net::NetSim::Datagram &p_d, net::CombatEvents &r_events) {
	core::Result<net::CombatEvents> e = net::decode_combat_events(p_d.bytes, frame_);
	if (!e) {
		++bad_datagrams_;
		return false;
	}
	const uint8_t slot = own_slot_;
	for (const net::HitEvent &h : e->hits) {
		if (!valid_slot(h.victim) || !valid_slot(h.shooter)) {
			++bad_events_;
			continue;
		}
		PresentEvent ev;
		ev.kind = PresentKind::Hit;
		const bool own_victim = h.victim == slot, own_shooter = h.shooter == slot;
		ev.timeline = own_victim || own_shooter ? PresentTimeline::Immediate : PresentTimeline::Render;
		ev.flags = uint8_t((own_victim ? present_flags::kOwnVictim : 0u) | (own_shooter ? present_flags::kOwnShooter : 0u));
		ev.slot = h.shooter;
		ev.aux = h.victim;
		ev.id = h.shot_index;
		ev.pellet = h.pellet;
		copy3(ev.pos, h.position);
		copy3(ev.dir, h.normal);
		ev.value = h.damage;
		ev.extra = std::isfinite(h.health_after) ? uint32_t(std::clamp(h.health_after * 100.0f, 0.0f, 1.0e7f)) : 0u;
		ev.material = uint32_t(h.zone);
		ev.time = h.time;
		events_.push(ev);
		++combat_hits_;
	}
	for (const net::DeathEvent &d : e->deaths) {
		if (!valid_slot(d.victim) || !valid_slot(d.killer) || (d.def != sim::weapons::kNoWeapon && !def_ok(d.def))) {
			++bad_events_;
			continue;
		}
		PresentEvent ev;
		ev.kind = PresentKind::Death;
		ev.timeline = PresentTimeline::Render;
		ev.flags = uint8_t((d.headshot ? present_flags::kHeadshot : 0u) | (d.victim == slot ? present_flags::kOwnVictim : 0u) |
				(d.killer == slot ? present_flags::kOwnShooter : 0u));
		ev.slot = d.killer;
		ev.aux = d.victim;
		ev.def = d.def;
		ev.id = d.shot_index;
		ev.material = uint32_t(d.zone);
		ev.time = d.time;
		ev.value = d.killer_health;
		ev.extra = d.respawn_tick;
		ev.seed = d.revive_until;
		events_.push(ev);
		++combat_deaths_;
	}
	for (const net::DetonationWire &d : e->detonations) {
		if (!valid_slot(d.shooter) || !def_ok(d.def)) {
			++bad_events_;
			continue;
		}
		PresentEvent ev;
		ev.kind = PresentKind::Detonation;
		ev.timeline = PresentTimeline::Render;
		ev.flags = d.dud ? present_flags::kDud : uint8_t(0);
		ev.slot = d.shooter;
		ev.id = d.shot_index;
		ev.def = d.def;
		copy3(ev.pos, d.position);
		copy3(ev.dir, d.normal);
		ev.time = d.time;
		events_.push(ev);
		++combat_detonations_;
	}
	r_events = std::move(*e);
	return true;
}

bool ClientGunplay::put_wire(const sim::SoldierState &p_s, sim::Tick p_spawn_tick) {
	core::Result<sim::weapons::SoldierWeaponsState> w = net::weapons_from_wire(wire_, p_s.tick, own_slot_, table_->defs.size());
	if (!w) {
		++bad_weapon_wires_;
		return false;
	}
	sim::WeaponPredictionEntry e;
	e.tick = p_s.tick;
	e.spawn_tick = p_spawn_tick;
	e.state = *w;
	lane_->put(e);
	return true;
}

void ClientGunplay::weapons_spawn(const sim::SoldierState &p_s) {
	if (!on_) {
		return;
	}
	const Life &l = lives_[p_s.life % lives_.size()];
	const sim::Tick t = p_s.tick;
	lane_->set_emitted_through(t);
	PresentEvent spawn;
	spawn.kind = PresentKind::Spawn;
	spawn.timeline = PresentTimeline::Predicted;
	spawn.slot = own_slot_;
	spawn.id = p_s.life;
	spawn.time = sim::weapons::tick_end(t);
	if (l.valid && l.life == p_s.life && l.team >= 1 && l.team <= kKitTeams && l.kit < kKitCount) {
		const KitLoadout &k = inputs_->kits.loadouts[size_t(l.team - 1)][l.kit];
		const bool sent = std::any_of(
				l.defs.begin(), l.defs.end(), [&](uint16_t p_def) { return p_def != sim::weapons::kNoWeapon; });
		const std::array<uint16_t, sim::weapons::kMaxWeaponSlots> &defs = sent ? l.defs : k.defs;
		sim::WeaponPredictionEntry e;
		const sim::WeaponPredictionEntry *prev = lane_->newest_at_or_below(t);
		if (prev != nullptr) {
			e.state = prev->state;
		}
		e.state.player_slot = own_slot_;
		if (sim::weapons::spawn_soldier_weapons(e.state, *table_, defs, k.active, t).ok()) {
			e.tick = t;
			e.spawn_tick = t;
			lane_->put(e);
		}
		kit_ = l.kit;
		team_ = l.team;
	} else if (wire_at(t)) {
		put_wire(p_s, t);
	}
	spawn.aux = kit_;
	spawn.extra = team_;
	events_.push(spawn);
}

void ClientGunplay::weapons_ring_miss(const sim::SoldierState &p_s) {
	if (wire_at(p_s.tick)) {
		const sim::WeaponPredictionEntry *old = lane_->newest_at_or_below(p_s.tick);
		if (put_wire(p_s, old != nullptr ? old->spawn_tick : p_s.tick)) {
			++weapon_ring_misses_;
		}
	}
}

void ClientGunplay::weapons_reconcile(const sim::SoldierState &p_s) {
	if (!wire_at(p_s.tick)) {
		return;
	}
	const sim::WeaponPredictionEntry *w = lane_->find(p_s.tick);
	if (w == nullptr) {
		const sim::WeaponPredictionEntry *old = lane_->newest_at_or_below(p_s.tick);
		if (put_wire(p_s, old != nullptr ? old->spawn_tick : p_s.tick)) {
			++weapon_ring_misses_;
		}
	} else if (!(net::weapons_wire_view(w->state, p_s.tick) == wire_)) {
		if (put_wire(p_s, w->spawn_tick)) {
			++weapon_reconciliations_;
		}
	}
}

bool ClientGunplay::reconcile_after_match(const sim::SoldierState &p_s) {
	if (!(wire_at(p_s.tick) && p_s.life != 0 && p_s.mode != sim::MoveMode::Dead)) {
		return false;
	}
	const uint64_t before = weapon_reconciliations_ + weapon_ring_misses_;
	weapons_reconcile(p_s);
	return weapon_reconciliations_ + weapon_ring_misses_ != before;
}

void ClientGunplay::command_made(sim::Command &r_command, const InputSample &p_input, uint32_t p_own_life, uint64_t p_now, bool p_have_snapshot,
		uint16_t p_newest_seq, const PresentationFrame &p_built) {
	const InputSample &s = p_input;
	const bool pressed = fire_seen_ && s.fire_presses != last_fire_presses_;
	fire_seen_ = true;
	last_fire_presses_ = s.fire_presses;
	if (pressed && s.life >= p_own_life && !s.free_camera) {
		r_command.buttons |= sim::buttons::kFire;
	}

	double render = 0;
	sim::ViewQuality quality = sim::ViewQuality::None;
	uint16_t seq = 0;
	if (s.view_valid) {
		render = s.view_render_ticks;
		quality = sim::ViewQuality(std::min<uint8_t>(s.view_quality, uint8_t(sim::ViewQuality::Held)));
		seq = s.view_seq;
	} else if (p_built.serial != 0) {
		render = present_render_ticks(p_built, p_now);
		quality = present_view_quality(p_built, render);
		seq = p_built.newest_seq;
	}
	if (!p_have_snapshot || !std::isfinite(render)) {
		quality = sim::ViewQuality::None;
	}
	CommandView &v = views_[r_command.tick % kCommandViews];
	v = CommandView();
	v.valid = true;
	v.tick = r_command.tick;
	v.sequence = r_command.sequence;
	if (quality == sim::ViewQuality::None) {
		r_command.view_back_q8 = 0;
		r_command.view_quality = 0;
		r_command.view_seq8 = 0;
		return;
	}
	const double back = double(int64_t(r_command.tick) << 8) - std::round(render * 256.0);
	r_command.view_back_q8 = uint16_t(std::clamp(back, 0.0, 65535.0));
	r_command.view_quality = uint8_t(quality);
	r_command.view_seq8 = uint8_t(seq & 0xFFu);
	v.view.view_back_q8 = r_command.view_back_q8;
	v.view.quality = r_command.view_quality;

	const uint16_t back_seqs = uint16_t(p_newest_seq - seq);
	if (back_seqs < kViewSeqWindow) {
		v.view.view_seq = seq;
		v.view.main_bits = records_.mask(seq, true);
		v.view.others_bits = records_.mask(seq, false);
	} else {
		v.valid = false;
	}
}

void ClientGunplay::predict(const sim::Command &p_command, const sim::SoldierState &p_before, const sim::SoldierState &p_after,
		const sim::CollisionWorld &p_world) {
	if (!on_ || p_command.tick == 0) {
		return;
	}
	const sim::WeaponPredictionEntry *b = lane_->find(p_command.tick - 1u);
	if (b == nullptr) {
		b = lane_->newest_at_or_below(p_command.tick - 1u);
	}
	if (b != nullptr) {
		const sim::WeaponPredictionEntry from = *b;
		lane_->put(lane_->step(from, p_command, p_before, p_after));
	}
	if (projectiles_ != nullptr) {
		projectiles_->step_own(p_command.tick, p_world, records_, events_.own_staging());
		events_.drain_own();
	}
}

uint32_t ClientGunplay::others_ack_bits(bool p_have_snapshot, uint16_t p_newest_seq) const {
	return p_have_snapshot ? records_.mask(p_newest_seq, false) : 0u;
}

void ClientGunplay::publish(PresentationFrame &r_frame, std::span<const net::OtherPose> p_poses, const sim::CollisionWorld &p_world) {
	r_frame.newest_tick = newest_tick_;
	r_frame.match_seed = match_seed_;
	r_frame.own_kit = kit_;
	r_frame.own_team = team_;
	r_frame.own_weapons_valid = false;
	if (on_ && r_frame.valid && r_frame.tick > 0) {
		const sim::WeaponPredictionEntry *a = lane_->find(r_frame.tick - 1u);
		const sim::WeaponPredictionEntry *b = lane_->find(r_frame.tick);
		if (a != nullptr && b != nullptr) {
			r_frame.own_weapons_prev = net::weapons_wire_view(a->state, r_frame.tick - 1u);
			r_frame.own_weapons_curr = net::weapons_wire_view(b->state, r_frame.tick);
			r_frame.own_weapons_valid = true;
		}
	}
	r_frame.grenade_count = 0;
	if (projectiles_ != nullptr) {
		projectiles_->step_remote(r_frame.render_ticks_at_publish, p_world, p_poses, events_.ring());
		r_frame.grenade_count = uint8_t(projectiles_->live_grenades(r_frame.grenades));
		sample_scratch_.resize(4096);
		size_t got = 0;
		while ((got = projectiles_->take_samples(sample_scratch_)) > 0) {
			std::lock_guard lock(log_mutex_);
			for (size_t k = 0; k < got && sample_log_.size() < kLogLimit; ++k) {
				sample_log_.push_back(sample_scratch_[k]);
			}
		}
	}
}

void ClientGunplay::take_predicted(std::vector<PredictedShot> &r_shots, std::vector<PredictedTimed> &r_timed) {
	std::lock_guard lock(log_mutex_);
	r_shots.insert(r_shots.end(), shot_log_.begin(), shot_log_.end());
	r_timed.insert(r_timed.end(), timed_log_.begin(), timed_log_.end());
	shot_log_.clear();
	timed_log_.clear();
}

void ClientGunplay::take_own_test_samples(std::vector<OwnTestSample> &r_samples) {
	std::lock_guard lock(log_mutex_);
	r_samples.insert(r_samples.end(), sample_log_.begin(), sample_log_.end());
	sample_log_.clear();
}

void ClientGunplay::publish_stats(ClientStats &r_stats) const {
	if (projectiles_ != nullptr) {
		r_stats.projectiles = projectiles_->stats();
	}
	if (lane_ != nullptr) {
		r_stats.lane = lane_->stats();
	}
}

void ClientGunplay::stats(ClientStats &r_stats) const {
	r_stats.gunplay = on_;
	r_stats.weapon_hash_mismatch = weapon_hash_mismatch_;
	r_stats.kit_hash_mismatch = kit_hash_mismatch_;
	r_stats.weapon_reconciliations = weapon_reconciliations_;
	r_stats.weapon_ring_misses = weapon_ring_misses_;
	r_stats.predicted_shots = predicted_shots_;
	r_stats.predicted_timed = predicted_timed_;
	r_stats.no_view_shots = no_view_shots_;
	r_stats.snapshot_others_received = snapshot_others_received_;
	r_stats.remote_shots = remote_shots_;
	r_stats.combat_hits = combat_hits_;
	r_stats.combat_deaths = combat_deaths_;
	r_stats.combat_detonations = combat_detonations_;
	r_stats.bad_gunplay_datagrams = bad_datagrams_;
	r_stats.bad_weapon_wires = bad_weapon_wires_;
	r_stats.late_sample = late_sample_;
}

}

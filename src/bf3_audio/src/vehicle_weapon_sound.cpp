#include "bf3/audio/vehicle_weapon_sound.h"

namespace bf3::audio {

namespace {

void post(std::vector<WeaponSoundOp> &r_out, uint32_t p_hash) {
	r_out.push_back(WeaponSoundOp{ WeaponSoundOpKind::Post, p_hash, 0.0f });
}

void set(std::vector<WeaponSoundOp> &r_out, uint32_t p_hash, float p_value) {
	r_out.push_back(WeaponSoundOp{ WeaponSoundOpKind::Set, p_hash, p_value });
}

float flag(bool p_on) {
	return p_on ? 1.0f : 0.0f;
}

}

void WeaponSoundState::fire_start(const WeaponSoundOwner &p_owner) {
	if (!firing_) {
		fire_ = true;
		firing_ = true;
	}
	end_ = false;
	shot_mark_ = false;
	owner_ = p_owner;
}

void WeaponSoundState::shot() {
	++shots_;
	shot_mark_ = true;
}

void WeaponSoundState::fire_end() {
	end_ = true;
}

void WeaponSoundState::trigger_release() {
	if (!(owner_.local && shot_mark_)) {
		end_ = true;
	}
}

void WeaponSoundState::reload_start(bool p_first_person, bool p_short_reload) {
	reload_ = true;
	owner_.first_person = p_first_person;
	short_reload_ = flag(p_first_person && p_short_reload);
}

void WeaponSoundState::reload_end() {
	reload_end_ = true;
}

void WeaponSoundState::bolt() {
	bolt_ = true;
}

void WeaponSoundState::action(uint8_t p_type, float p_zoom, uint8_t p_fire_logic) {
	action_ = true;
	action_type_ = float(p_type);
	if (p_type == kZoomAction) {
		zoom_ = p_zoom;
	}
	if (p_type == kFireModeAction) {
		fire_logic_ = float(p_fire_logic);
	}
}

void WeaponSoundState::lock_on(const WeaponSoundOwner &p_owner, float p_amount) {
	lock_ = true;
	owner_ = p_owner;
	lock_amount_ = p_amount;
}

void WeaponSoundState::set_fire_logic(uint8_t p_type) {
	fire_logic_ = float(p_type);
}

void WeaponSoundState::set_overheat(float p_heat) {
	overheat_ = p_heat;
}

void WeaponSoundState::set_lock_amount(float p_amount) {
	lock_amount_ = p_amount;
}

bool WeaponSoundState::wants_handle() const {
	return reload_ || reload_end_ || action_ || fire_ || bolt_ || shots_ > 0 || lock_ || (end_ && firing_);
}

void WeaponSoundState::inputs(std::vector<WeaponSoundOp> &r_out) const {
	set(r_out, kHashFireLogicType, fire_logic_);
	set(r_out, kHashIsFirstPerson, flag(owner_.first_person));
	set(r_out, kHashOwnerSameTeam, flag(owner_.same_team));
	set(r_out, kHashOwnerLocal, flag(owner_.local));
	set(r_out, kHashOverheatLevel, overheat_);
	set(r_out, kHashLockAmount, lock_amount_);
	set(r_out, kHashWeaponFlag, kDefaultWeaponFlag);
}

void WeaponSoundState::update(bool p_playing, std::vector<WeaponSoundOp> &r_out) {
	if (reload_) {
		set(r_out, kHashIsShortReload, short_reload_);
		post(r_out, kHashReload);
	}
	if (reload_end_) {
		post(r_out, kHashReloadEnd);
	}
	if (action_) {
		set(r_out, kHashActionType, action_type_);
		set(r_out, kHashZoomLevel, zoom_);
		post(r_out, kHashAction);
	}
	if (fire_) {
		post(r_out, kHashFire);
	}
	if (bolt_) {
		post(r_out, kHashBolt);
	}
	if (shots_ > 0) {
		--shots_;
		post(r_out, kHashShotFired);
	}
	if (lock_) {
		post(r_out, kHashLockOn);
	}
	if (end_ && shots_ == 0) {
		end_ = false;
		if (firing_) {
			post(r_out, kHashFireStopped);
		}
		firing_ = false;
	}
	if (!p_playing && !owner_.local) {
		r_out.push_back(WeaponSoundOp{ WeaponSoundOpKind::Release, 0, 0.0f });
	} else if (p_playing) {
		inputs(r_out);
	}
	reload_ = reload_end_ = action_ = fire_ = bolt_ = lock_ = false;
}

void WeaponSoundState::destroyed(bool p_playing, std::vector<WeaponSoundOp> &r_out) {
	if (p_playing) {
		if (firing_) {
			post(r_out, kHashFireStopped);
		}
		post(r_out, kHashWeaponGone);
		r_out.push_back(WeaponSoundOp{ WeaponSoundOpKind::Detach, 0, 0.0f });
	} else {
		r_out.push_back(WeaponSoundOp{ WeaponSoundOpKind::Release, 0, 0.0f });
	}
	shots_ = 0;
	firing_ = false;
	reload_ = reload_end_ = action_ = fire_ = bolt_ = lock_ = false;
}

}

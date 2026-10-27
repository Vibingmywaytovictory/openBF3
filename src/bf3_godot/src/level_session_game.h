#pragma once

#include "level_session.h"
#include "m2a_check.h"
#include "crosshair_walk_check.h"
#include "native_hud_moments.h"
#include "game_frame.h"
#include "game_settings.h"
#include "tdm_flow.h"

#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/firefight_brain.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/sim/collision_world.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <godot_cpp/variant/vector3.hpp>

namespace godot {
class CanvasLayer;
class Label;
class StandardMaterial3D;
}

namespace bf3::godot_side {

class Bf3HudItems;
class Bf3NativeHud;
class Bf3Localisation;
class Bf3UiAssets;
class Bf3Viewmodel;
class Bf3GameAudio;
class Bf3GameFx;
class Bf3GameCrowd;

template <typename T>
void delete_part(T *p_part) {
	delete p_part;
}

inline std::filesystem::path utf8_path(const std::string &p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

class ServerShotRecorder : public bf3::host::GunplayObserver {
public:
	static constexpr size_t kMaxRecords = 1u << 16;
	struct Shot {
		uint32_t tick = 0;
		bf3::sim::weapons::ShotEvent shot;
	};
	struct Timed {
		uint32_t tick = 0;
		bf3::sim::weapons::TimedWeaponEvent event;
	};
	struct Damage {
		uint32_t tick = 0;
		bf3::sim::weapons::DamageEvent event;
		bool applied = false;
		float health_after = 0;
	};
	void watch_slot(int p_slot) { slot_.store(p_slot, std::memory_order_release); }
	void weapon_tick(uint8_t p_slot, const bf3::sim::Command &p_command, const bf3::sim::weapons::SoldierWeaponsState &p_after,
			const bf3::sim::weapons::WeaponTickOutput &p_out) override;
	void damage(uint32_t p_tick, const bf3::sim::weapons::DamageEvent &p_event, bool p_applied, float p_health_after) override;

	struct Detonation {
		uint32_t tick = 0;
		bf3::sim::weapons::DetonationEvent event;
	};
	void projectiles(uint32_t p_tick, const bf3::sim::weapons::ProjectileStepOutput &p_out, std::span<const bf3::sim::weapons::Projectile> p_live) override;

	void take(std::vector<Shot> &r_shots, std::vector<Timed> &r_timed, std::vector<Damage> &r_damage);
	void take_detonations(std::vector<Detonation> &r_detonations);
	uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
	std::atomic<int> slot_{ -1 };
	std::atomic<uint64_t> dropped_{ 0 };
	std::mutex mutex_;
	std::vector<Shot> shots_;
	std::vector<Timed> timed_;
	std::vector<Damage> damage_;
	std::vector<Detonation> detonations_;
};

struct TdmState;

struct GameState {

	bool enabled = false;
	bool playable = false;
	uint8_t kit = 0;
	uint8_t team = 1;
	double respawn_s = 5.0;
	std::vector<M2aCase> cases;
	M2aCheckOptions check_options;
	bool checks() const { return !cases.empty(); }
	bool hud_ready() const;
	bool wants_case(M2aCase p_case) const;

	std::shared_ptr<const bf3::host::ClientGunplayInputs> gunplay;
	bf3::host::HudStatic hud_static;
	std::string hud_static_error;

	std::array<std::unique_ptr<bf3::render::ViewmodelSet>, 2> vm_sets;
	std::vector<bf3::render::ViewmodelKit> vm_kits;
	std::string vm_error;
	double vm_seconds = 0;
	bf3::anim::ViewmodelTuning vm_tuning;
	std::string vm_tuning_source;

	std::array<std::unordered_map<uint16_t, int>, 2> def_items;
	std::vector<bool> def_switch_when_empty;
	double vm_release_end_seen = -1;
	bool slot_selectable(const bf3::net::WeaponSlotWire &p_slot) const;
	std::unique_ptr<bf3::sim::CollisionWorld> world;
	std::shared_ptr<ServerShotRecorder> recorder;

	bool built = false;
	Bf3Localisation *localisation = nullptr;
	Bf3UiAssets *ui_assets = nullptr;
	Bf3HudItems *hud_items = nullptr;
	Bf3NativeHud *native_hud = nullptr;
	bool hud_names_applied = false;
	std::array<Bf3Viewmodel *, 2> vm{ nullptr, nullptr };
	godot::CanvasLayer *layer = nullptr;
	godot::Label *score_label = nullptr;
	godot::Label *death_label = nullptr;
	godot::Label *kit_label = nullptr;
	std::array<godot::Ref<godot::StandardMaterial3D>, 3> tints;
	int shown_team = 0;
	float base_sensitivity = 0;

	std::vector<bf3::host::PresentEvent> events;
	bf3::host::PresentedWeapon weapon;
	bool alive = false;
	bool spawned = false;
	uint8_t own_slot = 0xFF;
	uint8_t own_team = 0;
	uint32_t own_life = 0;
	bf3::host::HudFeedState hud_state;
	bf3::core::ui::HudModel hud_model{};
	std::vector<bf3::anim::VmCue> cues;
	uint64_t vm_shot_cues = 0;
	int vm_item = -1;
	uint64_t vm_unmapped = 0;
	double prev_yaw = 0, prev_pitch = 0;
	bool prev_view_valid = false;

	uint32_t kills = 0, deaths = 0;
	std::array<uint32_t, 3> team_kills{};
	uint64_t deaths_seen = 0, hits_seen = 0, own_shots_seen = 0, remote_shots_seen = 0;
	std::array<uint8_t, 256> slot_team{};

	bool dead = false;
	uint8_t killed_by = 0xFF;
	bool killed_by_self = false;
	uint64_t death_ns = 0;
	uint32_t respawns = 0;
	uint32_t deaths_by_enemy = 0;

	uint8_t next_kit = 0, next_team = 1;
	bool kit_pending = false;
	uint64_t kit_selects_sent = 0;

	bool record_predicted = false;
	std::vector<bf3::host::PredictedShot> predicted_shots;
	std::vector<bf3::host::PredictedTimed> predicted_timed;
	std::vector<ServerShotRecorder::Shot> server_shots;
	std::vector<ServerShotRecorder::Timed> server_timed;
	std::vector<ServerShotRecorder::Damage> server_damage;
	std::vector<ServerShotRecorder::Detonation> server_detonations;
	uint64_t events_drained = 0;
	uint64_t frames = 0;

	std::unique_ptr<Bf3GameFx, void (*)(Bf3GameFx *)> fx{ nullptr, nullptr };
	std::unique_ptr<Bf3GameAudio, void (*)(Bf3GameAudio *)> audio{ nullptr, nullptr };
	std::unique_ptr<Bf3GameCrowd, void (*)(Bf3GameCrowd *)> crowd{ nullptr, nullptr };
	std::unique_ptr<GameSettings> settings;
	std::string settings_path;
	bool settings_missed_printed = false;
	bool crowd_built = false;
	std::vector<std::string> part_notes;

	std::shared_ptr<bf3::host::FirefightStats> firefight_stats;
	uint32_t firefight_bots = 0;
	bool firefight_pairs = false;

	bool fx_off = false;

	bool camera_valid = false;
	godot::Vector3 camera_prev;

	double ms_vm = 0, ms_hud = 0, ms_crowd = 0, ms_fx = 0, ms_audio = 0, ms_total = 0;
	float fp_fov_applied = -1.0f;
	uint64_t own_muzzles_fx = 0, own_muzzles_standin = 0;

	bool listener_override = false;
	godot::Vector3 listener_position, listener_forward{ 0, 0, -1 };

	Bf3Viewmodel *own_viewmodel() const { return own_team == 2 ? vm[1] : (own_team == 1 ? vm[0] : nullptr); }

	int item_of(uint8_t p_team, uint16_t p_def) const;

	std::unique_ptr<TdmState, void (*)(TdmState *)> tdm{ nullptr, nullptr };
	TdmPreview tdm_preview;
};

struct Bf3LevelSession::Game : GameState {

	std::thread vm_loader;
	std::atomic<bool> vm_done{ false };
	bool vm_built = false;
	bf3::render::LoadProgress vm_progress;

	std::thread crowd_loader;
	std::string crowd_body_error;
	double crowd_body_seconds = 0;
#if OPENBF3_BUILD_CHECKS
	std::unique_ptr<M2aCheck> check;
	std::unique_ptr<NativeHudMomentRun> hud_moments;
	std::unique_ptr<CrosshairWalkRun> crosshair_walk;
#endif
	bool finished = false;
};

}

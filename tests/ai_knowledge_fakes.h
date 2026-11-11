#pragma once

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/knowledge/weapon_info.h"

#include <map>
#include <string>
#include <vector>

namespace bf3::ai::test {

struct FakeTimer final : AiTimer {
	sim::Tick tick = 600;
	mutable double clock = 0.0;
	double step = 1e-6;
	sim::Tick game_tick() const override { return tick; }
	double tick_seconds() const override { return 1.0 / 60.0; }
	double update_time() const override { return clock += step; }
};

struct FakeWeapon final : AiWeapon {
	int rounds = 30;
	std::string label = "M16A4";
	Transform transform;
	Vec3 where{};
	const Transform &absolute_transformation() const override { return transform; }
	const Vec3 &position() const override { return where; }
	const Transform &projectile_origin() const override { return transform; }
	int weapon_activate() const override { return 1; }
	int weapon_fire() const override { return 0; }
	int weapon_zoom() const override { return 0; }
	int weapon_alt_fire() const override { return 0; }
	bool is_zoomed() const override { return false; }
	int weapon_state() const override { return 0; }
	float drag() const override { return 0.0f; }
	float gravity_modifier() const override { return 1.0f; }
	float exit_velocity() const override { return 900.0f; }
	float min_range() const override { return 0.0f; }
	float max_range() const override { return 300.0f; }
	float optimal_range_percentage() const override { return 0.5f; }
	SoldierPose firing_pose() const override { return SoldierPose::Standing; }
	float allowed_deviation() const override { return 1.0f; }
	bool fires_through_transparent() const override { return true; }
	const int *strengths() const override { return nullptr; }
	int strength(int) const override { return 0; }
	float explosion_radius() const override { return 0.0f; }
	bool is_thrown() const override { return false; }
	bool use_aimer_only() const override { return false; }
	int fire_rate() const override { return 800; }
	int ammo() const override { return rounds; }
	int current_ammo() const override { return rounds; }
	int ammo_in_mag(int) const override { return rounds; }
	int max_ammo() const override { return 90; }
	int max_ammo_in_mag() const override { return 30; }
	int number_of_mags() const override { return 3; }
	bool has_auto_reload() const override { return true; }
	float reload_time() const override { return 2.0f; }
	int weapon_type() const override { return 0; }
	int weapon_guiding_system() const override { return 0; }
	sim::Tick time_of_firing() const override { return 0; }
	bool burst() const override { return false; }
	bool indirect() const override { return false; }
	float heat() const override { return 0.0f; }
	bool has_heat() const override { return false; }
	bool uses_heat() const override { return false; }
	bool is_ready_to_fire() const override { return true; }
	bool set_fake_fire(bool) override { return false; }
	float sound_sphere_radius() const override { return 60.0f; }
	int projectile_material() const override { return 0; }
	float replenish_radius() const override { return 0.0f; }
	void set_bot_skill(float, sim::Tick, float) override {}
	bool set_target(uint32_t) override { return false; }
	bool has_target_locked() const override { return false; }
	AiObject *locked_target() const override { return nullptr; }
	const std::string &name() const override { return label; }
};

struct FakeArmament final : AiObjectArmament {
	std::vector<AiWeapon *> list;
	int is_anti_aircraft() const override { return 0; }
	int active_weapon() const override { return 0; }
	void set_active_weapon(int, int) override {}
	AiWeapon *last_fired_weapon() const override { return list.empty() ? nullptr : list.front(); }
	const std::vector<AiWeapon *> &weapons() const override { return list; }
	int weapon_id_from_activation_id(int, int) const override { return 0; }
	bool rearm(AiWeapon *, float p_amount, int) override { return p_amount >= 1.0f; }
	bool projectile_position(Vec3 &) const override { return false; }
	bool projectile_speed(Vec3 &) const override { return false; }
};

struct FakePhysical final : AiObjectPhysical {
	float hp = 100.0f;
	float reach = 0.5f;
	bool down = false;
	float health() const override { return hp; }
	float max_health() const override { return 100.0f; }
	float mass() const override { return 80.0f; }
	float volume() const override { return 0.1f; }
	float armour(const Vec3 &) const override { return 1.0f; }
	float best_armour() const override { return 1.0f; }
	float radius() const override { return reach; }
	Vec3 sphere_local_offset() const override { return {}; }
	float smallest_radius() const override { return reach; }
	void sub_spheres_2d(std::vector<Vec2> &) const override {}
	int strength_type() const override { return 0; }
	bool is_touching_terrain(Vec3 &) const override { return true; }
	bool is_in_air() const override { return false; }
	bool is_in_water() const override { return false; }
	int material() const override { return 0; }
	float man_down_time_left() const override { return 0.0f; }
	float max_man_down_time() const override { return 10.0f; }
	bool man_down() const override { return down; }
};

struct FakeUnit final : AiObjectUnit {
	int battle[kBattleStrengthCount] = { 10, 20, 0, 0, 0, 0 };
	int strategic[kStrategicStrengthCount] = { 4, 8 };
	Vec3 entry{};
	int max_ammo() const override { return 180; }
	const int *battle_strengths() const override { return battle; }
	const int *strategic_strengths() const override { return strategic; }
	int equipment_type() const override { return 0; }
	bool use_no_pathfinding_to_get_to_object() const override { return false; }
	bool has_exposed_soldier() const override { return true; }
	bool always_target_soldier() const override { return false; }
	bool is_occupied() const override { return true; }
	float cull_distance() const override { return 100.0f; }
	float cull_distance_sqr() const override { return 10000.0f; }
	float entry_radius() const override { return 1.0f; }
	AiObject *occupier() const override { return nullptr; }
	AiObject *occupied_object() const override { return nullptr; }
	sim::Tick last_player_possession_tick() const override { return 0; }
	int team_lock() const override { return 0; }
	bool must_borrow_armament(const AiObject *) const override { return false; }
	int occupying_player_id() const override { return 3; }
	void set_missile_lock_status(int) override {}
	int missile_lock_status() const override { return 0; }
	const Vec3 &entry_position_of_non_mobile() const override { return entry; }
};

struct FakeMobile final : AiObjectMobile {
	Vec3 velocity{ 6.0f, 0.0f, 0.0f };
	Vec3 spin{};
	const Vec3 &speed() const override { return velocity; }
	const Vec3 &rotational_speed() const override { return spin; }
	int vehicle_number() const override { return 0; }
	float max_speed() const override { return 7.0f; }
	float turn_radius() const override { return 0.0f; }
	float drag() const override { return 0.0f; }
	float cover_search_radius() const override { return 20.0f; }
	bool is_turnable() const override { return true; }
	float sound_sphere_radius() const override { return 10.0f; }
	float sound_sphere_radius_min() const override { return 5.0f; }
	float sound_sphere_radius_max() const override { return 30.0f; }
	float hearing_probability_max() const override { return 1.0f; }
	float hearing_probability_min() const override { return 0.0f; }
	float avoid_collision_look_ahead() const override { return 2.0f; }
	AiObject *sector() const override { return nullptr; }
	int last_passed_portal() const override { return -1; }
	float sprint_stamina() const override { return 1.0f; }
};

struct FakeObject final : AiObject {
	std::string label = "soldier";
	FakePhysical body;
	FakeUnit squad_unit;
	FakeMobile legs;
	FakeArmament arms;
	FakeWeapon rifle;
	FakeWeapon pistol;
	bool with_unit = true;
	Vec3 where{};
	Transform transform;
	int team = 1;
	bool primary = false;
	std::map<int, uint32_t> handles;

	FakeObject() {
		pistol.rounds = 60;
		arms.list = { &rifle, &pistol };
	}
	const std::string &name() const override { return label; }
	const std::string &ai_name() const override { return label; }
	const std::string &object_type_name() const override { return label; }
	AiObjectArmament *armament() const override { return const_cast<FakeArmament *>(&arms); }
	AiObjectCover *cover() const override { return nullptr; }
	AiObjectMobile *mobile() const override { return const_cast<FakeMobile *>(&legs); }
	AiObjectPhysical *physical() const override { return const_cast<FakePhysical *>(&body); }
	AiObjectUnit *unit() const override { return with_unit ? const_cast<FakeUnit *>(&squad_unit) : nullptr; }
	uint32_t information(int p_side) const override {
		const auto found = handles.find(p_side);
		return found != handles.end() ? found->second : kNoHandle;
	}
	const Vec3 &position() const override { return where; }
	const Transform &transformation() const override { return transform; }
	uint32_t object_id() const override { return 7; }
	SoldierPose soldier_pose() const override { return SoldierPose::Crouching; }
	int kit_type() const override { return 0; }
	bool is_primary() const override { return primary; }
	AiObject *primary_object() const override { return nullptr; }
	AiObject *secondary_object(int &) const override { return nullptr; }
	AiObject *first_secondary_object(int &) const override { return nullptr; }
	int first_secondary_object_iterator() const override { return -1; }
	int side() const override { return team; }
	bool object_spawner_holding() const override { return false; }
	AiObject *object_spawner() const override { return nullptr; }
	bool is_root_object() const override { return true; }
	int player_id() const override { return 3; }
};

struct Knowledge {
	FakeTimer timer;
	InformationManager information;
	WeaponInfoManager weapons;
	InformationFactory factory{ make() };

	KnowledgeContext make() {
		KnowledgeContext context;
		context.information = &information;
		context.weapons = &weapons;
		context.timer = &timer;
		return context;
	}

	uint32_t soldier(FakeObject &r_object, int p_side, bool p_with_plug_ins = true) {
		const uint32_t handle = factory.request_information_real(kNoHandle, &r_object, 1u, 0.5f, timer.tick, 30);
		r_object.handles[p_side] = handle;
		if (p_with_plug_ins) {
			factory.request_information_plug_in(handle, PlugInRequest::Physical, true);
			factory.request_information_plug_in(handle, PlugInRequest::Armament, true);
			factory.request_information_plug_in(handle, PlugInRequest::Mobile, true);
			if (r_object.with_unit) {
				factory.request_information_plug_in(handle, PlugInRequest::Unit, true);
			}
		}
		return handle;
	}
};

}

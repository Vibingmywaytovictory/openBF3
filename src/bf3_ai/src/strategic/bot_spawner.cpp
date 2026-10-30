#include "bf3/ai/strategic/bot_spawner.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/information_grid.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/ai_random.h"
#include "bf3/ai/squad/squad_manager.h"
#include "bf3/ai/vehicle/vehicle_occupancy.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

namespace bf3::ai {

namespace {

constexpr double kDrawScale = 1.0 / 4294967296.0;
constexpr float kKeepSpawnPointDraw = 0.8f;
constexpr float kFollowSquadDraw = 0.3f;
constexpr float kFreeUnitRadius = 50.0f;
constexpr float kFreeStrengthWeight = 0.033f;
constexpr float kDistanceWeight = 0.1f;
constexpr float kAreaBoxScale = 3.0f;
constexpr float kInfantryPressure = 1.5f;
constexpr float kDamagedHealth = 0.5f;
constexpr uint32_t kFlagNoSpawnUnitA = 0x80u;
constexpr uint32_t kFlagNoSpawnUnitB = 0x200000u;

struct PressureKit {
	float draw;
	int kit;
};

constexpr std::array<PressureKit, 3> kPressureKits = { { { 0.85f, 0 }, { 0.55f, 2 }, { 0.25f, 3 } } };

Information *lookup(const InformationManager &p_information, uint32_t p_handle) {
	return (p_handle & kHandleIndexMask) != 0 ? p_information.information(p_handle) : nullptr;
}

class FreeUnitPredicate final : public InformationGridPredicate {
public:
	FreeUnitPredicate(const InformationManager &p_information, int p_side) :
			information_(p_information), side_(p_side) {}

	bool include_information(const Information &p_information) const override {
		if (p_information.unit() == nullptr || !p_information.has_mobile()) {
			return false;
		}
		const uint32_t flags = p_information.object_type_flags();
		if ((flags & kFlagNoSpawnUnitA) != 0 || (flags & kFlagNoSpawnUnitB) != 0) {
			return false;
		}
		bool any_free = false;
		float strength = 0.0f;
		const Information *current = &p_information;
		int iterator = p_information.first_secondary_object_iterator();
		while (current != nullptr) {
			if (current->unit() != nullptr && !current->unit()->is_occupied()) {
				any_free = true;
				strength += current->strength();
			}
			current = lookup(information_, p_information.secondary_object(iterator, side_));
		}
		if (any_free) {
			strength_ += strength;
		}
		return any_free;
	}

	void reset() { strength_ = 0.0f; }
	float strength() const { return strength_; }

private:
	const InformationManager &information_;
	int side_ = 0;
	mutable float strength_ = 0.0f;
};

float planar_distance_sq(Vec2 p_a, Vec2 p_b) {
	const float dx = p_a.x - p_b.x;
	const float dz = p_a.y - p_b.y;
	return dx * dx + dz * dz;
}

}

double BotSpawner::draw() {
	return services_.random != nullptr ? double(services_.random->next()) * kDrawScale : 0.0;
}

const SquadMember *BotSpawner::squad_leader_to_follow(const SquadMember *p_member) {
	const double follow = draw();
	const SquadMember *const leader = p_member != nullptr ? p_member->squad().squad_leader() : nullptr;
	if (leader != nullptr && (follow > kFollowSquadDraw || !leader->player().is_ai()) && !p_member->is_squad_leader() &&
			leader->player().is_alive() && !leader->player().is_man_down()) {
		return leader;
	}
	return nullptr;
}

bool BotSpawner::follows_squad(const SquadMember *p_member, float p_draw) {
	const double follow = draw();
	const SquadMember *const leader = p_member != nullptr ? p_member->squad().squad_leader() : nullptr;
	return p_member != nullptr && p_member->squad().number_of_squad_members() > 1 &&
			(follow > p_draw || (leader != nullptr && !leader->player().is_ai()));
}

const SquadMember *BotSpawner::squad_mate_to_follow(const SquadMember *p_member) const {
	if (p_member == nullptr) {
		return nullptr;
	}
	const SquadCoordinator &squad = p_member->squad();
	const SquadMember *const leader = squad.squad_leader();
	const auto ready = [&](const SquadMember *p_mate) {
		return p_mate != nullptr && p_mate != p_member && p_mate->player().is_alive() &&
				!p_mate->player().is_man_down();
	};
	if (ready(leader)) {
		return leader;
	}
	for (int i = 0; i < squad.number_of_squad_members(); ++i) {
		if (ready(squad.squad_member(i))) {
			return squad.squad_member(i);
		}
	}
	return nullptr;
}

bool BotSpawner::find_suitable_spawn_group(int p_strategic_object_id, SpawnBot &r_bot) {
	SpawnGame &game = *services_.game;
	const int side = r_bot.side();
	if (r_bot.player().spawn_group() > 0) {
		return true;
	}
	const SpawnObjective *object = nullptr;
	if (draw() > kKeepSpawnPointDraw) {
		object = game.strategic_object(p_strategic_object_id);
		if (game.is_eligible_spawnpoint(side, r_bot.spawn_point())) {
			game.add_spawn_to_point(side, r_bot.spawn_point());
			return true;
		}
	}
	const std::vector<int> &groups = game.spawn_point_list(side);
	if (groups.empty()) {
		r_bot.set_spawn_target(-1, kKitNone);
		return false;
	}
	int group = 0;
	if (object != nullptr) {
		const Vec2 target = object->position(side);
		float best = 1.0e15f;
		bool found = false;
		Vec3 position{};
		for (const int candidate : groups) {
			if (!game.is_eligible_spawnpoint(side, candidate)) {
				continue;
			}
			const Vec3 at = game.spawn_point_position(candidate);
			const float distance = planar_distance_sq({ at.x, at.z }, target);
			if (distance < best) {
				best = distance;
				group = candidate;
				found = true;
				position = at;
			}
		}
		if (!found) {
			r_bot.set_spawn_target(-1, kKitNone);
			return false;
		}
		game.add_spawn_to_point(side, group);
		object = game.closest_object(position, side);
	} else {
		group = find_suitable_spawn_group(side, object, r_bot);
	}
	const int kit = find_suitable_kit(object, side, r_bot);
	if (object != nullptr) {
		r_bot.set_spawn_target(object->index(), group, kit);
	} else {
		r_bot.set_spawn_target(group, kit);
	}
	return true;
}

int BotSpawner::find_suitable_spawn_group(int p_side, const SpawnObjective *&r_object, SpawnBot &r_bot) {
	SpawnGame &game = *services_.game;
	const std::vector<int> &groups = game.spawn_point_list(p_side);
	if (groups.empty()) {
		return 0;
	}
	const SquadMember *const member = r_bot.squad_member();
	Vec3 squad_position{};
	if (member != nullptr) {
		member->squad().squad_position(squad_position);
	}
	const Vec2 squad_xz{ squad_position.x, squad_position.z };
	point_weights_.clear();

	if (squad_leader_to_follow(member) != nullptr) {
		r_object = nullptr;
		float best = FLT_MAX;
		int selected = 0;
		bool found = false;
		for (const int candidate : groups) {
			if (!game.is_eligible_spawnpoint(p_side, candidate)) {
				continue;
			}
			const Vec3 position = game.spawn_point_position(candidate);
			const float distance = distance_sq(squad_position, position);
			if (distance < best && game.is_valid_position(position)) {
				best = distance;
				selected = candidate;
				found = true;
			}
		}
		if (found) {
			game.add_spawn_to_point(p_side, selected);
			return selected;
		}
	}

	const AIInformationGrid *const grid = game.information_grid(p_side);
	for (const int candidate : groups) {
		if (!game.is_eligible_spawnpoint(p_side, candidate) || game.control_point_id(candidate) == -1) {
			continue;
		}
		const Vec3 position = game.spawn_point_position(candidate);
		r_object = game.closest_object(position, p_side);
		if (r_object == nullptr) {
			continue;
		}
		SpawnChoice choice;
		choice.group = candidate;
		choice.strategic_value = r_object->strategic_value(p_side);
		if (grid != nullptr && services_.information != nullptr) {
			FreeUnitPredicate predicate(*services_.information, p_side);
			std::vector<uint32_t> information;
			grid->information_within_circle(information, { position.x, position.z }, kFreeUnitRadius, &predicate);
			choice.information_count = int(information.size());
			choice.free_strength = predicate.strength();
		}
		choice.object = r_object;
		choice.distance = std::sqrt(planar_distance_sq(squad_xz, r_object->position(p_side)));
		point_weights_.push_back(choice);
	}
	if (point_weights_.empty()) {
		return -1;
	}
	double total = 0.0;
	for (SpawnChoice &choice : point_weights_) {
		double weight = (double(choice.free_strength) * kFreeStrengthWeight + 1.0) * choice.strategic_value -
				double(choice.distance) * kDistanceWeight;
		if (weight < 1.0) {
			weight = 1.0;
		}
		choice.weight = float(weight);
		total += weight;
	}
	double remaining = draw() * total;
	auto selected = point_weights_.begin();
	for (; selected != point_weights_.end(); ++selected) {
		remaining -= selected->weight;
		if (remaining <= 0.0) {
			break;
		}
	}
	if (selected == point_weights_.end()) {
		selected = point_weights_.end() - 1;
	}
	game.add_spawn_to_point(p_side, selected->group);
	r_object = selected->object;
	return selected->group;
}

int BotSpawner::random_kit() {
	std::vector<int> choices;
	for (const int type : services_.game->kit_templates()) {
		if (type >= 0 && type != kKitMedic) {
			choices.push_back(type);
		}
	}
	if (choices.empty()) {
		return kKitNone;
	}
	const size_t index = std::min(size_t(draw() * double(choices.size())), choices.size() - 1);
	return choices[index];
}

int BotSpawner::find_suitable_kit(const SpawnObjective *p_object, int p_side, SpawnBot &r_bot) {
	random_kit();
	SpawnGame &game = *services_.game;
	const std::vector<int> &templates = game.kit_templates();
	auto has_template = [&templates](int p_kit) {
		return size_t(p_kit) < templates.size() && templates[size_t(p_kit)] >= 0;
	};
	const SquadPlayer &player = r_bot.player();
	const SquadCoordinator *const squad =
			services_.squads != nullptr ? services_.squads->squad_from_player(player.id(), p_side) : nullptr;
	const SquadMember *const member = r_bot.squad_member();
	if (squad != nullptr && member != nullptr && !member->is_squad_leader()) {
		if (!squad->has_member_with_kit(kKitMedic) || player.spawn_kit() == kKitMedic) {
			return kKitMedic;
		}
	}
	if (p_object == nullptr) {
		return random_kit();
	}

	const Vec2 centre = p_object->centre();
	const Vec2 position = p_object->position(p_side);
	const Vec2 far_corner{ centre.x - (position.x - centre.x) * kAreaBoxScale,
		centre.y - (position.y - centre.y) * kAreaBoxScale };
	const Vec2 box_min{ std::min(far_corner.x, position.x), std::min(far_corner.y, position.y) };
	const Vec2 box_max{ std::max(far_corner.x, position.x), std::max(far_corner.y, position.y) };
	std::vector<uint32_t> information;
	const AIInformationGrid *const grid = game.information_grid(p_side);
	if (grid != nullptr) {
		grid->information_within_rectangle(information, box_min, box_max, nullptr);
	}

	std::array<int, kKitTypeCount> kit_counts{};
	int enemy_vehicles = 0;
	int enemy_infantry = 0;
	int friendly_vehicles = 0;
	int friendly_infantry = 0;
	int damaged_friendly_vehicles = 0;
	const int enemy_side = (p_side == 1) + 1;
	for (const uint32_t handle : information) {
		const Information *const entry =
				services_.information != nullptr ? lookup(*services_.information, handle) : nullptr;
		const IPIPhysical *const physical = entry != nullptr ? entry->physical() : nullptr;
		if (physical == nullptr || !vehicle_occupied(*services_.information, handle, p_side)) {
			continue;
		}
		const int strength_type = physical->strength_type();
		const bool infantry = strength_type == 0;
		const bool vehicle = strength_type == 1 || strength_type == 2 || strength_type == 4;
		if (entry->side() == enemy_side) {
			enemy_infantry += infantry ? 1 : 0;
			enemy_vehicles += vehicle ? 1 : 0;
		} else if (infantry) {
			++friendly_infantry;
			const int kit = entry->kit_type();
			if (kit >= 0 && kit < kKitTypeCount) {
				++kit_counts[size_t(kit)];
			}
		} else if (vehicle) {
			++friendly_vehicles;
			if (physical->health() < kDamagedHealth) {
				++damaged_friendly_vehicles;
			}
		}
	}

	for (const auto &[bot_id, death] : game.dead_bots(p_side)) {
		if (uint32_t(bot_id) == r_bot.bot_id()) {
			continue;
		}
		const SpawnBot *const other = game.bot_from_id(uint32_t(bot_id));
		if (other != nullptr && other->spawn_to_strategic_area_id() == p_object->index() && death.second >= 0 &&
				death.second < kKitTypeCount) {
			++kit_counts[size_t(death.second)];
		}
	}

	if (enemy_vehicles > friendly_vehicles + kit_counts[kKitAntiTank] && has_template(kKitAntiTank)) {
		return kKitAntiTank;
	}
	if (float(enemy_infantry) > float(friendly_infantry) * kInfantryPressure) {
		const double pressure = draw();
		for (const PressureKit &choice : kPressureKits) {
			if (pressure > choice.draw && has_template(choice.kit)) {
				return choice.kit;
			}
		}
		if (has_template(kKitEngineer)) {
			return kKitEngineer;
		}
	} else if (damaged_friendly_vehicles > kit_counts[kKitEngineer] && has_template(kKitEngineer)) {
		return kKitEngineer;
	}
	return random_kit();
}

}

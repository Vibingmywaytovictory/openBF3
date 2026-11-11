#include "bf3/ai/strategic/sai.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/information_grid.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/ai_random.h"
#include "bf3/ai/main/ai_settings.h"
#include "bf3/ai/main/bot.h"
#include "bf3/ai/squad/squad_manager.h"
#include "bf3/ai/strategic/strategy.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::ai {

namespace {

constexpr AIStrategicObject::Type kAreaFlags[] = { AIStrategicObject::Flank, AIStrategicObject::Base,
	AIStrategicObject::Close, AIStrategicObject::Centre, AIStrategicObject::Remote };
constexpr AIStrategicObject::Type kOtherFlags[] = { AIStrategicObject::Route, AIStrategicObject::Bridge,
	AIStrategicObject::Route, AIStrategicObject::Bridge, AIStrategicObject::North, AIStrategicObject::West,
	AIStrategicObject::South, AIStrategicObject::East };
constexpr int kOtherIndices[] = { 27, 28, 27, 28, 29, 30, 31, 32 };

}

Sai::Sai(int p_side, const SaiServices &p_services) :
		services_(p_services), side_(p_side), side_mask_(1 << p_side), enemy_mask_(p_side == 1 ? 4 : 2) {
	clear_states(state_own_[0], state_enemy_[0]);
	clear_states(state_own_[1], state_enemy_[1]);
	const int bots = services_.settings != nullptr ? std::max(services_.settings->max_bots, 0) : 0;
	bots_.resize(size_t(bots));
	const double now = game_time();
	uav_ready_ = now + 10.0;
	artillery_ready_ = now + 60.0;
	supply_ready_ = now + 60.0;
	vehicle_drop_ready_ = now + 60.0;
}

void Sai::clear_states(std::array<float, kSaiStateCount> &r_own, std::array<float, kSaiStateCount> &r_enemy) {
	r_own.fill(0.0f);
	r_enemy.fill(0.0f);
	r_own[kSaiStateCount - 1] = 1.0f;
	r_enemy[kSaiStateCount - 1] = 1.0f;
}

double Sai::game_time() const {
	return services_.timer != nullptr ? double(services_.timer->game_tick()) * services_.timer->tick_seconds() : 0.0;
}

bool Sai::over_budget(double p_start, double p_budget) const {
	return services_.timer != nullptr && services_.timer->update_time() - p_start > p_budget * 0.95;
}

Sai::~Sai() = default;

float Sai::draw() const {
	return services_.random != nullptr ? float(double(services_.random->next()) * 2.3283064e-10) : 0.0f;
}

AIStrategicObject *Sai::object(int p_index) const {
	return services_.objects != nullptr ? services_.objects->strategic_object(p_index) : nullptr;
}

const Sai::BotInfo *Sai::bot_info(int p_bot_id) const {
	return p_bot_id >= 0 && size_t(p_bot_id) < bots_.size() ? bots_[size_t(p_bot_id)].get() : nullptr;
}

int Sai::bot_current_pos(int p_bot_id) const {
	const BotInfo *info = bot_info(p_bot_id);
	return info != nullptr ? info->occupied : kUnknownBotPosition;
}

int Sai::bot_ordered_pos(int p_bot_id) const {
	const BotInfo *info = bot_info(p_bot_id);
	return info != nullptr ? info->ordered : kUnknownBotOrderedPosition;
}

int Sai::bot_last_ordered_pos(int p_bot_id) const {
	const BotInfo *info = bot_info(p_bot_id);
	return info != nullptr ? info->last_ordered : -1;
}

AIStrategicObject *Sai::closest_object_of(const BotInfo &p_info) const {
	if (services_.information == nullptr || services_.objects == nullptr) {
		return nullptr;
	}
	const Information *information = services_.information->information(p_info.bot->current_equipment());
	if (information == nullptr || information->unit() == nullptr) {
		return nullptr;
	}
	return services_.objects->closest_object(information->position(), side_);
}

void Sai::set_as_dead(BotInfo &r_info) {
	r_info.ordered = -1;
	r_info.last_ordered = -1;
	r_info.occupied = -1;
	r_info.time_alive = 0.0f;
}

void Sai::respawn(BotInfo &r_info, AIStrategicObject &r_object) {
	r_info.occupied = r_object.index();
	r_object.set_as_present_unit(int(r_info.bot->bot_id()), side_);
	free_bots_.insert(int(r_info.bot->bot_id()));
}

void Sai::free_from(BotInfo &r_info, AIStrategicObject &r_object, bool p_attacker, std::set<int>::iterator &r_it) {
	const int id = int(r_info.bot->bot_id());
	if (p_attacker) {
		r_object.unregister_available_attacker(r_it, side_);
	} else {
		r_object.unregister_available_defender(r_it, side_);
	}
	assigned_bots_.erase(id);
	free_bots_.insert(id);
}

void Sai::free_bot(BotInfo &r_info, bool p_release) {
	const int id = int(r_info.bot->bot_id());
	AIStrategicObject *current = object(r_info.occupied);
	if (p_release) {
		if (current != nullptr && !current->unregister_available_attacker(id, side_)) {
			current->unregister_available_defender(id, side_);
		}
		assigned_bots_.erase(id);
	}
	free_bots_.insert(id);
}

void Sai::remove_assigned(BotInfo &r_info, AIStrategicObject &r_object, bool p_release) {
	std::set<int>::iterator it = r_object.assigned_units(side_).find(int(r_info.bot->bot_id()));
	remove_assigned(r_info, r_object, it, p_release);
}

void Sai::remove_assigned(BotInfo &r_info, AIStrategicObject &r_object, std::set<int>::iterator &r_it, bool p_release) {
	const int id = int(r_info.bot->bot_id());
	if (r_it != r_object.assigned_units(side_).end()) {
		r_it = r_object.assigned_units(side_).erase(r_it);
	}
	if (p_release) {
		assigned_bots_.erase(id);
	} else if (AIStrategicObject *current = object(r_info.occupied)) {
		if (current->is_attack(side_)) {
			current->register_available_attacker(id, side_);
		} else {
			current->register_available_defender(id, side_);
		}
	}
	r_info.ordered = -1;
	r_info.order_time = 0.0;
}

void Sai::occupy(BotInfo &r_info, AIStrategicObject &r_from, AIStrategicObject &r_to) {
	const int id = int(r_info.bot->bot_id());
	r_to.set_as_present_unit(id, side_);
	if (r_from.unregister_available_attacker(id, side_)) {
		r_to.register_available_attacker(id, side_);
	} else if (r_from.unregister_available_defender(id, side_)) {
		r_to.register_available_defender(id, side_);
	}
	r_from.remove_as_present_unit(id, side_);
	r_info.occupied = r_to.index();
}

void Sai::assign_to_object(
		BotInfo &r_info, std::set<int>::iterator &r_it, AIStrategicObject &r_object, bool p_attacker) {
	r_info.ordered = r_object.index();
	r_info.last_ordered = r_object.index();
	r_info.order_time = game_time();
	if (AIStrategicObject *current = object(r_info.occupied)) {
		if (p_attacker) {
			current->unregister_available_attacker(r_it, side_);
		} else {
			current->unregister_available_defender(r_it, side_);
		}
	}
	r_object.assigned_units(side_).insert(int(r_info.bot->bot_id()));
}

void Sai::remove(BotInfo &r_info) {
	const int id = int(r_info.bot->bot_id());
	AIStrategicObject *current = object(r_info.occupied);
	AIStrategicObject *assigned = object(r_info.ordered);
	if (assigned != nullptr) {
		remove_assigned(r_info, *assigned, true);
		if (current != nullptr) {
			current->remove_as_present_unit(id, side_);
		}
	} else if (current != nullptr) {
		if (assigned_bots_.contains(id)) {
			free_bot(r_info, true);
		}
		current->remove_as_present_unit(id, side_);
	}
	free_bots_.erase(id);
}

void Sai::add_bot(Bot *p_bot) {
	if (p_bot == nullptr) {
		return;
	}
	if (services_.squads != nullptr) {
		services_.squads->add_squad_member(p_bot->player_id(), side_);
	}
	added_.push_back(p_bot);
}

void Sai::remove_bot(Bot *p_bot) {
	if (p_bot == nullptr) {
		return;
	}
	added_.erase(std::remove(added_.begin(), added_.end(), p_bot), added_.end());
	if (services_.squads != nullptr) {
		services_.squads->remove_squad_member(p_bot->player_id(), side_);
	}
	const uint32_t id = p_bot->bot_id();
	if (id >= bots_.size() || bots_[id] == nullptr) {
		return;
	}
	if (dead_bots_.erase(int(id)) == 0) {
		remove(*bots_[id]);
	}
	bots_[id].reset();
}

void Sai::set_bot_is_dead(int p_bot_id) {
	handle_dead_bot(p_bot_id);
}

void Sai::handle_dead_bot(int p_bot_id) {
	BotInfo *info = p_bot_id >= 0 && size_t(p_bot_id) < bots_.size() ? bots_[size_t(p_bot_id)].get() : nullptr;
	if (info == nullptr) {
		return;
	}
	const int ordered = info->ordered;
	remove(*info);
	dead_bots_.insert({ p_bot_id, { ordered, kDeadBotKit } });
	set_as_dead(*info);
}

void Sai::set_bot_is_alive(int p_bot_id) {
	BotInfo *info = p_bot_id >= 0 && size_t(p_bot_id) < bots_.size() ? bots_[size_t(p_bot_id)].get() : nullptr;
	if (info == nullptr || info->bot->is_dead()) {
		return;
	}
	AIStrategicObject *closest = closest_object_of(*info);
	if (closest == nullptr) {
		return;
	}
	respawn(*info, *closest);
	dead_bots_.erase(p_bot_id);
}

void Sai::set_bot_has_changed_equipment(int p_bot_id) {
	handle_changing_bot(p_bot_id);
}

void Sai::handle_changing_bot(int p_bot_id) {
	BotInfo *info = p_bot_id >= 0 && size_t(p_bot_id) < bots_.size() ? bots_[size_t(p_bot_id)].get() : nullptr;
	if (info == nullptr) {
		return;
	}
	if (!free_bots_.contains(p_bot_id)) {
		if (AIStrategicObject *ordered = object(info->ordered)) {
			remove_assigned(*info, *ordered, true);
			free_bot(*info, false);
		} else if (info->occupied != -1) {
			free_bot(*info, true);
		}
	}
	info->order_time = game_time();
	info->last_ordered = -1;
	AIStrategicObject *closest = closest_object_of(*info);
	AIStrategicObject *current = object(info->occupied);
	if (closest == nullptr || closest == current) {
		return;
	}
	if (current == nullptr) {
		respawn(*info, *closest);
		return;
	}
	occupy(*info, *current, *closest);
}

bool Sai::release_unit_from_object(int p_object, std::set<int>::iterator &r_it) {
	AIStrategicObject *target = object(p_object);
	if (target == nullptr || r_it == target->assigned_units(side_).end()) {
		return false;
	}
	BotInfo *info = size_t(*r_it) < bots_.size() ? bots_[size_t(*r_it)].get() : nullptr;
	if (info == nullptr) {
		r_it = target->assigned_units(side_).erase(r_it);
		return false;
	}
	remove_assigned(*info, *target, r_it, true);
	free_bot(*info, false);
	return true;
}

int Sai::assign_bot(std::set<int>::iterator &r_it, int p_object, int p_bot_id, bool p_attacker) {
	BotInfo &info = *bots_[size_t(p_bot_id)];
	const int current = info.occupied;
	if (AIStrategicObject *target = object(p_object)) {
		assign_to_object(info, r_it, *target, p_attacker);
	}
	return current;
}

void Sai::add_strategy(const Strategy *p_strategy) {
	StrategyContainer container;
	container.strategy = p_strategy;
	strategies_.push_back(container);
	active_ = 0;
}

const Strategy *Sai::active_strategy() const {
	return active_ < strategies_.size() ? strategies_[active_].strategy : nullptr;
}

Vec2 Sai::strategy_heat(size_t p_index) const {
	if (p_index >= strategies_.size()) {
		return {};
	}
	return { strategies_[p_index].distributed_score, strategies_[p_index].score };
}

std::string Sai::list_strategies() const {
	std::string out;
	for (size_t i = 0; i < strategies_.size(); ++i) {
		if (strategies_[i].strategy != nullptr) {
			out += std::to_string(i) + " " + strategies_[i].strategy->name() + "\n";
		}
	}
	return out;
}

void Sai::choose_strategy() {
	if (strategies_.empty()) {
		return;
	}
	const float *own = state_own_[current_].data();
	const float *enemy = state_enemy_[current_].data();
	const Strategy *previous = active_strategy();
	const Strategy *forced = services_.strategies != nullptr ? services_.strategies->forced_strategy(side_) : nullptr;
	if (forced != nullptr) {
		for (size_t i = 0; i < strategies_.size(); ++i) {
			if (strategies_[i].strategy == forced) {
				change_strategy(i);
			}
		}
		return;
	}
	bool changed = false;
	for (StrategyContainer &container : strategies_) {
		container.score = std::max(container.strategy->calculate_prerequisite(own, enemy, previous, side_), 0.0f);
		float ratio = 0.0f;
		if (container.score != 0.0f) {
			ratio = container.distributed_score / container.score;
		} else {
			ratio = container.distributed_score == 0.0f ? 1.0f : 0.0f;
		}
		if (ratio <= 0.83 || ratio >= 1.2) {
			changed = true;
		}
	}
	if (!changed) {
		StrategyContainer &active = strategies_[active_];
		const float rounds = std::pow(0.5f, float(active.rounds));
		const float half = 0.5f - rounds;
		const double threshold = ((half + half) + 1.0f) * active.strategy->minimum_active_time();
		if (!(game_time() - active.start_time >= threshold)) {
			return;
		}
		++active.rounds;
	}
	float total = 0.0f;
	for (StrategyContainer &container : strategies_) {
		container.distributed_score = container.score;
		total += container.score > 0.0f ? container.score : 0.0f;
	}
	float pick = draw() * total;
	size_t chosen = 0;
	for (; chosen < strategies_.size(); ++chosen) {
		const float score = strategies_[chosen].distributed_score;
		pick -= score > 0.0f ? score : 0.0f;
		if (0.0 >= pick) {
			break;
		}
	}
	change_strategy(std::min(chosen, strategies_.size() - 1));
}

void Sai::change_strategy(size_t p_index) {
	if (p_index >= strategies_.size() || p_index == active_) {
		return;
	}
	active_ = p_index;
	if (services_.objects != nullptr) {
		for (AIStrategicObject *each : services_.objects->strategic_objects()) {
			if (!player_commander_) {
				release_squads_from_object(each->index());
			}
			for (const bool attacker : { true, false }) {
				std::set<int> &units = each->available_bots(attacker, side_);
				for (auto it = units.begin(); it != units.end();) {
					BotInfo *info = size_t(*it) < bots_.size() ? bots_[size_t(*it)].get() : nullptr;
					if (info == nullptr) {
						it = units.erase(it);
						continue;
					}
					free_from(*info, *each, attacker, it);
				}
			}
		}
	}
	strategies_[active_].rounds = 1;
	strategies_[active_].start_time = float(game_time());
}

void Sai::modify_strategic_objects_temperatures() {
	const Strategy *strategy = active_strategy();
	if (strategy == nullptr || services_.objects == nullptr) {
		return;
	}
	for (AIStrategicObject *each : services_.objects->strategic_objects()) {
		each->update_temperature(*strategy, side_);
	}
}

int Sai::release_squads_from_object(int p_object) {
	if (services_.decisions != nullptr) {
		return services_.decisions->release_squads_from_object(*this, p_object, side_);
	}
	int released = 0;
	if (services_.squads != nullptr) {
		for (int q = 1; q <= services_.squads->coordinator_count(side_); ++q) {
			SquadCoordinator &squad = services_.squads->coordinator(side_, q);
			const SquadArea *area = squad.squad_strategic_area_assignment();
			if (area != nullptr && area->index() == p_object) {
				squad.un_assign_squad();
				++released;
			}
		}
	}
	return released;
}

void Sai::deny(SaiDenial p_denial, int p_player_id, double p_now) {
	deny_requests_.push_back({ p_denial, p_now + 2.0f + double(draw()) * 3.0f, p_player_id });
}

bool Sai::player_requests_artillery(int p_player_id, const Vec3 &p_position) {
	if (player_commander_ || services_.commander == nullptr) {
		return false;
	}
	const double now = game_time();
	Commander &commander = *services_.commander;
	if (!commander.game_has_artillery(side_) || !commander.any_artillery_pieces_left(side_)) {
		deny(SaiDenial::ArtilleryUnavailable, p_player_id, now);
	} else if (now > artillery_ready_) {
		artillery_ready_ = now + 60.0f;
		commander.fire_artillery(side_, p_player_id, { p_position.x, p_position.z });
	} else {
		deny(SaiDenial::Artillery, p_player_id, now);
	}
	return false;
}

bool Sai::player_requests_uav(int p_player_id, const Vec3 &p_position) {
	if (player_commander_ || services_.commander == nullptr) {
		return false;
	}
	const double now = game_time();
	if (now > uav_ready_) {
		uav_ready_ = now + 30.0f;
		services_.commander->send_uav(side_, p_player_id, { p_position.x, p_position.z });
	} else {
		deny(SaiDenial::Uav, p_player_id, now);
	}
	return false;
}

bool Sai::player_requests_supply(int p_player_id, const Vec3 &p_position) {
	if (player_commander_ || services_.commander == nullptr) {
		return false;
	}
	const double now = game_time();
	if (now > supply_ready_) {
		supply_ready_ = now + 60.0f;
		services_.commander->drop_supply(side_, p_player_id, { p_position.x, p_position.z });
	} else {
		deny(SaiDenial::Supply, p_player_id, now);
	}
	return false;
}

bool Sai::player_requests_vehicle_drop(int p_player_id, const Vec3 &p_position) {
	if (player_commander_ || services_.commander == nullptr) {
		return false;
	}
	const double now = game_time();
	if (now > vehicle_drop_ready_) {
		vehicle_drop_ready_ = now + 60.0f;
		services_.commander->drop_vehicle(side_, p_player_id, { p_position.x, p_position.z });
	} else {
		deny(SaiDenial::VehicleDrop, p_player_id, now);
	}
	return false;
}

void Sai::update_request_deny_timers() {
	if (player_commander_ || services_.commander == nullptr) {
		deny_requests_.clear();
		return;
	}
	const double now = game_time();
	Commander &commander = *services_.commander;
	for (auto it = deny_requests_.begin(); it != deny_requests_.end();) {
		if (!(now > it->deadline)) {
			++it;
			continue;
		}
		switch (it->denial) {
			case SaiDenial::Supply:
				commander.deny_supply_request(side_, it->player_id);
				break;
			case SaiDenial::Artillery:
				commander.deny_artillery_request(side_, it->player_id);
				break;
			case SaiDenial::ArtilleryUnavailable:
				commander.artillery_unavailable(side_, it->player_id);
				break;
			case SaiDenial::Uav:
				commander.deny_uav_request(side_, it->player_id);
				break;
			case SaiDenial::VehicleDrop:
				commander.deny_vehicle_drop_request(side_, it->player_id);
				break;
		}
		it = deny_requests_.erase(it);
	}
}

void Sai::set_player_is_commander(bool p_commander) {
	if (!p_commander && player_commander_) {
		if (services_.squads != nullptr) {
			for (int q = 1; q <= services_.squads->coordinator_count(side_); ++q) {
				services_.squads->coordinator(side_, q).un_assign_squad();
			}
		}
		const double now = game_time();
		artillery_ready_ = now + 60.0f;
		supply_ready_ = now + 60.0f;
		vehicle_drop_ready_ = now + 60.0f;
		uav_ready_ = now + 10.0f;
	}
	player_commander_ = p_commander;
}

bool Sai::hostile_takeover_in_progress(AIStrategicObject *&r_base, Vec2 &r_position) const {
	if (services_.objects == nullptr) {
		return false;
	}
	for (AIStrategicObject *each : services_.objects->strategic_objects()) {
		if (each->type() != 0 || !each->has_object_type_flag(AIStrategicObject::ControlPoint) ||
				!each->is_enemy_taking_over_base(side_) || !(0.75f > each->control_point_take_over_counter())) {
			continue;
		}
		r_base = each;
		const Vec2 &corner = static_cast<const AIStrategicArea *>(each)->centre();
		const Vec2 &centre = each->position(side_);
		const double u = draw();
		r_position.x = float(corner.x + (double(centre.x) - corner.x) * 2.0 * u);
		const double v = draw();
		r_position.y = float(corner.y + (double(centre.y) - corner.y) * 2.0 * v);
		return true;
	}
	return false;
}

bool Sai::is_pos_inside_owned_control_point(
		AIStrategicObject *&r_object, const Vec3 &p_position, float p_max_radius) const {
	AIStrategicObject *closest =
			services_.objects != nullptr ? services_.objects->closest_object(p_position, side_) : nullptr;
	if (closest == nullptr || closest->type() != 0 || !closest->has_object_type_flag(AIStrategicObject::ControlPoint) ||
			closest->status(side_) != StrategicStatus::Owned) {
		return false;
	}
	Vec3 centre{};
	float radius = 0.0f;
	if (!static_cast<const AIStrategicArea *>(closest)->control_point_position_and_radius(centre, radius)) {
		return false;
	}
	if (p_max_radius > 0.0f && p_max_radius > radius) {
		radius = p_max_radius;
	}
	if (!(radius * radius > distance_sq(centre, p_position))) {
		return false;
	}
	r_object = closest;
	return true;
}

void Sai::set_new_bot_kit(int p_bot_id, int p_kit) {
	const auto found = dead_bots_.find(p_bot_id);
	if (found != dead_bots_.end()) {
		found->second.second = p_kit;
	}
}

void Sai::add_spawn_to_point(int p_point) {
	const auto inserted = spawns_to_points_.insert({ p_point, 1 });
	if (!inserted.second) {
		++inserted.first->second;
	}
}

int Sai::spawns_to_point(int p_point) const {
	const auto found = spawns_to_points_.find(p_point);
	return found == spawns_to_points_.end() ? 0 : found->second;
}

bool Sai::is_player_commander(int p_side) const {
	return p_side == side_ && player_commander_;
}

bool Sai::have_any_owned_control_points(int p_side) const {
	if (services_.objects == nullptr) {
		return false;
	}
	for (const AIStrategicObject *object : services_.objects->strategic_objects()) {
		const auto *area = object->type() == 0 ? static_cast<const AIStrategicArea *>(object) : nullptr;
		if (area != nullptr && area->control_point() != kNoControlPoint &&
				area->status(p_side) == StrategicStatus::Owned) {
			return true;
		}
	}
	return false;
}

bool Sai::have_any_owned_and_takeable_control_points(int p_side) const {
	if (services_.objects == nullptr) {
		return false;
	}
	for (const AIStrategicObject *object : services_.objects->strategic_objects()) {
		const auto *area = object->type() == 0 ? static_cast<const AIStrategicArea *>(object) : nullptr;
		if (area != nullptr && area->is_area_takeable() && area->status(p_side) == StrategicStatus::Owned) {
			return true;
		}
	}
	return false;
}

bool Sai::own_all_takeable_control_points(int p_side) const {
	if (services_.objects == nullptr) {
		return false;
	}
	bool any = false;
	for (const AIStrategicObject *object : services_.objects->strategic_objects()) {
		const auto *area = object->type() == 0 ? static_cast<const AIStrategicArea *>(object) : nullptr;
		if (area == nullptr || !area->is_area_takeable()) {
			continue;
		}
		any = true;
		if (area->status(p_side) != StrategicStatus::Owned) {
			return false;
		}
	}
	return any;
}

void Sai::update(double p_budget) {
	const double start = services_.timer != nullptr ? services_.timer->update_time() : 0.0;
	update_request_deny_timers();
	const float increase = services_.timer != nullptr ? float(services_.timer->tick_seconds()) : 0.0f;
	for (const std::unique_ptr<BotInfo> &info : bots_) {
		if (info != nullptr) {
			info->time_alive += increase;
		}
	}
	SaiDecisions *decide = player_commander_ ? nullptr : services_.decisions;
	switch (stage_) {
		case SaiStage::Done:
			stage_ = SaiStage::Start;
			[[fallthrough]];
		case SaiStage::Start:
			stage_ = SaiStage::Strengths;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Strengths:
			update_strengths();
			stage_ = SaiStage::ClearStates;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::ClearStates:
			clear_states(state_own_[current_ ^ 1u], state_enemy_[current_ ^ 1u]);
			stage_ = SaiStage::UpdateStates;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::UpdateStates:
			update_states(state_own_[current_ ^ 1u], state_enemy_[current_ ^ 1u]);
			current_ ^= 1u;
			stage_ = SaiStage::Prepare;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Prepare: {
			const double frequency = services_.settings != nullptr ? services_.settings->sai_update_frequency : 35.0;
			if (!(game_time() - last_update_ > frequency)) {
				return;
			}
			prepare_update();
			stage_ = SaiStage::StrategicObjects;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		}
		case SaiStage::StrategicObjects:
			if (!player_commander_ && !update_strategic_objects()) {
				return;
			}
			stage_ = SaiStage::ChooseStrategy;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::ChooseStrategy:
			if (decide != nullptr) {
				decide->choose_strategy(*this);
			} else if (!player_commander_) {
				choose_strategy();
			}
			stage_ = SaiStage::Temperatures;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Temperatures:
			if (services_.decisions != nullptr) {
				services_.decisions->modify_strategic_objects_temperatures(*this);
			} else {
				modify_strategic_objects_temperatures();
			}
			stage_ = SaiStage::BotPositions;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::BotPositions:
			if (services_.decisions != nullptr) {
				services_.decisions->update_bot_positions(*this);
			}
			stage_ = SaiStage::Distribution;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Distribution:
			if (decide != nullptr) {
				decide->update_distribution(*this);
			} else if (services_.decisions == nullptr && !player_commander_) {
				update_distribution();
			}
			stage_ = SaiStage::Resources;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Resources:
			if (decide != nullptr) {
				decide->distribute_resources(*this);
			} else if (services_.decisions == nullptr && !player_commander_) {
				distribute_resources();
			}
			if (!have_any_owned_control_points(side_)) {
				stage_ = SaiStage::NoAreas;
				return;
			}
			stage_ = SaiStage::Defence;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Defence:
			if (decide != nullptr) {
				decide->distribute_defence(*this);
			} else if (services_.decisions == nullptr && !player_commander_) {
				distribute_defence();
			}
			stage_ = SaiStage::Attack;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Attack:
			if (decide != nullptr) {
				decide->distribute_attack(*this);
			} else if (services_.decisions == nullptr && !player_commander_) {
				distribute_attack();
			}
			stage_ = SaiStage::Artillery;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Artillery:
			stage_ = SaiStage::Done;
			++completed_;
			return;
		case SaiStage::NoAreas:
			if (decide != nullptr) {
				decide->distribute_with_no_areas(*this);
			} else if (services_.decisions == nullptr && !player_commander_) {
				distribute_with_no_areas();
			}
			stage_ = SaiStage::Done;
			++completed_;
			return;
	}
}

void Sai::update_strengths() {
	if (services_.grid == nullptr || services_.information == nullptr) {
		return;
	}
	std::vector<uint32_t> handles;
	const UnitPredicate units;
	services_.grid->all_information(handles, &units);
	for (const uint32_t handle : handles) {
		const Information *root = services_.information->information(handle);
		if (root == nullptr) {
			continue;
		}
		const bool friendly = root->side() == side_;
		std::array<float, 6> &strengths = friendly ? friendly_strengths_ : enemy_strengths_;
		std::array<float, 6> &counts = friendly ? friendly_counts_ : enemy_counts_;
		int secondary = root->first_secondary_object_iterator();
		const Information *current = root;
		while (current != nullptr) {
			const IPIUnit *unit = current->unit();
			if (unit != nullptr && unit->is_occupied()) {
				const float security = current->security();
				for (size_t category = 0; category < strengths.size(); ++category) {
					strengths[category] = security * unit->battle_strengths()[category] + strengths[category];
				}
				const IPIPhysical *physical = current->physical();
				const int category = physical != nullptr ? physical->strength_type() : 0;
				if (category >= 0 && category < int(counts.size())) {
					counts[size_t(category)] += security;
				}
			}
			current = services_.information->information(root->secondary_object(secondary, side_));
		}
	}
	for (size_t category = 0; category < 6; ++category) {
		for (std::array<float, 6> *values :
				{ &friendly_strengths_, &friendly_counts_, &enemy_strengths_, &enemy_counts_ }) {
			if ((*values)[category] > 0.001f) {
				(*values)[category] *= 0.5f;
			}
		}
	}
}

void Sai::update_states(std::array<float, kSaiStateCount> &r_own, std::array<float, kSaiStateCount> &r_enemy) {
	const int enemy = side_ == 1 ? 2 : 1;
	if (services_.game != nullptr) {
		r_own[13] = float(services_.game->tickets(side_));
		r_enemy[13] = float(services_.game->tickets(enemy));
		r_own[14] = float(services_.game->number_of_control_points(side_));
		r_enemy[14] = float(services_.game->number_of_control_points(enemy));
	}
	const float now = float(game_time());
	r_own[15] = now;
	r_enemy[15] = now;
	if (services_.objects == nullptr) {
		return;
	}
	int attack = 0;
	int defence = 0;
	std::array<float, kSaiStateCount> *selected = nullptr;
	bool neighbour = false;
	for (const AIStrategicObject *object : services_.objects->strategic_objects()) {
		if (object->type() == 0) {
			switch (object->status(side_)) {
				case StrategicStatus::Owned:
					r_own[19] += 1.0f;
					r_enemy[21] += 1.0f;
					selected = &r_own;
					neighbour = object->has_neighbour_with_status(StrategicStatus::Hostile, side_);
					(neighbour ? r_own[39] : r_own[40]) += 1.0f;
					(neighbour ? r_enemy[41] : r_enemy[42]) += 1.0f;
					break;
				case StrategicStatus::Hostile:
					r_own[21] += 1.0f;
					r_enemy[19] += 1.0f;
					selected = &r_enemy;
					neighbour = object->has_neighbour_with_status(StrategicStatus::Owned, side_);
					(neighbour ? r_enemy[39] : r_enemy[40]) += 1.0f;
					(neighbour ? r_own[41] : r_own[42]) += 1.0f;
					break;
				case StrategicStatus::Neutral:
					r_own[20] += 1.0f;
					r_enemy[20] += 1.0f;
					if (object->has_neighbouring_object_with_status(StrategicStatus::Owned, side_)) {
						r_own[38] += 1.0f;
					}
					if (object->has_neighbouring_object_with_status(StrategicStatus::Hostile, side_)) {
						r_enemy[38] += 1.0f;
					}
					selected = nullptr;
					break;
				case StrategicStatus::Unknown:
					selected = nullptr;
					neighbour = false;
					break;
			}
		}
		if (object->number_of_assigned_units(side_) > 0) {
			++(object->is_attack(side_) ? attack : defence);
		}
		if (selected == nullptr) {
			continue;
		}
		for (size_t i = 0; i < std::size(kAreaFlags); ++i) {
			if (object->has_object_type_flag(kAreaFlags[i])) {
				(*selected)[22 + i] += 1.0f;
				if (neighbour) {
					(*selected)[33 + i] += 1.0f;
				}
			}
		}
		for (size_t i = 0; i < std::size(kOtherFlags); ++i) {
			if (object->has_object_type_flag(kOtherFlags[i])) {
				(*selected)[size_t(kOtherIndices[i])] += 1.0f;
			}
		}
	}
	r_enemy[17] = float(attack);
	r_enemy[18] = float(defence);
}

void Sai::prepare_update() {
	for (Bot *bot : added_) {
		const uint32_t id = bot->bot_id();
		if (id >= bots_.size()) {
			continue;
		}
		bots_[id] = std::make_unique<BotInfo>(bot);
		if (bot->is_dead()) {
			dead_bots_.insert({ int(id), { -1, kDeadBotKit } });
		} else {
			set_bot_is_alive(int(id));
		}
	}
	added_.clear();
	for (auto &[point, count] : spawns_to_points_) {
		count = 0;
	}
	friendly_strengths_.fill(0.0f);
	friendly_counts_.fill(0.0f);
	if (services_.information != nullptr) {
		for (const std::unique_ptr<BotInfo> &info : bots_) {
			if (info == nullptr || info->bot->is_dead()) {
				continue;
			}
			const Information *information = services_.information->information(info->bot->current_equipment());
			if (information == nullptr || information->unit() == nullptr) {
				continue;
			}
			for (size_t i = 0; i < 6; ++i) {
				friendly_strengths_[i] = information->unit()->battle_strengths()[i];
			}
			const int category = information->physical() != nullptr ? information->physical()->strength_type() : 0;
			if (category >= 0 && category < 6) {
				friendly_counts_[size_t(category)] += 1.0f;
			}
		}
	}
	for (auto it = dead_bots_.begin(); it != dead_bots_.end();) {
		if (it->first < 0 || size_t(it->first) >= bots_.size() || bots_[size_t(it->first)] == nullptr) {
			it = dead_bots_.erase(it);
		} else {
			++it;
		}
	}
}

bool Sai::update_strategic_objects() {
	if (services_.objects == nullptr || services_.objects->strategic_objects().empty()) {
		return true;
	}
	const std::vector<AIStrategicObject *> &objects = services_.objects->strategic_objects();
	if (objects_pass_start_) {
		objects_changed_ = false;
		objects_pass_start_ = false;
		object_cursor_ = 0;
		enemy_stats_.clear();
		friendly_stats_.clear();
	}
	bool changed = false;
	AIStrategicObject *object = objects[object_cursor_];
	object->update(side_, *this, changed);
	objects_changed_ = objects_changed_ || changed;
	if (object->status(side_) == StrategicStatus::Owned) {
		enemy_stats_.add(object->battle_statistics(side_));
	} else {
		friendly_stats_.add(object->battle_statistics(side_));
	}
	++object_cursor_;
	if (object_cursor_ != objects.size()) {
		return false;
	}
	if (objects_changed_) {
		for (AIStrategicObject *each : objects) {
			each->set_as_dirty(side_);
			if (!player_commander_) {
				release_squads_from_object(each->index());
			}
		}
	}
	objects_pass_start_ = true;
	return true;
}

}

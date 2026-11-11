#include "bf3/ai/main/communication_hub.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/main/bot_manager.h"
#include "bf3/ai/strategic/strategic_object.h"

#include <cfloat>
#include <cmath>

namespace bf3::ai {

namespace {

inline constexpr double kNever = -1000.0;
inline constexpr float kPickupPeriod = 20.0f;
inline constexpr float kSpotReportPeriod = 60.0f;
inline constexpr float kLargeSpotRangeSq = 10000.0f;
inline constexpr float kSpotRangeSq = 900.0f;
inline constexpr float kRightAngle = 1.5707963705062866f;

}

CommunicationHub::CommunicationHub(const CommunicationHubServices &p_services) : services_(p_services) {}

void CommunicationHub::reset() {
	last_pickup_send_time_ = { kNever, kNever };
	last_large_report_time_ = kNever;
	const size_t areas = services_.objects != nullptr ? services_.objects->strategic_areas().size() : 0;
	if (combat_strength_[0].empty()) {
		for (size_t i = 0; i < areas; ++i) {
			combat_strength_[0].emplace_back(kCombatStrengthNone, kNever);
			combat_strength_[1].emplace_back(kCombatStrengthNone, kNever);
		}
		return;
	}
	for (size_t i = 0; i < areas && i < combat_strength_[0].size(); ++i) {
		combat_strength_[0][i] = { kCombatStrengthNone, kNever };
		combat_strength_[1][i] = { kCombatStrengthNone, kNever };
	}
}

void CommunicationHub::enemy_spotted_by_ai(int p_player_id, AiObject &r_object, int p_side) {
	if (services_.information == nullptr || services_.objects == nullptr || p_side < 1 || p_side > 2) {
		return;
	}
	const Information *const information = services_.information->information(r_object.information(p_side));
	if (information == nullptr || !information->is_primary()) {
		return;
	}
	const IPIPhysical *const physical = information->physical();
	if (physical == nullptr) {
		return;
	}
	const int strength_type = physical->strength_type();
	const bool large_type = uint32_t(strength_type - 4) <= 1u;
	const float range_sq = large_type ? kLargeSpotRangeSq : kSpotRangeSq;
	const double now = game_time(services_.timer);
	std::vector<std::pair<int, double>> &strengths = combat_strength_[size_t(p_side - 1)];
	const auto &areas = services_.objects->strategic_areas();
	size_t index = 0;
	for (auto it = areas.begin(); it != areas.end(); ++it, ++index) {
		const Vec3 &position = r_object.position();
		const Vec2 &centre = it->second->centre();
		const float dx = position.x - centre.x;
		const float dy = position.z - centre.y;
		if (!(range_sq > dx * dx + dy * dy)) {
			continue;
		}
		if (large_type && now - last_large_report_time_ > kSpotReportPeriod) {
			last_large_report_time_ = now;
			break;
		}
		if (index >= strengths.size()) {
			continue;
		}
		std::pair<int, double> &entry = strengths[index];
		if (now - entry.second > kSpotReportPeriod) {
			entry = { strength_type, now };
			break;
		}
		if ((entry.first == 0 || entry.first == 1) && strength_type == 2) {
			entry = { 2, now };
			break;
		}
	}
	if (index == areas.size()) {
		return;
	}
	if (services_.game != nullptr) {
		services_.game->enemy_spotted_by_ai(p_player_id, r_object);
	}
}

void CommunicationHub::enemy_spotted_by_human(AiObject &r_object, int p_side) const {
	if (services_.information == nullptr) {
		return;
	}
	Information *information = services_.information->information(r_object.information(p_side));
	if (information == nullptr && services_.game != nullptr) {
		information = services_.information->information(services_.game->force_information(r_object, p_side, true));
	}
	if (information != nullptr && services_.timer != nullptr) {
		information->set_time(services_.timer->game_tick());
	}
}

void CommunicationHub::send_radio_message(int p_sender, int p_side, const RadioMessage &p_message) {
	const double now = game_time(services_.timer);
	if (p_message.type == kRadioRequestPickup && p_side >= 1 && p_side <= 2) {
		double &last = last_pickup_send_time_[size_t(p_side - 1)];
		if (!(kPickupPeriod > now - last)) {
			last = game_time(services_.timer);
		} else {
			if (services_.game == nullptr || !services_.game->player_is_ai(p_message.receiver) ||
					services_.voices == nullptr) {
				return;
			}
			RadioMessageSettings settings;
			services_.voices->settings(kRadioRequestPickup, settings);
			filter_and_receive_ai_radio_message(settings, p_sender, p_message.receiver, true);
			return;
		}
	}
	if (services_.radio != nullptr) {
		services_.radio->send_radio_message(p_sender, p_side, p_message);
	}
}

void CommunicationHub::filter_and_receive_ai_radio_message(
		const RadioMessageSettings &p_settings, int p_sender, int p_receiver, bool p_unconfirmable) {
	Bot *const bot = services_.bots != nullptr ? services_.bots->bot_from_player_id(p_receiver) : nullptr;
	if (bot == nullptr || p_sender == bot->player_id()) {
		return;
	}
	AiGame *const game = services_.game;
	const bool human_sender = game != nullptr && game->is_valid_player_id(p_sender) && !game->player_is_ai(p_sender);
	if (p_settings.type == kRadioRequestPickup && human_sender && services_.information != nullptr) {
		Transform camera;
		const Information *const equipment = services_.information->information(bot->current_equipment());
		if (equipment != nullptr && game->player_camera(p_sender, camera)) {
			const Vec3 &target = equipment->position();
			Vec3 direction{ target.x - camera.trans.x, target.y - camera.trans.y, target.z - camera.trans.z };
			const double length_sq = double(direction.x) * direction.x + double(direction.y) * direction.y +
					double(direction.z) * direction.z;
			if (!(std::fabs(length_sq - 1.0f) < FLT_EPSILON)) {
				if (!(std::fabs(float(length_sq)) < FLT_EPSILON)) {
					const double scale = 1.0f / std::sqrt(length_sq);
					direction = { float(direction.x * scale), float(direction.y * scale), float(direction.z * scale) };
				} else {
					direction = {};
				}
			}
			double cosine = double(camera.forward.x) * direction.x + double(camera.forward.y) * direction.y +
					double(camera.forward.z) * direction.z;
			if (-1.0f > cosine) {
				cosine = -1.0f;
			} else if (cosine > 1.0f) {
				cosine = 1.0f;
			}
			if (std::acos(float(cosine)) > kRightAngle) {
				return;
			}
		}
	}
	if (CommunicationAgent *const agent = bot->communication()) {
		agent->receive_radio_message(p_settings, p_sender, p_unconfirmable, game_time(services_.timer));
	}
}

}

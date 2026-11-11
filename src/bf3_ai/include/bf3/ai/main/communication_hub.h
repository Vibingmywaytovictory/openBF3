#pragma once

#include "bf3/ai/bot/communication_agent.h"
#include "bf3/ai/knowledge/ai_object.h"

#include <array>
#include <utility>
#include <vector>

namespace bf3::ai {

class AIStrategicObjectManager;
class AiGame;
class AiTimer;
class BotManager;
class InformationManager;

inline constexpr int kRadioRequestPickup = 2;
inline constexpr int kCombatStrengthNone = 6;

struct CommunicationHubServices {
	RadioComm *radio = nullptr;
	const RadioVoices *voices = nullptr;
	const AiTimer *timer = nullptr;
	AiGame *game = nullptr;
	const BotManager *bots = nullptr;
	const InformationManager *information = nullptr;
	const AIStrategicObjectManager *objects = nullptr;
};

class CommunicationHub final : public RadioComm {
public:
	explicit CommunicationHub(const CommunicationHubServices &p_services);

	void reset();
	void enemy_spotted_by_ai(int p_player_id, AiObject &r_object, int p_side);
	void enemy_spotted_by_human(AiObject &r_object, int p_side) const;
	void send_radio_message(int p_sender, int p_side, const RadioMessage &p_message) override;
	void filter_and_receive_ai_radio_message(
			const RadioMessageSettings &p_settings, int p_sender, int p_receiver, bool p_unconfirmable);

	double last_pickup_send_time(int p_side) const { return last_pickup_send_time_[size_t(p_side - 1)]; }
	const std::vector<std::pair<int, double>> &combat_strengths(int p_side) const {
		return combat_strength_[size_t(p_side - 1)];
	}
	double last_large_report_time() const { return last_large_report_time_; }

private:
	double game_time() const;

	CommunicationHubServices services_;
	std::array<double, 2> last_pickup_send_time_{ -1000.0, -1000.0 };
	std::array<std::vector<std::pair<int, double>>, 2> combat_strength_;
	double last_large_report_time_ = -1000.0;
};

}

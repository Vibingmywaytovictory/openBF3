#pragma once

#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/behaviour/urgency_merger.h"
#include "bf3/ai/bot/infantry.h"
#include "bf3/core/util/key_values.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bf3::ai {

class BotScript;
struct SquadGoal;

enum class InfantryBehaviour : uint8_t { Revive, FireInfantery, TakeCoverInfantry, Avoid, MoveTo, Random, Idle };
inline constexpr size_t kInfantryBehaviourCount = 7;
std::string_view infantry_behaviour_kind(InfantryBehaviour p_behaviour);

struct BehaviourCurve {
	float weight = 1.0f;
	float slope = 1.0f;
	float offset = 0.0f;

	bool operator==(const BehaviourCurve &) const = default;
};

struct BehaviourTuning {
	std::array<BehaviourCurve, kInfantryBehaviourCount> curves{ { { 8.0f }, { 7.0f }, { 6.0f }, { 1.75f }, { 1.5f },
			{ 1.0f }, { 0.5f } } };
	uint32_t decision_ticks = 6;
	float squad_far_m = 15.0f;
	float squad_arrive_m = 3.0f;
	float mission_arrive_m = 10.0f;
	float hunt_near_m = 30.0f;

	bool operator==(const BehaviourTuning &) const = default;
};

inline constexpr std::string_view kBehaviourSettingsPrefix = "bb.";
bool is_behaviour_key(std::string_view p_key);
core::KeyOutcome parse_behaviour_setting(std::string_view p_key, std::string_view p_value, BehaviourTuning &r_tuning);
std::string format_behaviour_tuning(const BehaviourTuning &p_tuning);

class BotBehaviour : public UrgencyGenerator {
public:
	BotBehaviour(InfantryBehaviour p_kind, const UrgeCurve &p_curve, InfantryBody &r_body);

	InfantryBehaviour kind() const { return kind_; }
	float situation_value() const { return situation_; }

	float urgency(Bot *p_bot) override;
	float active_urgency(Bot *p_bot) override;
	void update_active_urgency(Bot *p_bot) override;
	bool has_changed_target(Bot *p_bot) override;
	UrgencyGenerator *chosen_generator() override { return this; }
	void reset(Bot *p_bot) override;
	int size() override { return 1; }

	void refuse(sim::Tick p_tick) { refused_tick_ = p_tick; }
	virtual bool plan(BotScript &r_script, bool p_was_active) = 0;
	virtual void deactivate() {}

protected:
	virtual float situation() = 0;

	InfantryBehaviour kind_;
	const UrgeCurve &curve_;
	InfantryBody &body_;
	float situation_ = 0.0f;
	float urgency_ = 0.0f;
	float active_urgency_ = 0.0f;
	sim::Tick refused_tick_ = UINT32_MAX;
};

class BBRevive final : public BotBehaviour {
public:
	BBRevive(const UrgeCurve &p_curve, InfantryBody &r_body) :
			BotBehaviour(InfantryBehaviour::Revive, p_curve, r_body) {}
	bool plan(BotScript &r_script, bool p_was_active) override;

protected:
	float situation() override;
};

class BBFireInfantery final : public BotBehaviour {
public:
	enum class Mode : uint8_t { None, Throwing, Sight, Throw, Blind };

	BBFireInfantery(const UrgeCurve &p_curve, InfantryBody &r_body) :
			BotBehaviour(InfantryBehaviour::FireInfantery, p_curve, r_body) {}
	bool plan(BotScript &r_script, bool p_was_active) override;
	Mode mode() const { return mode_; }

protected:
	float situation() override;

private:
	Mode mode_ = Mode::None;
};

class BBTakeCoverInfantry final : public BotBehaviour {
public:
	enum class Phase : uint8_t { None, Move, Wait };
	enum class Want : uint8_t { None, Start, Move, WaitFromMove, Wait };

	BBTakeCoverInfantry(const UrgeCurve &p_curve, InfantryBody &r_body) :
			BotBehaviour(InfantryBehaviour::TakeCoverInfantry, p_curve, r_body) {}
	bool plan(BotScript &r_script, bool p_was_active) override;
	void deactivate() override { phase_ = Phase::None; }
	void reset(Bot *p_bot) override;
	Phase phase() const { return phase_; }

protected:
	float situation() override;

private:
	Phase phase_ = Phase::None;
	Want want_ = Want::None;
};

class BBAvoid final : public BotBehaviour {
public:
	BBAvoid(const UrgeCurve &p_curve, InfantryBody &r_body) : BotBehaviour(InfantryBehaviour::Avoid, p_curve, r_body) {}
	bool plan(BotScript &r_script, bool p_was_active) override;

protected:
	float situation() override;
};

class BBMoveTo final : public BotBehaviour {
public:
	BBMoveTo(const UrgeCurve &p_curve, InfantryBody &r_body, const SquadGoal &p_goal, const BehaviourTuning &p_tuning,
			const int &p_ordered) :
			BotBehaviour(InfantryBehaviour::MoveTo, p_curve, r_body), goal_(p_goal), tuning_(p_tuning),
			ordered_(p_ordered) {}
	bool plan(BotScript &r_script, bool p_was_active) override;
	bool following_squad() const { return squad_; }
	bool holding() const { return squad_ && hold_; }

protected:
	float situation() override;

private:
	const SquadGoal &goal_;
	const BehaviourTuning &tuning_;
	const int &ordered_;
	bool squad_ = false;
	bool hold_ = false;
};

class BBRandom final : public BotBehaviour {
public:
	BBRandom(const UrgeCurve &p_curve, InfantryBody &r_body) :
			BotBehaviour(InfantryBehaviour::Random, p_curve, r_body) {}
	bool plan(BotScript &r_script, bool p_was_active) override;

protected:
	float situation() override;
};

class BBIdle final : public BotBehaviour {
public:
	BBIdle(const UrgeCurve &p_curve, InfantryBody &r_body) : BotBehaviour(InfantryBehaviour::Idle, p_curve, r_body) {}
	bool plan(BotScript &r_script, bool p_was_active) override;

protected:
	float situation() override;
};

}

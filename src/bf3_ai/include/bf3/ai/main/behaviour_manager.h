#pragma once

#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/main/ai_settings.h"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::ai {

class BAPEntry;

class BBCreateFunction {
public:
	virtual ~BBCreateFunction() = default;
	virtual int operator()(int p_behaviour_id, const std::string &p_name, int p_mode, const UrgeCurve *p_urge_curve,
			const float *p_weights) = 0;
};

struct BehaviourKind {
	std::string_view name;
	AiDomain domain = AiDomain::Any;
};

std::span<const BehaviourKind> behaviour_kinds();

class BotBehaviourManager {
public:
	explicit BotBehaviourManager(const AiSettings &p_settings);

	bool reset();
	void set_behaviour_name(int p_id, const std::string &p_name);
	int behaviour_id(std::string_view p_name) const;
	std::string behaviour_name(int p_id) const;
	bool set_create_function(std::string_view p_kind, BBCreateFunction *p_function);
	bool is_registered(std::string_view p_kind) const;
	bool is_enabled(std::string_view p_kind) const;
	int create(const std::string &p_behaviour_name, const std::string &p_create_function_name,
			const std::string &p_name, int p_mode, const std::string &p_urge_curve_name, const float *p_weights,
			std::string &r_message);
	std::string create_uc_constant(const std::string &p_name, float p_value);
	std::string create_uc_linear(const std::string &p_name, float p_slope, float p_offset);
	std::string create_uc_x_inverse(
			const std::string &p_name, float p_numerator, float p_offset, float p_slope, float p_bias);
	const UrgeCurve *urge_curve(const std::string &p_name) const;
	std::unique_ptr<BAPEntry> create_plan_entry(std::string_view p_name) const;

private:
	struct NoCase {
		using is_transparent = void;
		bool operator()(std::string_view p_a, std::string_view p_b) const;
	};
	struct Registration {
		AiDomain domain = AiDomain::Any;
		BBCreateFunction *function = nullptr;
	};

	const AiSettings &settings_;
	std::map<std::string, Registration, NoCase> create_functions_;
	std::map<std::string, int, NoCase> behaviour_ids_;
	std::map<std::string, std::unique_ptr<UrgeCurve>, NoCase> urge_curves_;
	std::vector<std::string> behaviour_names_;
};

}

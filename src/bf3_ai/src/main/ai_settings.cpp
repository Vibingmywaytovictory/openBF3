#include "bf3/ai/main/ai_settings.h"

#include <algorithm>
#include <climits>

namespace bf3::ai {

namespace {

using core::KeyDef;
using core::KeyFault;
using core::KeyKind;
using core::KeyOutcome;

constexpr size_t kMaxSettingsBytes = 64 * 1024;
constexpr size_t kMaxSettingsLines = 1024;

#define BF3_AI_FIELD(f) [](AiSettings &c) -> void * { return &c.f; }

const KeyDef<AiSettings> kKeys[] = {
	{ "ai.max_bots", KeyKind::I32, 0, 64, BF3_AI_FIELD(max_bots) },
	{ "ai.max_bots_include_humans", KeyKind::Bool, 0, 0, BF3_AI_FIELD(max_bots_include_humans) },
	{ "ai.bot_skill", KeyKind::Float, 0.0, 1.0, BF3_AI_FIELD(bot_skill) },
	{ "ai.view_distance_m", KeyKind::Float, -1.0, 5000.0, BF3_AI_FIELD(view_distance_m) },
	{ "ai.sai_update_frequency", KeyKind::Float, 0.01, 1000.0, BF3_AI_FIELD(sai_update_frequency) },
	{ "ai.information_grid_dimension", KeyKind::U32, 1, 1024, BF3_AI_FIELD(information_grid_dimension) },
	{ "ai.sides", KeyKind::U32, 1, 2, BF3_AI_FIELD(sides) },
	{ "ai.respawn_allowed", KeyKind::Bool, 0, 0, BF3_AI_FIELD(respawn_allowed) },
	{ "ai.auto_spawn_bots", KeyKind::Bool, 0, 0, BF3_AI_FIELD(auto_spawn_bots) },
	{ "ai.enable_vehicle_ai", KeyKind::Bool, 0, 0, BF3_AI_FIELD(enable_vehicle_ai) },
	{ "ai.enable_air_ai", KeyKind::Bool, 0, 0, BF3_AI_FIELD(enable_air_ai) },
};

#undef BF3_AI_FIELD

bool is_ai_key(std::string_view p_key) {
	return p_key.substr(0, kAiSettingsPrefix.size()) == kAiSettingsPrefix;
}

KeyOutcome skip_host_key(std::string_view p_key, std::string_view, AiSettings &) {
	if (is_ai_key(p_key)) {
		return { KeyFault::UnknownKey };
	}
	return {};
}

const core::KeyValueGrammar<AiSettings> kGrammar{ "bot_tuning ai", kKeys, kMaxSettingsBytes, kMaxSettingsLines,
	SIZE_MAX, false, '\n', true, true, core::key_fault_message, nullptr, skip_host_key };

}

bool AiSettings::domain_enabled(AiDomain p_domain) const {
	switch (p_domain) {
		case AiDomain::Any:
		case AiDomain::Infantry:
			return true;
		case AiDomain::Vehicle:
			return enable_vehicle_ai;
		case AiDomain::Air:
			return enable_air_ai;
	}
	return false;
}

std::span<const KeyDef<AiSettings>> ai_settings_keys() {
	return kKeys;
}

core::Status parse_ai_settings(std::string_view p_text, AiSettings &r_settings) {
	core::Result<AiSettings> parsed = core::parse_key_values(p_text, kGrammar, r_settings);
	if (!parsed) {
		return parsed.error();
	}
	r_settings = *parsed;
	return core::Status::success();
}

KeyOutcome check_ai_setting(std::string_view p_key, std::string_view p_value) {
	const auto key = std::find_if(
			std::begin(kKeys), std::end(kKeys), [&](const KeyDef<AiSettings> &p_def) { return p_def.name == p_key; });
	if (key == std::end(kKeys)) {
		return { KeyFault::UnknownKey };
	}
	AiSettings scratch;
	return core::parse_key_value(*key, p_value, scratch);
}

std::string format_ai_settings(const AiSettings &p_settings) {
	return core::format_key_values(p_settings, ai_settings_keys());
}

}

#pragma once

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/main/ai_script.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace bf3::test {

inline constexpr std::string_view kShippedAiLevel = "MP_Subway";

inline std::filesystem::path shipped_ai_root() {
	return OPENBF3_AI_DIR;
}

inline ai::AiConfig shipped_ai_config() {
	core::Result<ai::AiConfig> config = ai::load_level_ai_config(shipped_ai_root(), kShippedAiLevel, {});
	REQUIRE_MESSAGE(config.ok(), (config.ok() ? std::string() : config.error().message));
	return std::move(*config);
}

inline ai::BehaviourTuning shipped_behaviour_tuning() {
	ai::BehaviourTuning tuning;
	const core::Status st = ai::apply_behaviour_weights(shipped_ai_config(), tuning);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	return tuning;
}

template <class BotConfig> void use_shipped_bot_ai(BotConfig &r_bots) {
	const core::Status st = load_bot_ai(shipped_ai_root(), kShippedAiLevel, {}, r_bots);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}

}

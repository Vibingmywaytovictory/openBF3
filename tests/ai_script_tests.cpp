#include <doctest/doctest.h>

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/main/ai_script.h"
#include "bf3/ai/main/bot_skill.h"
#include "bf3/core/level/level.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

using namespace bf3;
using namespace bf3::ai;

namespace {

const std::filesystem::path kShipped = OPENBF3_AI_DIR;

struct ScratchRoot {
	std::filesystem::path dir = std::filesystem::temp_directory_path() /
			("openbf3-ai-script-" +
					std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
	ScratchRoot() {
		std::filesystem::create_directories(dir);
		std::filesystem::copy_file(kShipped / kAiDefaultScript, dir / kAiDefaultScript);
		std::filesystem::copy_file(kShipped / kAiBehavioursScript, dir / kAiBehavioursScript);
	}
	~ScratchRoot() {
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
	void level(std::string_view p_level, std::string_view p_text) const {
		const std::filesystem::path path = level_ai_script(dir, p_level);
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary) << p_text;
	}
};

std::string message(const core::Status &p_status) {
	return p_status.ok() ? std::string() : p_status.error().message;
}

}

TEST_SUITE("ai-script") {
	TEST_CASE("the shipped .ai files: every shipped level loads its own AI.ai, and they give the shipped "
			  "settings and the Default set's behaviour weights on the Memory curve") {
		const std::vector<std::string_view> levels = core::level::shipped_level_ids();
		REQUIRE(levels.size() == 29u);
		for (const std::string_view level : levels) {
			const core::Result<AiConfig> config = load_level_ai_config(kShipped, level);
			REQUIRE_MESSAGE(config.ok(), (config.ok() ? std::string() : config.error().message));
			REQUIRE(config->scripts.size() == 3u);
			CHECK(config->scripts.back() == level_ai_script(kShipped, level).generic_string());
			AiSettings expected;
			expected.vehicles = { "Infantry" };
			expected.sides = 2;
			expected.behaviour_count = int32_t(kInfantryBehaviourCount);
			expected.basic_bot_weights = config->behaviour_modifiers.at("Default");
			CHECK(config->settings == expected);
			BehaviourTuning tuning;
			tuning.squad_far_m = 99.0f;
			const core::Status st = apply_behaviour_weights(*config, tuning);
			REQUIRE_MESSAGE(st.ok(), message(st));
			BehaviourTuning want;
			want.squad_far_m = 99.0f;
			const UrgeCurveDef &memory = config->urge_curves.at("Memory");
			REQUIRE(memory.kind == UrgeCurveDef::Kind::Linear);
			for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
				want.curves[i] = { expected.basic_bot_weights[i], memory.values[0], memory.values[1] };
			}
			CHECK(tuning == want);
			CHECK(config->weapon_templates.empty());
		}
	}

	TEST_CASE("a level loads its own map file: its weights and skill reach the config, a level without one fails") {
		const ScratchRoot root;
		std::string own_script = "aiSettings.setBotSkill 0.2\n"
								 "aiSettings.createUCLinear Rising 0.5 0.25\n"
								 "aiSettings.createBehaviourModifiers Own\n"
								 "aiSettings.setBehaviourModifier Revive 3\n"
								 "aiSettings.setBehaviourModifier Idle 9\n";
		for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
			const InfantryBehaviour b = InfantryBehaviour(i);
			if (b != InfantryBehaviour::Revive && b != InfantryBehaviour::Idle) {
				own_script +=
						"aiSettings.setBehaviourModifier " + std::string(infantry_behaviour_kind(b).substr(2)) + " 0\n";
			}
		}
		root.level("XP9_Own",
				own_script +
						"aiSettings.setBasicBotWeights Own\n"
						"aiSettings.setVehicleBehaviour Infantry Idle BBIdle BBPIdle 0 Rising Own\n");
		root.level("XP9_Other", "aiSettings.setBasicBotWeights Default\n");
		const core::Result<AiConfig> own = load_level_ai_config(root.dir, "XP9_Own");
		REQUIRE_MESSAGE(own.ok(), (own.ok() ? std::string() : own.error().message));
		const core::Result<AiConfig> other = load_level_ai_config(root.dir, "XP9_Other");
		REQUIRE(other.ok());
		CHECK(own->settings.bot_skill == doctest::Approx(0.2f));
		CHECK(other->settings.bot_skill == doctest::Approx(0.75f));
		BehaviourTuning a, b;
		REQUIRE(apply_behaviour_weights(*own, a).ok());
		REQUIRE(apply_behaviour_weights(*other, b).ok());
		CHECK(a.curves[size_t(InfantryBehaviour::Revive)].weight == 3.0f);
		CHECK(a.curves[size_t(InfantryBehaviour::Idle)].weight == 9.0f);
		CHECK(a.curves[size_t(InfantryBehaviour::Idle)].slope == 0.5f);
		CHECK(a.curves[size_t(InfantryBehaviour::Idle)].offset == 0.25f);
		CHECK(a.curves[size_t(InfantryBehaviour::FireInfantery)].weight == 0.0f);
		const core::Result<AiConfig> shipped = load_level_ai_config(kShipped, "MP_Subway");
		REQUIRE(shipped.ok());
		BehaviourTuning shipped_tuning;
		REQUIRE(apply_behaviour_weights(*shipped, shipped_tuning).ok());
		CHECK(b == shipped_tuning);

		const core::Result<AiConfig> none = load_level_ai_config(root.dir, "XP9_None");
		REQUIRE_FALSE(none.ok());
		CHECK(none.error().message.find("Levels/XP9_None/AI/AI.ai") != std::string::npos);
		root.level("XP9_NoWeights", "aiSettings.setBotSkill 0.5\n");
		const core::Result<AiConfig> unweighted = load_level_ai_config(root.dir, "XP9_NoWeights");
		REQUIRE(unweighted.ok());
		BehaviourTuning c;
		const core::Status st = apply_behaviour_weights(*unweighted, c);
		CHECK_FALSE(st.ok());
		CHECK(message(st).find("aiSettings.setBasicBotWeights") != std::string::npos);
	}

	TEST_CASE("the reader: BF2's command names, strict arguments, a failed script changes nothing") {
		AiConfig c;
		REQUIRE(run_ai_script("aiSettings.setMaxNumberOfBehaviours 2\r\n\n  aiSettings.setBehaviour 0 A\t\n"
							  "AISETTINGS.SETBEHAVIOUR 1 B\naiSettings.createBehaviourModifiers M\n"
							  "aiSettings.setBehaviourModifier b 4.5\naiSettings.setStandardPersonality m 2\n",
				"t.ai", c)
						.ok());
		CHECK(c.behaviour_id("a") == 0);
		CHECK(c.behaviour_id("B") == 1);
		CHECK(c.behaviour_modifiers.at("M") == std::vector<float>{ 0.0f, 4.5f });
		CHECK(c.settings.personality(1) == nullptr);
		REQUIRE(c.settings.personality(2) != nullptr);
		CHECK(c.settings.personality(2)[1] == 4.5f);
		CHECK(c.scripts == std::vector<std::string>{ "t.ai" });

		const AiConfig before = c;
		for (const char *bad : { "aiSettings.nope 1\n", "aiSettings.setBotSkill\n", "aiSettings.setBotSkill 0.5 1\n",
					 "aiSettings.setBotSkill 1.5\n", "aiSettings.setBotSkill nan\n", "aiSettings.setNSides 3\n",
					 "aiSettings.setRespawnAllowed true\n", "aiSettings.setMaxNBots 2.5\n",
					 "aiSettings.setBehaviour 2 C\n", "aiSettings.setBehaviourModifier C 1\n",
					 "aiSettings.setBasicBotWeights N\n", "aiSettings.activeBehaviourModifiers N\n",
					 "aiSettings.setStandardPersonality M 3\n",
					 "aiSettings.setVehicleBehaviour Infantry A BBA BBPA 0 Memory M\n",
					 "aiSettings.setVehicle Infantry\naiSettings.setVehicleBehaviour Infantry A BBA BBPA 0 Memory M\n",
					 "weaponTemplate.deviation 3\n", "weaponTemplate.active Gun\n",
					 "aiSettings.setBotSkill 0.1\nrem a comment\n" }) {
			AiConfig t = c;
			const core::Status st = run_ai_script(bad, "bad.ai", t);
			CHECK_MESSAGE(!st.ok(), bad);
			CHECK_MESSAGE(message(st).rfind("bad.ai:", 0) == 0, message(st));
			CHECK(t == before);
		}
		AiConfig big;
		CHECK_FALSE(run_ai_script(std::string(kMaxAiScriptBytes + 1, '\n'), "big.ai", big).ok());
		CHECK_FALSE(run_ai_script(std::string(kMaxAiScriptLines, '\n'), "long.ai", big).ok());
		CHECK(run_ai_script(std::string(kMaxAiScriptLines - 1, '\n'), "long.ai", big).ok());
		CHECK_FALSE(run_ai_script_file(kShipped / "no-such.ai", big).ok());
	}

	TEST_CASE("weaponTemplate: the reconstruction's defaults, the console's correction rate, unknown names default") {
		const WeaponTemplate def;
		CHECK(def.deviation == 14.0f);
		CHECK(def.deviation_correction_time == 15.0f);
		CHECK(def.deviation_correction_rate == 0.06666667f);
		AiConfig c;
		REQUIRE(run_ai_script("weaponTemplate.create Gun\nweaponTemplate.deviation 4\n"
							  "weaponTemplate.deviationCorrectionTime 2\nweaponTemplate.create Zero\n"
							  "weaponTemplate.deviationCorrectionTime 0\n",
				"w.ai", c)
						.ok());
		CHECK(c.weapon_template("gun").deviation == 4.0f);
		CHECK(c.weapon_template("Gun").deviation_correction_rate == 0.5f);
		CHECK(c.weapon_template("Zero").deviation_correction_rate == 0.0f);
		CHECK(c.weapon_template("Other") == def);
		AiConfig twice = c;
		CHECK_FALSE(run_ai_script("weaponTemplate.create GUN\n", "w2.ai", twice).ok());
	}

	TEST_CASE("bot skill as the reconstruction reads it: EntryTrigger's skill, WeaponObject's deviation, "
			  "BBPFireInfantery's first-shot delay and its fresh plan") {
		CHECK(trigger_skill(0.75f, {}, true, false) == doctest::Approx(0.875f));
		CHECK(trigger_skill(0.75f, {}, true, true) == doctest::Approx(0.75f));
		CHECK(trigger_skill(0.75f, { false, 4.0f, false }, true, false) == doctest::Approx(0.9f));
		CHECK(trigger_skill(0.75f, { false, 4.0f, false }, false, false) == doctest::Approx(0.8f));
		CHECK(trigger_skill(0.2f, { true, 0.0f, false }, true, false) == 1.0f);
		CHECK(trigger_skill(0.2f, { false, 0.0f, true }, true, false) == 1.0f);
		const WeaponTemplate w;
		CHECK(ai_deviation_deg(w, 0.875f, 0.0, 1.0f) == doctest::Approx(1.75f));
		CHECK(ai_deviation_deg(w, 0.875f, 7.5, 1.0f) == doctest::Approx(1.3125f));
		CHECK(ai_deviation_deg(w, 0.875f, 15.0, 1.0f) == doctest::Approx(0.875f));
		CHECK(ai_deviation_deg(w, 0.875f, 600.0, 3.0f) == doctest::Approx(2.625f));
		CHECK(ai_deviation_deg(w, 1.0f, 0.0, 1.0f) == 0.0f);
		CHECK(fire_delay_s(0.75f, 0.0f) == doctest::Approx(0.375f));
		CHECK(fire_delay_s(0.75f, 1.0f) == doctest::Approx(0.625f));
		CHECK(fire_delay_s(1.0f, 0.5f) == 0.0f);
		CHECK(fresh_fire_plan(false, 0.75f));
		CHECK_FALSE(fresh_fire_plan(true, 0.75f));
		CHECK_FALSE(fresh_fire_plan(true, 0.3f));
		CHECK(fresh_fire_plan(true, 0.29f));
	}

	TEST_CASE("a basic bot weights set that misses one behaviour's modifier fails loudly, naming it; an explicit "
			  "0 is a modifier") {
		std::ifstream f(kShipped / kAiBehavioursScript, std::ios::binary);
		REQUIRE(f.good());
		const std::string shipped((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		const std::string line = "aiSettings.setBehaviourModifier Resupply ";
		const size_t at = shipped.find(line);
		REQUIRE(at != std::string::npos);
		const size_t end = shipped.find('\n', at);
		REQUIRE(end != std::string::npos);
		const std::string without = shipped.substr(0, at) + shipped.substr(end + 1);
		const ScratchRoot root;
		root.level("XP9_Missing", "aiSettings.setBasicBotWeights Default\n");
		std::ofstream(root.dir / kAiBehavioursScript, std::ios::binary) << without;
		const core::Result<AiConfig> missing = load_level_ai_config(root.dir, "XP9_Missing");
		REQUIRE_FALSE(missing.ok());
		CHECK(missing.error().message.find("Resupply") != std::string::npos);
		CHECK(missing.error().message.find("aiSettings.setBehaviourModifier") != std::string::npos);
		std::ofstream(root.dir / kAiBehavioursScript, std::ios::binary)
				<< without << "aiSettings.activeBehaviourModifiers Default\n"
				<< "aiSettings.setBehaviourModifier Resupply 0\n";
		const core::Result<AiConfig> zero = load_level_ai_config(root.dir, "XP9_Missing");
		REQUIRE_MESSAGE(zero.ok(), (zero.ok() ? std::string() : zero.error().message));
		CHECK(zero->settings.basic_bot_weights.at(size_t(InfantryBehaviour::Resupply)) == 0.0f);
	}
}

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
constexpr std::string_view kTwoAreas = "aiStrategicArea.createFromControlPoint A 0 30\n"
									   "aiStrategicArea.createFromControlPoint B 1 30\n";

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
	void level(std::string_view p_level, std::string_view p_text) const { file(level_ai_script(dir, p_level), p_text); }
	void file(const std::filesystem::path &p_path, std::string_view p_text) const {
		std::filesystem::create_directories(p_path.parent_path());
		std::ofstream(p_path, std::ios::binary) << p_text;
	}
};

std::string message(const core::Status &p_status) {
	return p_status.ok() ? std::string() : p_status.error().message;
}

}

TEST_SUITE("ai-script") {
	TEST_CASE("the shipped .ai files: no level has its own file, every shipped level loads the common scripts and "
			  "its derived strategic areas, and they give the shipped settings and the Default set's behaviour "
			  "weights on the Memory curve") {
		const std::vector<std::string_view> levels = core::level::shipped_level_ids();
		REQUIRE(levels.size() == 29u);
		CHECK_FALSE(std::filesystem::exists(kShipped / "Levels"));
		for (const std::string_view level : levels) {
			const core::Result<AiConfig> config = load_level_ai_config(kShipped, level, kTwoAreas);
			REQUIRE_MESSAGE(config.ok(), (config.ok() ? std::string() : config.error().message));
			REQUIRE(config->scripts.size() == 3u);
			CHECK(config->scripts.back() ==
					"derived:" + level_strategic_areas_script(kShipped, level).generic_string());
			AiSettings expected;
			expected.vehicles = { "Infantry" };
			expected.sides = 2;
			expected.behaviour_count = int32_t(kInfantryBehaviourCount);
			expected.basic_bot_weights = config->behaviour_modifiers.at("Default");
			CHECK(config->settings == expected);
			CHECK(config->strategic_areas.size() == 2u);
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

	TEST_CASE("a level's own files override: its StrategicAreas.ai replaces the derived areas, its AI.ai runs last "
			  "and its weights and skill reach the config; a config that never sets the basic bot weights is refused") {
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
		root.file(level_strategic_areas_script(root.dir, "XP9_Own"),
				"aiStrategicArea.createFromControlPoint Own 0 12\naiStrategicArea.setActive Own\n"
				"aiStrategicArea.setSide 2\n");
		const core::Result<AiConfig> own = load_level_ai_config(root.dir, "XP9_Own", kTwoAreas);
		REQUIRE_MESSAGE(own.ok(), (own.ok() ? std::string() : own.error().message));
		CHECK(own->scripts.size() == 4u);
		const core::Result<AiConfig> other = load_level_ai_config(root.dir, "XP9_Other", kTwoAreas);
		REQUIRE(other.ok());
		CHECK(own->settings.bot_skill == doctest::Approx(0.2f));
		CHECK(other->settings.bot_skill == doctest::Approx(0.75f));
		REQUIRE(own->strategic_areas.size() == 1u);
		CHECK(own->strategic_areas[0].name == "Own");
		CHECK(own->strategic_areas[0].side == 2);
		REQUIRE(other->strategic_areas.size() == 2u);
		CHECK(other->strategic_areas[1].name == "B");
		BehaviourTuning a, b;
		REQUIRE(apply_behaviour_weights(*own, a).ok());
		REQUIRE(apply_behaviour_weights(*other, b).ok());
		CHECK(a.curves[size_t(InfantryBehaviour::Revive)].weight == 3.0f);
		CHECK(a.curves[size_t(InfantryBehaviour::Idle)].weight == 9.0f);
		CHECK(a.curves[size_t(InfantryBehaviour::Idle)].slope == 0.5f);
		CHECK(a.curves[size_t(InfantryBehaviour::Idle)].offset == 0.25f);
		CHECK(a.curves[size_t(InfantryBehaviour::FireInfantery)].weight == 0.0f);
		const core::Result<AiConfig> shipped = load_level_ai_config(kShipped, "MP_Subway", kTwoAreas);
		REQUIRE(shipped.ok());
		BehaviourTuning shipped_tuning;
		REQUIRE(apply_behaviour_weights(*shipped, shipped_tuning).ok());
		CHECK(b == shipped_tuning);

		AiConfig unweighted;
		REQUIRE(run_ai_script_file(kShipped / kAiDefaultScript, unweighted).ok());
		REQUIRE(unweighted.settings.vehicle("Infantry") >= 0);
		BehaviourTuning c;
		const core::Status st = apply_behaviour_weights(unweighted, c);
		CHECK_FALSE(st.ok());
		CHECK(message(st).find("aiSettings.setBasicBotWeights") != std::string::npos);

		const core::Result<AiConfig> broken =
				load_level_ai_config(root.dir, "XP9_Other", "aiStrategicArea.setSide 1\n");
		REQUIRE_FALSE(broken.ok());
		CHECK(broken.error().message.find("derived:") != std::string::npos);
		CHECK(broken.error().message.find("aiStrategicArea.setActive") != std::string::npos);
	}

	TEST_CASE("aiStrategicArea: OpenBF2's StrategicAreas.ai commands, strict arguments, a create makes its area the "
			  "active one, and saveAll's text (by name, after its rem header) reads back to the same areas") {
		std::vector<StrategicAreaDef> areas(3);
		areas[0].name = "HQ_US";
		areas[0].from_control_point = true;
		areas[0].control_point = 0;
		areas[0].radius = 35.5f;
		areas[0].side = 1;
		areas[0].takeable = { false, false };
		areas[0].neighbours = { "CP_B" };
		areas[1].name = "CP_B";
		areas[1].category = "Flags";
		areas[1].from_control_point = true;
		areas[1].control_point = 1;
		areas[1].radius = 0.1f;
		areas[2].name = "TDM_2";
		areas[2].category = "Clusters";
		areas[2].corner = { -12.345678f, 1.0e-3f };
		areas[2].centre = { 27.654322f, 40.001f };
		areas[2].height = -3.25f;
		areas[2].radius = 40.0f;
		areas[2].layer = 3;
		areas[2].takeable = { true, false };
		const std::string text = format_strategic_areas(areas);
		CHECK(text.starts_with("rem *** Create strategic areas ***\naiStrategicArea.createFromControlPoint CP_B 1 "
							   "0.1 Flags\naiStrategicArea.layer -1\n\n"));
		AiConfig read;
		REQUIRE_MESSAGE(run_ai_script(text, "areas.ai", read).ok(), text);
		const std::vector<StrategicAreaDef> by_name = strategic_areas_by_name(areas);
		REQUIRE(by_name.size() == 3u);
		CHECK(by_name[0].name == "CP_B");
		CHECK(by_name[1].name == "HQ_US");
		CHECK(read.strategic_areas == by_name);
		CHECK(read.strategic_area("CP_B") == &read.strategic_areas[0]);
		CHECK(read.strategic_area("cp_b") == nullptr);
		CHECK(read.strategic_area("none") == nullptr);
		CHECK(format_strategic_areas(read.strategic_areas) == text);

		AiConfig created;
		const core::Status made = run_ai_script("rem a comment line\naiStrategicArea.createFromControlPoint A 0 30\n"
												"aiStrategicArea.setSide 2\naiStrategicArea.create B 1/2 3/4 5 6\n"
												"aiStrategicArea.layer 2\naiStrategicArea.setActive A\n"
												"aiStrategicArea.layer 2\naiStrategicArea.addNeighbour B\n",
				"created.ai", created);
		REQUIRE_MESSAGE(made.ok(), message(made));
		REQUIRE(created.strategic_areas.size() == 2u);
		CHECK(created.strategic_areas[0].side == 2);
		CHECK(created.strategic_areas[0].layer == 2);
		CHECK(created.strategic_areas[0].neighbours == std::vector<std::string>{ "B" });
		CHECK(created.strategic_areas[1].side == 0);
		CHECK(created.strategic_areas[1].layer == 2);
		CHECK(created.strategic_areas[1].neighbours.empty());
		CHECK(StrategicAreaDef{}.layer == -1);
		AiConfig unlayered;
		REQUIRE(run_ai_script(
				"aiStrategicArea.create C 1/2 3/4 5 6\naiStrategicArea.layer -1\n", "unlayered.ai", unlayered)
						.ok());
		CHECK(unlayered.strategic_areas[0].layer == -1);

		const AiConfig before = read;
		for (const char *bad : { "aiStrategicArea.create X 1/2 3/4 5\n", "aiStrategicArea.create X 1 3/4 5 6\n",
					 "aiStrategicArea.create X 1/2 3/z 5 6\n", "aiStrategicArea.create X 1/2 3/4 5 6 Cat More\n",
					 "aiStrategicArea.createFromControlPoint HQ_US 4 5\n",
					 "aiStrategicArea.createFromControlPoint HQ_US 4 5\naiStrategicArea.setSide 2\n",
					 "aiStrategicArea.createFromControlPoint Y 1 5 Cat More\n",
					 "aiStrategicArea.createFromControlPoint Y -1 5\n", "aiStrategicArea.setActive Nowhere\n",
					 "aiStrategicArea.setActive cp_b\n",
					 "aiStrategicArea.setActive CP_B\naiStrategicArea.addNeighbour CP_B\n",
					 "aiStrategicArea.setActive CP_B\naiStrategicArea.addNeighbour Nowhere\n",
					 "aiStrategicArea.setActive HQ_US\naiStrategicArea.addNeighbour TDM_2\n",
					 "aiStrategicArea.setActive CP_B\naiStrategicArea.layer 1\naiStrategicArea.addNeighbour HQ_US\n",
					 "aiStrategicArea.setActive CP_B\naiStrategicArea.setSide 3\n",
					 "aiStrategicArea.setActive CP_B\naiStrategicArea.setTakeable 0 1\n" }) {
			AiConfig t = read;
			const core::Status st = run_ai_script(bad, "bad.ai", t);
			CHECK_MESSAGE(!st.ok(), bad);
			CHECK(t == before);
		}
		AiConfig fresh;
		CHECK_FALSE(run_ai_script("aiStrategicArea.setSide 1\n", "bad.ai", fresh).ok());
		CHECK_FALSE(run_ai_script("aiStrategicArea.layer 1\n", "bad.ai", fresh).ok());
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
					 "aiSettings.setBotSkill 0.1\nremark a comment\n" }) {
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

	TEST_CASE("weaponTemplate: the reconstruction's defaults, the console's correction rate, unknown names default, "
			  "the deviation weapon_deviation_deg takes from a named template") {
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
		CHECK(weapon_deviation_deg(c.weapon_templates, "Gun", {}, 0.75f, 0.0) == doctest::Approx(0.5f));
		CHECK(weapon_deviation_deg(c.weapon_templates, "gun", {}, 0.75f, 2.0) == doctest::Approx(0.25f));
		CHECK(weapon_deviation_deg(c.weapon_templates, "Rifle", {}, 0.75f, 0.0) == doctest::Approx(1.75f));
		CHECK(weapon_deviation_deg(c.weapon_templates, "Zero", {}, 1.0f, 0.0) == 0.0f);
		CHECK(&find_weapon_template(c.weapon_templates, "Rifle") == &find_weapon_template({}, "Gun"));
	}

	TEST_CASE("weaponTemplate: every console binding of the pin with its constructor defaults, the strength type "
			  "and pose names, the inputs the AI reads, and the formatter's round trip") {
		const WeaponTemplate def;
		CHECK_FALSE(def.indirect);
		CHECK_FALSE(def.is_thrown);
		CHECK_FALSE(def.use_aimer_only);
		CHECK(def.fire_rate == -1);
		CHECK(def.drag == 0.0f);
		CHECK(def.min_range == 0.0f);
		CHECK(def.max_range == 0.0f);
		CHECK(def.explosion_radius == 0.0f);
		CHECK(def.sound_sphere_radius == 50.0f);
		CHECK(def.exit_velocity == 0.0f);
		CHECK(def.strengths == std::array<int32_t, kCombatStrengthTypeCount>{});
		CHECK(def.firing_pose == SoldierPose::Standing);
		CHECK(def.allowed_deviation == 0.5f);
		CHECK(def.optimal_range_percentage == 0.35f);
		CHECK(def.fires_through_transparent);
		CHECK(def.weapon_activate == PlayerInputMap::None);
		CHECK(def.weapon_fire == PlayerInputMap::None);

		AiConfig c;
		REQUIRE(run_ai_script("weaponTemplate.create Rpg\n"
							  "weaponTemplate.indirect 1\nweaponTemplate.useAimerOnly 1\nweaponTemplate.drag 0.25\n"
							  "weaponTemplate.minRange 12\nweaponTemplate.maxRange 75\n"
							  "weaponTemplate.weaponActivate PIWeaponSelect5\nweaponTemplate.weaponFire PIFire\n"
							  "weaponTemplate.isThrown 1\nweaponTemplate.soundSphereRadius 33\n"
							  "weaponTemplate.exitVelocity 310\nweaponTemplate.getStrength Infantry\n"
							  "weaponTemplate.setStrength Infantery 3\nweaponTemplate.setStrength CSTLightArmour 6\n"
							  "weaponTemplate.setStrength heavyarmour 5\nweaponTemplate.setStrength 3 -2\n"
							  "weaponTemplate.setStrength Helicopter 9\nweaponTemplate.setStrength AirPlane 7\n"
							  "weaponTemplate.fireRate 45\nweaponTemplate.firingPose SPLying\n"
							  "weaponTemplate.optimalRangePercentage 40\nweaponTemplate.explosionRadius 3.5\n"
							  "weaponTemplate.allowedDeviation 1.25\nweaponTemplate.firesThroughTransparent 0\n"
							  "weaponTemplate.active Rpg\nweaponTemplate.DEVIATION 3\n",
				"w.ai", c)
						.ok());
		const WeaponTemplate &w = c.weapon_template("Rpg");
		CHECK(w.indirect);
		CHECK(w.use_aimer_only);
		CHECK(w.drag == 0.25f);
		CHECK(w.min_range == 12.0f);
		CHECK(w.max_range == 75.0f);
		CHECK(w.weapon_activate == PlayerInputMap::WeaponSelect5);
		CHECK(w.weapon_fire == PlayerInputMap::Fire);
		CHECK(w.is_thrown);
		CHECK(w.sound_sphere_radius == 33.0f);
		CHECK(w.exit_velocity == 310.0f);
		CHECK(w.strengths == std::array<int32_t, kCombatStrengthTypeCount>{ 3, 6, 5, -2, 9, 7 });
		CHECK(w.strength(CombatStrengthType::HeavyArmour) == 5);
		CHECK(w.fire_rate == 45);
		CHECK(w.firing_pose == SoldierPose::Lying);
		CHECK(w.optimal_range_percentage == 40.0f);
		CHECK(w.explosion_radius == 3.5f);
		CHECK(w.allowed_deviation == 1.25f);
		CHECK_FALSE(w.fires_through_transparent);
		CHECK(w.deviation == 3.0f);

		AiConfig odd;
		REQUIRE(run_ai_script("weaponTemplate.create Odd\nweaponTemplate.firingPose Kneeling\n"
							  "weaponTemplate.weaponFire PINone\n",
				"odd.ai", odd)
						.ok());
		CHECK(odd.weapon_template("Odd").firing_pose == SoldierPose::NotAvailable);
		CHECK(odd.weapon_template("Odd").weapon_fire == PlayerInputMap::None);

		const AiConfig before = c;
		for (const char *bad : { "weaponTemplate.setStrength All 3\n", "weaponTemplate.setStrength 6 3\n",
					 "weaponTemplate.setStrength Infantry 5.0\n", "weaponTemplate.getStrength Tank\n",
					 "weaponTemplate.setSoundSphereRadius 33\n", "weaponTemplate.setFiringPose Lying\n",
					 "weaponTemplate.weaponFire PIBogus\n", "weaponTemplate.weaponFire 8\n",
					 "weaponTemplate.isThrown 2\n" }) {
			AiConfig t = c;
			CHECK_MESSAGE(!run_ai_script(bad, "bad.ai", t).ok(), bad);
			CHECK(t == before);
		}
		AiConfig none;
		CHECK_FALSE(run_ai_script("weaponTemplate.minRange 3\n", "none.ai", none).ok());

		const std::string text = format_weapon_template("Rpg", w);
		CHECK(text.rfind("weaponTemplate.create Rpg\n", 0) == 0);
		CHECK(text.find("weaponTemplate.setStrength Infantry 3\n") != std::string::npos);
		CHECK(text.find("weaponTemplate.firingPose Lying\n") != std::string::npos);
		CHECK(text.find("weaponTemplate.weaponFire PIFire\n") != std::string::npos);
		AiConfig again;
		REQUIRE(run_ai_script(text, "again.ai", again).ok());
		CHECK(again.weapon_template("Rpg") == w);
		CHECK(format_weapon_template("Plain", WeaponTemplate{}) == "weaponTemplate.create Plain\n");
	}

	TEST_CASE("weapon templates: a level's generated templates join the config after the hand-written Weapons/*.ai "
			  "files, which win by name in any case; a broken hand file fails the load") {
		ScratchRoot root;
		WeaponTemplate rifle;
		rifle.strengths[0] = 78;
		WeaponTemplate rocket;
		rocket.strengths = { 11, 7, 2, 0, 0, 0 };
		rocket.min_range = 20.0f;
		const WeaponTemplateMap generated{ { "weapons/ak/u_ak", rifle }, { "weapons/rpg/u_rpg", rocket } };

		const core::Result<AiConfig> plain = load_level_ai_config(root.dir, "XP9_W", kTwoAreas, generated);
		REQUIRE(plain.ok());
		CHECK(plain->weapon_templates == generated);

		root.file(root.dir / kWeaponTemplatesDir / "b.ai",
				"weaponTemplate.create WEAPONS/RPG/U_RPG\n"
				"weaponTemplate.setStrength HeavyArmour 9\n");
		root.file(root.dir / kWeaponTemplatesDir / "a.ai", "weaponTemplate.create Hand\n");
		root.file(root.dir / kWeaponTemplatesDir / "notes.txt", "not a script\n");
		const core::Result<AiConfig> hand = load_level_ai_config(root.dir, "XP9_W", kTwoAreas, generated);
		REQUIRE(hand.ok());
		CHECK(hand->weapon_templates.size() == 3u);
		CHECK(hand->weapon_template("weapons/rpg/u_rpg").strengths == std::array<int32_t, 6>{ 0, 0, 9, 0, 0, 0 });
		CHECK(hand->weapon_template("weapons/rpg/u_rpg").min_range == 0.0f);
		CHECK(hand->weapon_template("weapons/ak/u_ak") == rifle);
		CHECK(hand->weapon_templates.count("Hand") == 1u);
		REQUIRE(hand->scripts.size() >= 2u);

		root.file(root.dir / kWeaponTemplatesDir / "c.ai", "weaponTemplate.setStrength Tank 1\n");
		const core::Result<AiConfig> broken = load_level_ai_config(root.dir, "XP9_W", kTwoAreas, generated);
		REQUIRE_FALSE(broken.ok());
		CHECK(broken.error().message.find("c.ai") != std::string::npos);
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
		std::ofstream(root.dir / kAiBehavioursScript, std::ios::binary) << without;
		const core::Result<AiConfig> missing = load_level_ai_config(root.dir, "XP9_Missing", kTwoAreas);
		REQUIRE_FALSE(missing.ok());
		CHECK(missing.error().message.find("Resupply") != std::string::npos);
		CHECK(missing.error().message.find("aiSettings.setBehaviourModifier") != std::string::npos);
		std::ofstream(root.dir / kAiBehavioursScript, std::ios::binary) << shipped.substr(0, at) << line << "0\n"
																		<< shipped.substr(end + 1);
		const core::Result<AiConfig> zero = load_level_ai_config(root.dir, "XP9_Missing", kTwoAreas);
		REQUIRE_MESSAGE(zero.ok(), (zero.ok() ? std::string() : zero.error().message));
		CHECK(zero->settings.basic_bot_weights.at(size_t(InfantryBehaviour::Resupply)) == 0.0f);
	}
}

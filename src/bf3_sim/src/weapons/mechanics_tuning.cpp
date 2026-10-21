#include "bf3/sim/weapons/mechanics_tuning.h"

#include "bf3/core/util/key_values.h"

#include <string>

namespace bf3::sim::weapons {

namespace {

constexpr std::string_view kDiscs[] = { "uniform_area", "uniform_radius" };
constexpr std::string_view kRecoveries[] = { "linear", "exponential" };
constexpr std::string_view kRearms[] = { "on_trigger_release", "when_recovered" };
constexpr std::string_view kRises[] = { "immediate", "at_decrease_rate" };
constexpr std::string_view kLinear[] = { "linear" };
constexpr std::string_view kFalloffBases[] = { "straight_line", "path_length" };

static_assert(sizeof(DiscDistribution) == 1 && sizeof(RecoveryShape) == 1 && sizeof(FirstShotRearm) == 1 &&
		sizeof(MinSpreadRise) == 1 && sizeof(FalloffShape) == 1 && sizeof(FalloffBasis) == 1 &&
		sizeof(BlastFalloff) == 1);

using Key = core::KeyDef<Mechanics>;
using core::KeyKind;

#define BF3_MECH_FIELD(f) [](Mechanics &m) -> void * { return &m.f; }

const Key kKeys[] = {
	{ "disc", KeyKind::Choice, 0, 0, BF3_MECH_FIELD(disc), kDiscs },
	{ "recovery", KeyKind::Choice, 0, 0, BF3_MECH_FIELD(recovery), kRecoveries },
	{ "first_shot_rearm", KeyKind::Choice, 0, 0, BF3_MECH_FIELD(first_shot_rearm), kRearms },
	{ "min_spread_rise", KeyKind::Choice, 0, 0, BF3_MECH_FIELD(min_spread_rise), kRises },
	{ "falloff", KeyKind::Choice, 0, 0, BF3_MECH_FIELD(falloff), kLinear },
	{ "falloff_basis", KeyKind::Choice, 0, 0, BF3_MECH_FIELD(falloff_basis), kFalloffBases },
	{ "blast_falloff", KeyKind::Choice, 0, 0, BF3_MECH_FIELD(blast_falloff), kLinear },
	{ "stance_transition_penalty", KeyKind::Bool, 0, 0, BF3_MECH_FIELD(stance_transition_penalty) },
	{ "suppression_threshold", KeyKind::Float, 0.0f, 1.0f, BF3_MECH_FIELD(suppression_threshold) },
	{ "apply_protection_threshold", KeyKind::Bool, 0, 0, BF3_MECH_FIELD(apply_protection_threshold) },
	{ "bullets_pass_soldiers", KeyKind::Bool, 0, 0, BF3_MECH_FIELD(bullets_pass_soldiers) },
	{ "blast_occlusion", KeyKind::Bool, 0, 0, BF3_MECH_FIELD(blast_occlusion) },
	{ "empty_reload_unchambered", KeyKind::Bool, 0, 0, BF3_MECH_FIELD(empty_reload_unchambered) },
	{ "sprint_blocks_fire", KeyKind::Bool, 0, 0, BF3_MECH_FIELD(sprint_blocks_fire) },
	{ "spawn_protection_blocks", KeyKind::Bool, 0, 0, BF3_MECH_FIELD(spawn_protection_blocks) },
	{ "bullet_damage_modifier", KeyKind::Float, 0.0f, 100.0f, BF3_MECH_FIELD(bullet_damage_modifier) },
	{ "measured", KeyKind::Bool, 0, 0, BF3_MECH_FIELD(measured) },
};

#undef BF3_MECH_FIELD

std::string mechanics_fault(const core::KeyFaultInfo &p_info) {
	switch (p_info.fault) {
		case core::KeyFault::NotABool:
			return core::key_fault_where(p_info) + ": `" + std::string(p_info.key) + "` must be true or false, got `" +
					core::shown_text(p_info.value) + "`";
		case core::KeyFault::NotAChoice:
			return core::key_fault_where(p_info) + ": `" + std::string(p_info.key) + "` must be " +
					core::choice_list(p_info.choices, " | ", " | ") + ", got `" + core::shown_text(p_info.value) + "`";
		default:
			return core::key_fault_message(p_info);
	}
}

const core::KeyValueGrammar<Mechanics> kGrammar{ "weapon mechanics", kKeys, kMaxMechanicsTuningBytes,
	kMaxMechanicsTuningLines, SIZE_MAX, true, '\n', true, true, mechanics_fault };
}

core::Result<Mechanics> parse_mechanics_tuning(std::string_view p_text) {
	return core::parse_key_values(p_text, kGrammar);
}

std::string format_mechanics_tuning(const Mechanics &p_mechanics) {
	return core::format_key_values(p_mechanics, kKeys);
}

}

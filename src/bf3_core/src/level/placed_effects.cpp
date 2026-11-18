#include "bf3/core/level/placed_effects.h"

#include "bf3/core/level/budget.h"
#include "bf3/core/util/text.h"

#include <optional>
#include <string_view>

namespace bf3::core::level {

Result<std::vector<PlacedEffect>> read_placed_effects(const World &p_world) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	std::vector<PlacedEffect> out;
	for (size_t i = 0; i < p_world.objects.size(); ++i) {
		const PlacedObject &object = p_world.objects[i];
		BF3_TRY(budget.charge(1));
		if (object.kind != ObjectKind::Effect || !object.entity.is_a("EffectEntityData")) {
			continue;
		}
		if (out.size() >= kMaxPlacedEffects) {
			return make_error("placed effects: more than " + std::to_string(kMaxPlacedEffects));
		}
		const std::optional<std::string_view> name = p_world.scopes.db().name_of(object.entity.partition().guid());
		if (!name) {
			return make_error(
					"placed effects: effect entity " + std::to_string(i) + " has no blueprint partition name");
		}
		BF3_TRY(budget.charge(1 + name->size() / 16));
		PlacedEffect effect;
		effect.object = uint32_t(i);
		effect.blueprint = to_lower_ascii(*name);
		effect.transform = object.transform;
		if (object.reference) {
			const std::optional<EbxValue> v = object.reference->get("AutoStart");
			effect.auto_start = (v ? v->as_bool() : std::nullopt).value_or(false);
		}
		out.push_back(std::move(effect));
	}
	return out;
}

}

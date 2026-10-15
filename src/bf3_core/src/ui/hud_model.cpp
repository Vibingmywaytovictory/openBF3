// UI ui2: the HudModel binding rows against an install table (include/bf3/core/ui/hud_model.h; docs/design/
// ui-minimap-icons.md §7.2, gate G8). Both directions: every row's (component, source) must be in the table, and every
// source of the 11 bound types must have a row. The in-game components without rows are listed with their source count.
#include "bf3/core/ui/hud_model.h"

#include "bf3/core/ui/hud_sources.h"

#include <algorithm>
#include <set>

namespace bf3::core::ui {

HudBindingReport check_hud_bindings(const HudSourceTable &p_table) {
	HudBindingReport r;
	std::set<std::pair<std::string_view, std::string_view>> rows;
	for (const HudBinding &b : kHudBindings) {
		++r.rows;
		r.kinds[size_t(b.kind)] += 1;
		rows.emplace(b.component, b.source);
		const HudComponentType *t = p_table.find(b.component);
		const bool listed = t != nullptr && std::find(t->sources.begin(), t->sources.end(), b.source) != t->sources.end();
		if (listed) {
			++r.matched;
		} else {
			r.not_in_install.push_back(std::string(b.component) + "." + std::string(b.source));
		}
	}
	for (std::string_view component : kHudBoundComponents) {
		const HudComponentType *t = p_table.find(component);
		if (t == nullptr) {
			continue; // every row of it is already in not_in_install
		}
		for (const std::string &s : t->sources) {
			if (rows.find({ component, std::string_view(s) }) == rows.end()) {
				r.not_in_rows.push_back(std::string(component) + "." + s);
			}
		}
	}
	std::set<uint32_t> seen;
	for (uint32_t i : p_table.state_components) {
		if (i >= p_table.types.size() || !seen.insert(i).second) {
			continue;
		}
		const HudComponentType &t = p_table.types[i];
		if (std::find(std::begin(kHudBoundComponents), std::end(kHudBoundComponents), std::string_view(t.type)) != std::end(kHudBoundComponents)) {
			continue;
		}
		r.unbound_components.push_back(t.type);
		r.unbound_sources += t.sources.size();
	}
	return r;
}

} // namespace bf3::core::ui

#include "bf3/core/ui/hud_widget_registry.h"

#include <utility>

namespace bf3::core::ui {

bool HudWidgetRegistry::add(std::string_view p_class, HudWidgetFactory p_make) {
	if (p_class.empty() || !p_make || factories_.find(p_class) != factories_.end()) {
		rejected_.emplace_back(p_class);
		return false;
	}
	factories_.emplace(std::string(p_class), std::move(p_make));
	return true;
}

std::unique_ptr<HudWidgetController> HudWidgetRegistry::make(std::string_view p_class) const {
	const auto it = factories_.find(p_class);
	return it != factories_.end() ? it->second() : nullptr;
}

bool HudWidgetRegistry::knows(std::string_view p_class) const {
	return factories_.find(p_class) != factories_.end();
}

std::vector<std::string> HudWidgetRegistry::classes() const {
	std::vector<std::string> out;
	out.reserve(factories_.size());
	for (const auto &[name, make] : factories_) {
		out.push_back(name);
	}
	return out;
}

const HudWidgetRegistry &HudWidgetRegistry::installed() {
	static const HudWidgetRegistry registry = [] {
		HudWidgetRegistry r;
		register_hud_widget_files(r);
		return r;
	}();
	return registry;
}

std::unique_ptr<HudWidgetController> make_hud_widget_controller(std::string_view p_class) {
	return HudWidgetRegistry::installed().make(p_class);
}

bool hud_widget_class_known(std::string_view p_class) {
	return HudWidgetRegistry::installed().knows(p_class);
}

}

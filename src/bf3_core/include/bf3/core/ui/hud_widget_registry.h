#pragma once

#include "bf3/core/ui/hud_screen.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

using HudWidgetFactory = std::function<std::unique_ptr<HudWidgetController>()>;

class HudWidgetRegistry {
public:
	bool add(std::string_view p_class, HudWidgetFactory p_make);
	std::unique_ptr<HudWidgetController> make(std::string_view p_class) const;
	bool knows(std::string_view p_class) const;
	std::vector<std::string> classes() const;
	const std::vector<std::string> &rejected() const { return rejected_; }

	static const HudWidgetRegistry &installed();

private:
	std::map<std::string, HudWidgetFactory, std::less<>> factories_;
	std::vector<std::string> rejected_;
};

void register_hud_widget_files(HudWidgetRegistry &r_registry);

}

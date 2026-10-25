#pragma once

#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bf3::core::ui {
class HudScreen;
}

namespace bf3::godot_side {

class Bf3SwfView;

struct UiScreenSlot {
	std::string_view partition;
	bf3::core::ui::HudScreen *screen = nullptr;
	Bf3SwfView *view = nullptr;
	bool shown = false;
	int32_t order = 0;
};

class UiScreenHost {
public:
	virtual ~UiScreenHost() = default;

	virtual bf3::core::Status open_screen(std::string_view p_partition) = 0;
	virtual bool show_screen(std::string_view p_partition) = 0;
	virtual bool hide_screen(std::string_view p_partition) = 0;
	virtual bool order_screen(std::string_view p_partition, int32_t p_order) = 0;
	virtual bool close_screen(std::string_view p_partition) = 0;

	virtual size_t screen_count() const = 0;
	virtual UiScreenSlot screen_slot(size_t p_index) const = 0;

	UiScreenSlot find_screen(std::string_view p_partition) const {
		for (size_t i = 0; i < screen_count(); ++i) {
			const UiScreenSlot s = screen_slot(i);
			if (s.partition == p_partition) {
				return s;
			}
		}
		return {};
	}
};

}

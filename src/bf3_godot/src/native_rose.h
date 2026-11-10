#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/commo_rose.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace bf3::godot_side {

class Bf3SwfView;

inline constexpr float kRoseDeadZone = 24.0f;

struct NativeRoseState {
	bool open = false;
	godot::Vector2 cursor;
	bool in_vehicle = false;
	bool alive = true;
	bool leader = false;
};

godot::Vector2 rose_button_position(const bf3::core::ui::As2Clip &p_button);
std::optional<int32_t> rose_button_at(
		const std::array<godot::Vector2, 12> &p_buttons, std::array<bool, 12> p_shown, godot::Vector2 p_cursor);

class NativeRose {
public:
	static bf3::core::Result<std::unique_ptr<NativeRose>> open(godot::Node &r_parent,
			const bf3::core::ui::HudScreenAsset &p_asset, bf3::core::ui::CommoRoseLayout p_layout,
			const bf3::core::ui::HudClientState &p_client, const bf3::core::ui::HudStageRects &p_rects);
	~NativeRose();
	NativeRose(const NativeRose &) = delete;
	NativeRose &operator=(const NativeRose &) = delete;

	void step(const bf3::core::ui::HudModel &p_model, const NativeRoseState &p_state,
			const bf3::core::ui::HudStageRects &p_rects, double p_dt);
	bool shown() const { return shown_; }
	int32_t selected_button() const { return selected_; }
	const bf3::core::ui::CommoRoseEntry *selected_entry() const;
	const bf3::core::ui::CommoRoseLayout &layout() const { return layout_; }
	std::optional<godot::Vector2> button_offset(int32_t p_button) const;
	Bf3SwfView *view() const { return view_; }
	bf3::core::ui::HudScreen *screen() const { return screen_.get(); }
	uint64_t opens() const { return opens_; }

private:
	NativeRose() = default;
	std::string rose_path() const;

	Bf3SwfView *view_ = nullptr;
	std::unique_ptr<bf3::core::ui::HudScreen> screen_;
	bf3::core::ui::CommoRoseLayout layout_;
	bf3::core::ui::HudModel model_{};
	bool shown_ = false;
	bool initialized_ = false;
	int32_t selected_ = -1;
	uint64_t opens_ = 0;
};

}

#pragma once

#include "hud_tdm_screen.h"

#include <array>
#include <string>

namespace godot {
class Button;
}

namespace bf3::godot_side {

inline constexpr float kSpawnCardWidth = 190.0f;
inline constexpr float kSpawnCardHeight = 140.0f;
inline constexpr float kSpawnCardGap = 10.0f;
inline constexpr float kSpawnDeployLeft = 914.0f;
inline constexpr float kSpawnDeployTop = 622.0f;
inline constexpr float kSpawnDeployWidth = 270.0f;
inline constexpr float kSpawnDeployHeight = 56.0f;

class Bf3TdmSpawnScreen : public Bf3TdmScreen {
	GDCLASS(Bf3TdmSpawnScreen, Bf3TdmScreen)

public:
	void build_screen() override;
	void update_screen() override;
	void draw_screen() override;

	int take_kit_click();
	bool take_deploy_click();

protected:
	static void _bind_methods() {}

private:
	void on_kit(int64_t p_kit);
	void on_deploy();
	void style_buttons();
	godot::Rect2 card_rect(size_t p_kit) const;
	godot::Rect2 deploy_rect() const;

	std::array<godot::Button *, core::ui::kHudKitCount> kits_{};
	godot::Button *deploy_ = nullptr;
	bool styled_ = false;
	int32_t styled_size_ = 0;
	std::string deploy_text_;
	int pending_kit_ = -1;
	bool pending_deploy_ = false;
	bool was_visible_ = false;
};

}

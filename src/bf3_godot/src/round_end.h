#pragma once

#include "hud_tdm_screen.h"

namespace bf3::godot_side {

inline constexpr float kRoundEndLeft = 340.0f;
inline constexpr float kRoundEndTop = 150.0f;
inline constexpr float kRoundEndWidth = 600.0f;
inline constexpr float kRoundEndHeight = 400.0f;

class Bf3TdmRoundEnd : public Bf3TdmScreen {
	GDCLASS(Bf3TdmRoundEnd, Bf3TdmScreen)

public:
	void update_screen() override;
	void draw_screen() override;

protected:
	static void _bind_methods() {}

private:
	bool was_visible_ = false;
};

}

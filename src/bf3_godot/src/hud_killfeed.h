#pragma once

#include "hud_tdm_screen.h"

#include <array>
#include <cstdint>

namespace bf3::godot_side {

inline constexpr float kKillFeedRightInset = 18.0f;
inline constexpr float kKillFeedTop = 40.0f;
inline constexpr float kKillFeedLineHeight = 22.0f;
inline constexpr float kKillFeedNameSize = 14.0f;
inline constexpr float kKillFeedIconWidth = 64.0f;
inline constexpr float kKillFeedIconHeight = 16.0f;
inline constexpr float kKillFeedGap = 6.0f;
inline constexpr double kKillFeedFadeSeconds = 0.5;
inline constexpr float kKillCounterTop = 22.0f;
inline constexpr float kKillCounterSize = 20.0f;

class Bf3TdmKillFeed : public Bf3TdmScreen {
	GDCLASS(Bf3TdmKillFeed, Bf3TdmScreen)

public:
	void update_screen() override;
	void draw_screen() override;

protected:
	static void _bind_methods() {}

private:
	void draw_counter();
	struct Seen {
		uint32_t serial = 0;
		double first_s = 0;
	};
	std::array<Seen, core::ui::kHudKillFeedSize> seen_{};
};

}

#pragma once

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <array>
#include <cstdint>

namespace bf3::godot_side {

inline constexpr std::array<godot::Vector2, 7> kUiCursorArrow{ godot::Vector2(0.0f, 0.0f), godot::Vector2(13.0f, 13.0f),
	godot::Vector2(7.8f, 13.0f), godot::Vector2(9.6f, 18.6f), godot::Vector2(7.4f, 19.2f), godot::Vector2(5.0f, 13.8f),
	godot::Vector2(0.0f, 18.5f) };
inline constexpr float kUiCursorOutlinePx = 1.0f;
inline constexpr uint8_t kUiCursorFillRgb[3] = { 214, 245, 249 };
inline constexpr uint8_t kUiCursorOutlineRgb[3] = { 22, 64, 90 };

struct UiCursorStats {
	uint32_t draws = 0;
	uint32_t shows = 0;
	uint32_t hides = 0;
};

class Bf3UiCursor : public godot::CanvasLayer {
	GDCLASS(Bf3UiCursor, godot::CanvasLayer)

public:
	Bf3UiCursor();
	~Bf3UiCursor() override;

	static Bf3UiCursor *current();
	static Bf3UiCursor *ensure(godot::Node *p_parent);

	void _ready() override;
	void _process(double p_delta) override;

	void show_cursor();
	void hide_cursor();
	bool cursor_shown() const { return shown_; }
	void set_cursor_position(godot::Vector2 p_canvas);
	godot::Vector2 cursor_position() const { return position_; }
	void set_follow_mouse(bool p_follow) { follow_mouse_ = p_follow; }
	bool follows_mouse() const { return follow_mouse_; }
	float cursor_scale() const;
	const UiCursorStats &stats() const { return stats_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void redraw();

	godot::RID item_;
	bool shown_ = false;
	bool follow_mouse_ = false;
	godot::Vector2 position_;
	float drawn_scale_ = 0.0f;
	UiCursorStats stats_;
};

}

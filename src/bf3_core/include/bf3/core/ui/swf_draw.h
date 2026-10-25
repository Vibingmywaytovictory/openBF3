#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::core::ui {

inline constexpr uint32_t kSwfDrawMaxCommands = 1024;

enum class SwfDrawOpKind : uint8_t {
	Clear,
	BeginFill,
	MoveTo,
	LineTo,
	CurveTo,
	EndFill,
};

struct SwfDrawOp {
	SwfDrawOpKind kind = SwfDrawOpKind::Clear;
	SwfColor color;
	SwfPoint to;
	SwfPoint control;

	static SwfDrawOp clear() { return {}; }
	static SwfDrawOp begin_fill(SwfColor p_color) { return { SwfDrawOpKind::BeginFill, p_color, {}, {} }; }
	static SwfDrawOp move_to(float p_x, float p_y) { return { SwfDrawOpKind::MoveTo, {}, { p_x, p_y }, {} }; }
	static SwfDrawOp line_to(float p_x, float p_y) { return { SwfDrawOpKind::LineTo, {}, { p_x, p_y }, {} }; }
	static SwfDrawOp curve_to(float p_cx, float p_cy, float p_x, float p_y) { return { SwfDrawOpKind::CurveTo, {}, { p_x, p_y }, { p_cx, p_cy } }; }
	static SwfDrawOp end_fill() { return { SwfDrawOpKind::EndFill, {}, {}, {} }; }
};

SwfColor swf_draw_fill_color(uint32_t p_rgb, double p_alpha_percent);

class SwfDrawing {
public:
	Status apply(const SwfDrawOp &p_op);
	bool empty() const { return fills_.empty(); }
	uint32_t commands() const { return commands_; }
	Result<std::shared_ptr<const SwfShape>> shape() const;

private:
	struct Fill {
		SwfColor color;
		std::vector<SwfPoint> outline;
		std::vector<uint32_t> contour_ends;
	};
	void close_contour();

	std::vector<Fill> fills_;
	bool filling_ = false;
	SwfPoint pen_;
	uint32_t commands_ = 0;
};

}

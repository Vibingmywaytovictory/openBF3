#pragma once

#include "bf3/core/ui/minimap.h"
#include "bf3/core/ui/swf_scene.h"

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

enum class MinimapDebugKind : uint8_t { Objective, Friendly, Squad, Enemy, Downed, Local };

struct MinimapDebugHit {
	MinimapDebugKind kind = MinimapDebugKind::Objective;
	float world_x = 0.0f;
	float world_z = 0.0f;
	float local_x = 0.0f;
	float local_y = 0.0f;
	std::string icon;
};

struct MinimapDebugFrame {
	std::string path;
	godot::RID texture;
	bf3::core::ui::SwfRect local_bounds;
	godot::Vector2i size;
	bf3::core::ui::MinimapView view;
	float metres_per_unit = 0.0f;
	float local_per_unit = 1.0f;
	std::vector<MinimapDebugHit> hits;
	uint64_t paints = 0;
	uint64_t frame = 0;
};

inline constexpr std::string_view kMinimapDebugUvGlsl = R"(
	if (debug_uv) {
		vec2 q = floor(clamp(uv, 0.0, 1.0) * 4095.0 + 0.5);
		float uh = floor(q.x / 16.0);
		float vh = floor(q.y / 256.0);
		COLOR = vec4(uh / 255.0, ((q.x - uh * 16.0) * 16.0 + vh) / 255.0, (q.y - vh * 256.0) / 255.0, debug_alpha);
	}
)";

}

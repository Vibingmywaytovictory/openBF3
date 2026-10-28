#pragma once

#include "bf3/core/gameplay/eor_fading.h"
#include "bf3/host/logic_screen_effects.h"

#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>

namespace godot {
class CanvasLayer;
class Node;
}

namespace bf3::godot_side {

inline constexpr int32_t kEorWhiteLayer = 0;
inline constexpr int32_t kEorFadeLayer = 99;

struct EorFlashStats {
	uint64_t round_overs = 0;
	uint64_t white_frames = 0;
	float peak_visibility = 0.0f;
	uint64_t black_frames = 0;
	uint64_t fade_in_frames = 0;
	uint64_t ui_faded_frames = 0;
	float peak_ui_faded = 0.0f;
	float last_black = 0.0f;
	float last_visibility = 0.0f;
	float last_ui_faded = 0.0f;
};

class EorFlash {
public:
	EorFlash() = default;
	~EorFlash();
	EorFlash(const EorFlash &) = delete;
	EorFlash &operator=(const EorFlash &) = delete;

	void frame(godot::Node *p_parent, godot::CanvasLayer *p_ui, const core::gameplay::EorFading &p_fading,
			const host::LogicScreenEffectsFrame &p_effects);
	const EorFlashStats &stats() const { return stats_; }

private:
	void build(godot::Node *p_parent);
	void cover(godot::Node *p_parent);
	void fade_ui(godot::CanvasLayer *p_ui, float p_alpha);

	godot::CanvasLayer *white_layer_ = nullptr;
	godot::CanvasLayer *fade_layer_ = nullptr;
	godot::Ref<godot::ShaderMaterial> white_material_;
	godot::RID white_item_;
	godot::RID fade_item_;
	float covered_w_ = 0.0f;
	float covered_h_ = 0.0f;
	float drawn_black_ = -1.0f;
	bool white_enabled_ = false;
	EorFlashStats stats_;
};

}

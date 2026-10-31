#pragma once

#include "bf3/core/ui/swf_layer_plan.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::godot_side {

struct UiPagesCounters {
	uint64_t viewport_creates = 0;
	uint64_t pages_made = 0;
	uint64_t passes = 0;
	uint64_t own_surface_fallbacks = 0;
	uint64_t live_viewports = 0;
	uint64_t live_slots = 0;
	uint64_t used_px = 0;
	uint64_t bytes = 0;
};

class Bf3UiPages : public godot::Node {
	GDCLASS(Bf3UiPages, godot::Node)

public:
	Bf3UiPages();
	~Bf3UiPages() override;

	static Bf3UiPages *current();

	void _enter_tree() override;
	void _exit_tree() override;

	void ensure(godot::Vector2i p_window);
	void begin_frame();
	void end_pass();
	uint64_t generation() const { return generation_; }
	uint64_t pass_serial() const { return pass_serial_; }

	godot::RID scratch_canvas() const { return scratch_canvas_; }
	godot::RID result_canvas() const { return result_canvas_; }
	godot::RID scratch_texture() const { return scratch_texture_; }
	godot::RID result_texture() const { return result_texture_; }
	godot::Vector2i scratch_size() const { return godot::Vector2i(sizes_.scratch_width, sizes_.scratch_height); }
	godot::Vector2i result_size() const { return godot::Vector2i(sizes_.result_width, sizes_.result_height); }

	bool fits_scratch(godot::Vector2i p_size) const;
	std::optional<bf3::core::ui::SwfPageSlot> alloc_result(godot::Vector2i p_size);
	void release_result(const bf3::core::ui::SwfPageSlot &p_slot);
	bf3::core::ui::SwfScratchPass take_scratch(std::span<const bf3::core::ui::SwfDirtyLayer> p_dirty);
	void note_fill(godot::RID p_group, godot::RID p_stage);
	void forget(godot::RID p_item);
	void note_own_surface_fallback();
	UiPagesCounters counters() const;

protected:
	static void _bind_methods();

private:
	void free_pages();

	godot::RenderingServer *rs_ = nullptr;
	bf3::core::ui::SwfPageSizes sizes_;
	std::optional<bf3::core::ui::SwfShelfPacker> result_;
	std::optional<bf3::core::ui::SwfShelfPacker> pass_;
	godot::RID scratch_viewport_;
	godot::RID scratch_canvas_;
	godot::RID scratch_texture_;
	godot::RID result_viewport_;
	godot::RID result_canvas_;
	godot::RID result_texture_;
	std::vector<godot::RID> in_pass_;
	uint64_t generation_ = 0;
	uint64_t frame_ = UINT64_MAX;
	uint64_t pass_serial_ = 1;
	bool pass_requested_ = false;
	UiPagesCounters counters_;
};

}

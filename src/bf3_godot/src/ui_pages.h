#pragma once

#include "bf3/core/ui/swf_layer_plan.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform2d.hpp>
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
	uint64_t frame_updates = 0;
	uint64_t frame_presents = 0;
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
	void use_sizes(const bf3::core::ui::SwfPageSizes &p_sizes);
	void begin_frame();
	void end_pass();
	uint64_t generation() const { return generation_; }
	uint64_t pass_serial() const { return pass_serial_; }

	godot::RID scratch_canvas() const { return scratch_canvas_; }
	godot::RID result_canvas() const { return result_canvas_; }
	godot::RID scratch_texture() const { return scratch_texture_; }
	godot::RID result_texture() const { return result_texture_; }
	godot::RID frame_viewport() const { return frame_viewport_; }
	godot::RID frame_canvas() const { return frame_canvas_; }
	godot::RID frame_texture() const { return frame_texture_; }
	godot::Vector2i frame_size() const { return frame_size_; }
	bool pages_chain_into_frame() const;
	void mark_frame();
	void attach_present(godot::Node2D *p_node);
	godot::Node2D *present_node() const { return present_node_; }
	void present_frame(const godot::Rect2 &p_at);
	godot::Rect2 presented_at() const { return present_rect_; }
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
	void free_frame();
	void free_present();
	void size_frame(godot::Vector2i p_window);
	void make_pages(const bf3::core::ui::SwfPageSizes &p_sizes);

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
	godot::RID result_parent_;
	godot::RID scratch_parent_;
	godot::RID frame_viewport_;
	godot::RID frame_canvas_;
	godot::RID frame_texture_;
	godot::Vector2i frame_size_;
	godot::Node2D *present_node_ = nullptr;
	godot::RID present_item_;
	godot::Rect2 present_rect_;
	godot::Transform2D present_transform_;
	std::vector<godot::RID> in_pass_;
	uint64_t generation_ = 0;
	uint64_t frame_ = UINT64_MAX;
	uint64_t pass_serial_ = 1;
	bool pass_requested_ = false;
	bool frame_requested_ = false;
	bool sizes_fixed_ = false;
	UiPagesCounters counters_;
};

}

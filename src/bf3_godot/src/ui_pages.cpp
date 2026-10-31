#include "ui_pages.h"

#include "swf_view.h"
#include "ui_stage.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

namespace {

Bf3UiPages *g_current = nullptr;
uint64_t g_generation = 0;

RID make_page_viewport(RenderingServer *p_rs, RenderingServer::ViewportMSAA p_msaa,
		RenderingServer::ViewportClearMode p_clear, int32_t p_width, int32_t p_height, RID p_parent) {
	const RID viewport = p_rs->viewport_create();
	p_rs->viewport_set_disable_3d(viewport, true);
	p_rs->viewport_set_transparent_background(viewport, true);
	p_rs->viewport_set_clear_mode(viewport, p_clear);
	p_rs->viewport_set_update_mode(viewport, RenderingServer::VIEWPORT_UPDATE_DISABLED);
	p_rs->viewport_set_msaa_2d(viewport, p_msaa);
	p_rs->viewport_set_use_hdr_2d(viewport, false);
	if (p_parent.is_valid()) {
		p_rs->viewport_set_parent_viewport(viewport, p_parent);
	}
	p_rs->viewport_set_active(viewport, true);
	p_rs->viewport_set_size(viewport, p_width, p_height);
	return viewport;
}

}

Bf3UiPages::Bf3UiPages() : rs_(RenderingServer::get_singleton()) {
	swf_view_hold_shaders();
}

Bf3UiPages::~Bf3UiPages() {
	free_pages();
	free_frame();
	swf_view_release_shaders();
}

void Bf3UiPages::_bind_methods() {}

Bf3UiPages *Bf3UiPages::current() {
	return g_current;
}

void Bf3UiPages::_enter_tree() {
	g_current = this;
	ensure(ui_window_pixels(this));
}

void Bf3UiPages::_exit_tree() {
	if (g_current == this) {
		g_current = nullptr;
	}
	free_pages();
	free_frame();
}

void Bf3UiPages::free_frame() {
	free_present();
	if (!frame_viewport_.is_valid()) {
		return;
	}
	rs_->free_rid(frame_canvas_);
	rs_->free_rid(frame_viewport_);
	--counters_.live_viewports;
	frame_canvas_ = RID();
	frame_viewport_ = RID();
	frame_texture_ = RID();
	frame_size_ = Vector2i();
	frame_requested_ = false;
}

void Bf3UiPages::free_present() {
	if (present_item_.is_valid()) {
		rs_->free_rid(present_item_);
	}
	present_item_ = RID();
	present_rect_ = Rect2();
	present_transform_ = Transform2D();
}

void Bf3UiPages::attach_present(Node2D *p_node) {
	if (p_node != present_node_) {
		free_present();
		present_node_ = p_node;
	}
}

void Bf3UiPages::present_frame(const Rect2 &p_at) {
	if (present_node_ == nullptr || !present_node_->is_inside_tree() || !frame_viewport_.is_valid()) {
		return;
	}
	if (!present_item_.is_valid()) {
		present_item_ = rs_->canvas_item_create();
		rs_->canvas_item_set_parent(present_item_, present_node_->get_canvas_item());
		rs_->canvas_item_set_material(present_item_, swf_view_present_material());
		rs_->canvas_item_set_default_texture_filter(present_item_, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_NEAREST);
	}
	const Transform2D inverse = present_node_->get_global_transform_with_canvas().affine_inverse();
	if (inverse != present_transform_) {
		rs_->canvas_item_set_transform(present_item_, inverse);
		present_transform_ = inverse;
	}
	if (p_at != present_rect_) {
		rs_->canvas_item_clear(present_item_);
		rs_->canvas_item_add_texture_rect(present_item_, p_at, frame_texture_);
		present_rect_ = p_at;
		++counters_.frame_presents;
	}
}

Ref<Image> Bf3UiPages::capture_frame() const {
	return frame_texture_.is_valid() ? rs_->texture_2d_get(frame_texture_) : Ref<Image>();
}

void Bf3UiPages::size_frame(Vector2i p_window) {
	if (!frame_viewport_.is_valid()) {
		const RID owner = is_inside_tree() && get_viewport() != nullptr ? get_viewport()->get_viewport_rid() : RID();
		frame_viewport_ = make_page_viewport(rs_, RenderingServer::VIEWPORT_MSAA_4X,
				RenderingServer::VIEWPORT_CLEAR_ALWAYS, p_window.x, p_window.y, owner);
		frame_canvas_ = rs_->canvas_create();
		rs_->viewport_attach_canvas(frame_viewport_, frame_canvas_);
		frame_texture_ = rs_->viewport_get_texture(frame_viewport_);
		frame_size_ = p_window;
		++counters_.viewport_creates;
		++counters_.live_viewports;
	} else if (p_window != frame_size_) {
		frame_size_ = p_window;
		rs_->viewport_set_size(frame_viewport_, p_window.x, p_window.y);
		mark_frame();
	}
}

void Bf3UiPages::mark_frame() {
	if (frame_requested_ || !frame_viewport_.is_valid()) {
		return;
	}
	frame_requested_ = true;
	++counters_.frame_updates;
	rs_->viewport_set_update_mode(frame_viewport_, RenderingServer::VIEWPORT_UPDATE_ONCE);
}

bool Bf3UiPages::pages_chain_into_frame() const {
	return frame_viewport_.is_valid() && result_parent_ == frame_viewport_ && result_viewport_.is_valid() &&
			scratch_parent_ == result_viewport_;
}

void Bf3UiPages::free_pages() {
	for (const RID &r : { scratch_canvas_, result_canvas_, scratch_viewport_, result_viewport_ }) {
		if (r.is_valid()) {
			rs_->free_rid(r);
		}
	}
	counters_.live_viewports -= scratch_viewport_.is_valid() ? 2u : 0u;
	result_parent_ = RID();
	scratch_parent_ = RID();
	scratch_canvas_ = RID();
	result_canvas_ = RID();
	scratch_viewport_ = RID();
	result_viewport_ = RID();
	scratch_texture_ = RID();
	result_texture_ = RID();
	sizes_ = ui::SwfPageSizes{};
	result_.reset();
	pass_.reset();
	in_pass_.clear();
	pass_requested_ = false;
}

void Bf3UiPages::use_sizes(const ui::SwfPageSizes &p_sizes) {
	sizes_fixed_ = true;
	make_pages(p_sizes);
}

void Bf3UiPages::ensure(Vector2i p_window) {
	size_frame(p_window);
	const ui::SwfPageSizes want = ui::swf_page_sizes(p_window.x, p_window.y);
	if (sizes_fixed_ ||
			(scratch_viewport_.is_valid() && want.result_width == sizes_.result_width &&
					want.result_height == sizes_.result_height && want.scratch_width == sizes_.scratch_width &&
					want.scratch_height == sizes_.scratch_height)) {
		return;
	}
	make_pages(want);
}

void Bf3UiPages::make_pages(const ui::SwfPageSizes &p_sizes) {
	free_pages();
	sizes_ = p_sizes;
	result_parent_ = frame_viewport_;
	result_viewport_ = make_page_viewport(rs_, RenderingServer::VIEWPORT_MSAA_DISABLED,
			RenderingServer::VIEWPORT_CLEAR_ONLY_NEXT_FRAME, sizes_.result_width, sizes_.result_height, result_parent_);
	scratch_viewport_ = make_page_viewport(rs_, RenderingServer::VIEWPORT_MSAA_4X,
			RenderingServer::VIEWPORT_CLEAR_ALWAYS, sizes_.scratch_width, sizes_.scratch_height, result_viewport_);
	scratch_parent_ = result_viewport_;
	result_canvas_ = rs_->canvas_create();
	scratch_canvas_ = rs_->canvas_create();
	rs_->viewport_attach_canvas(result_viewport_, result_canvas_);
	rs_->viewport_attach_canvas(scratch_viewport_, scratch_canvas_);
	result_texture_ = rs_->viewport_get_texture(result_viewport_);
	scratch_texture_ = rs_->viewport_get_texture(scratch_viewport_);
	result_.emplace(sizes_.result_width, sizes_.result_height);
	pass_.emplace(sizes_.scratch_width, sizes_.scratch_height);
	generation_ = ++g_generation;
	counters_.viewport_creates += 2;
	counters_.live_viewports += 2;
	++counters_.pages_made;
}

void Bf3UiPages::begin_frame() {
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (frame != frame_) {
		frame_ = frame;
		end_pass();
	}
}

void Bf3UiPages::end_pass() {
	for (const RID &item : in_pass_) {
		rs_->canvas_item_set_visible(item, false);
	}
	in_pass_.clear();
	pass_requested_ = false;
	frame_requested_ = false;
	if (scratch_viewport_.is_valid()) {
		pass_.emplace(sizes_.scratch_width, sizes_.scratch_height);
	}
	++pass_serial_;
}

bool Bf3UiPages::fits_scratch(Vector2i p_size) const {
	return ui::swf_page_slot_side(p_size.x) <= sizes_.scratch_width &&
			ui::swf_page_slot_side(p_size.y) <= sizes_.scratch_height;
}

std::optional<ui::SwfPageSlot> Bf3UiPages::alloc_result(Vector2i p_size) {
	return result_ ? result_->alloc(p_size.x, p_size.y) : std::nullopt;
}

void Bf3UiPages::release_result(const ui::SwfPageSlot &p_slot) {
	if (result_) {
		result_->release(p_slot);
	}
}

ui::SwfScratchPass Bf3UiPages::take_scratch(std::span<const ui::SwfDirtyLayer> p_dirty) {
	return ui::swf_select_scratch_pass(p_dirty, *pass_);
}

void Bf3UiPages::note_fill(RID p_group, RID p_stage) {
	in_pass_.push_back(p_group);
	in_pass_.push_back(p_stage);
	if (!pass_requested_) {
		pass_requested_ = true;
		++counters_.passes;
		rs_->viewport_set_update_mode(scratch_viewport_, RenderingServer::VIEWPORT_UPDATE_ONCE);
		rs_->viewport_set_update_mode(result_viewport_, RenderingServer::VIEWPORT_UPDATE_ONCE);
	}
}

void Bf3UiPages::forget(RID p_item) {
	in_pass_.erase(std::remove(in_pass_.begin(), in_pass_.end(), p_item), in_pass_.end());
}

void Bf3UiPages::note_own_surface_fallback() {
	++counters_.own_surface_fallbacks;
}

UiPagesCounters Bf3UiPages::counters() const {
	UiPagesCounters out = counters_;
	out.live_slots = result_ ? result_->live_slots() : 0;
	out.used_px = result_ ? result_->used_px() : 0;
	out.bytes = (scratch_viewport_.is_valid() ? sizes_.result_bytes() + sizes_.scratch_bytes() : 0) +
			(frame_viewport_.is_valid() ? ui::swf_target_bytes(frame_size_.x, frame_size_.y, ui::kSwfTargetFactor4x)
										: 0);
	return out;
}

}

void openbf3_register_ui_pages() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiPages);
}

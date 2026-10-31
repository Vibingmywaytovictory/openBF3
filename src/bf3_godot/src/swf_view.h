#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_layer_plan.h"
#include "bf3/core/ui/swf_scene.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <godot_cpp/variant/rect2.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

enum class SwfScaleMode : uint8_t {
	ShowAll,
	ExactFit,
	NoBorder,
	NoScale,
};

struct SwfTilt {
	float x_rotation = 0.0f;
	float y_rotation = 0.0f;
	float perspective_fov = 0.0f;
};

struct SwfStageFit {
	double scale_x = 1.0;
	double scale_y = 1.0;
	double offset_x = 0.0;
	double offset_y = 0.0;
};

struct SwfEngineTarget {
	std::string renderer;
	std::string path;
	bf3::core::ui::SwfNodeId node = bf3::core::ui::kSwfNoNode;
	bf3::core::ui::SwfRect local_bounds;
	godot::Vector2i size;
	godot::RID canvas_item;
	godot::RID texture;
	bool live = false;
};

struct SwfViewStats {
	uint32_t items = 0;
	uint32_t fill_items = 0;
	uint32_t bitmap_fill_items = 0;
	uint32_t stroke_items = 0;
	uint32_t text_items = 0;
	uint32_t bitmap_items = 0;
	uint32_t glyphs = 0;
	uint32_t additive_items = 0;
	uint32_t filter_layers = 0;
	uint32_t filter_passes = 0;
	uint32_t drop_shadow_passes = 0;
	uint32_t glow_passes = 0;
	uint32_t blur_passes = 0;
	uint32_t mask_groups = 0;
	uint32_t blend_layers = 0;
	uint32_t engine_targets = 0;
	uint32_t tilted_items = 0;
	uint32_t scaling_grid_items = 0;
	uint32_t unsupported_fills = 0;
	uint32_t unsupported_filters = 0;
	uint32_t unsupported_blends = 0;
	uint32_t unsupported_lines = 0;
	uint32_t missing_textures = 0;
	uint32_t missing_fonts = 0;
	uint32_t missing_glyphs = 0;
	uint32_t layers_clamped = 0;
	uint32_t kernels_clamped = 0;
	uint32_t structure_passes = 0;
	uint32_t content_builds = 0;
	uint32_t reopens = 0;
	uint32_t nodes_refreshed = 0;
	uint32_t viewports_updated = 0;
	uint32_t page_fills = 0;
	uint32_t page_moves = 0;
	uint32_t changes = 0;
	double sync_seconds = 0.0;
};

struct SwfViewCounters {
	uint64_t viewport_creates = 0;
	uint64_t viewport_resizes = 0;
	uint64_t viewport_links_without_viewport = 0;
	uint64_t viewport_render_requests = 0;
	uint64_t live_targets_running = 0;
	uint64_t live_surfaces = 0;
	uint64_t live_items = 0;
	uint64_t live_triangle_arrays = 0;
	uint64_t smallest_triangle_array = 0;
	uint64_t live_filter_stages = 0;
	uint64_t live_filter_materials = 0;
	uint64_t retained_bytes = 0;
	uint64_t plans = 0;
	double plan_seconds = 0.0;
	double build_seconds = 0.0;
};

class Bf3SwfView : public godot::Node2D {
	GDCLASS(Bf3SwfView, godot::Node2D)

public:
	static constexpr int32_t kMaxLayerSide = bf3::core::ui::kSwfMaxLayerSide;
	static constexpr uint64_t kSpareSurfaceBytes = uint64_t(64) << 20;
	static constexpr size_t kSpareSurfaces = 256;
	static constexpr uint64_t kDormantSurfaceBytes = uint64_t(64) << 20;
	static constexpr size_t kDormantItems = 2048;
	static constexpr uint32_t kDormantSettleSyncs = 60;
	static constexpr int32_t kMaxKernelTaps = bf3::core::ui::kSwfMaxKernelTaps;

	Bf3SwfView();
	~Bf3SwfView() override;

	void _ready() override;
	void _process(double p_delta) override;

	bf3::core::Status open(std::string_view p_movie);
	bf3::core::Status open(std::shared_ptr<const bf3::core::ui::SwfLibrary> p_library, std::string_view p_movie);
	void close();
	uint64_t surface_bytes() const;
	void release_root_target();
	void release_page_slots();
	uint32_t instance_items() const;
	uint64_t open_serial() const;
	static double sync_seconds_total();
	static SwfViewCounters counters();
	bool is_open() const;
	const std::string &error() const;

	bf3::core::ui::SwfScene *scene();
	const bf3::core::ui::SwfScene *scene() const;

	void set_playing(bool p_playing);
	bool is_playing() const;

	bf3::core::Status set_tilt(std::string_view p_path, const SwfTilt &p_tilt);
	bf3::core::Status clear_tilt(std::string_view p_path);

	bf3::core::Status set_renderer_string(std::string_view p_path, std::string_view p_renderer);
	std::vector<SwfEngineTarget> engine_targets() const;
	std::vector<SwfEngineTarget> engine_targets(std::string_view p_renderer) const;
	bf3::core::Status redraw_engine_target(std::string_view p_path);
	bf3::core::Status set_engine_target_live(std::string_view p_path, bool p_live);

	void set_scale_mode(SwfScaleMode p_mode);
	SwfScaleMode scale_mode() const;
	void set_target_size(godot::Vector2i p_size);
	godot::Vector2i target_size() const;
	SwfStageFit stage_fit() const;
	bf3::core::ui::SwfRect visible_rect() const;
	godot::Rect2 present_rect() const;
	std::optional<bf3::core::ui::SwfPoint> canvas_to_stage(godot::Vector2 p_canvas) const;
	godot::Vector2 stage_to_canvas(bf3::core::ui::SwfPoint p_stage) const;
	void set_skips_hidden_render(bool p_skips);
	uint32_t filter_stages_without_viewport() const;
	bool page_fills_pending() const;
	uint32_t paged_layers() const;
	uint32_t paged_quads_shown() const;
	uint32_t clip_groups() const;
	uint32_t clips_misplaced() const;
	std::optional<godot::Rect2> clip_rect(std::string_view p_mask) const;
	uint32_t engine_targets_unlinked() const;
	void set_msaa(godot::RenderingServer::ViewportMSAA p_msaa);

	bf3::core::Status set_isolated(std::string_view p_path);
	void clear_isolation();
	void set_filters_enabled(bool p_enabled);
	void set_additive_coverage(bool p_coverage);

	void sync();
	godot::RID hud_texture() const;
	godot::Ref<godot::Image> capture_hud() const;
	const SwfViewStats &stats() const;

	bool open_movie(const godot::String &p_movie);
	godot::String get_error() const;
	bool set_tilt_degrees(const godot::String &p_path, float p_x_rotation, float p_y_rotation, float p_perspective_fov);
	bool set_renderer(const godot::String &p_path, const godot::String &p_renderer);
	godot::Ref<godot::Image> get_hud_image() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

godot::Ref<godot::Image> swf_view_transmittance(std::span<const godot::Ref<godot::Image>> p_colour);
void swf_view_hold_shaders();
void swf_view_release_shaders();
}

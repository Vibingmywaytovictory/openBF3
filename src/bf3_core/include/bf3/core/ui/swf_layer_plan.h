#pragma once

#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::ui {

class HudScreen;

inline constexpr int32_t kSwfMaxLayerSide = 4096;
inline constexpr int32_t kSwfMaxKernelTaps = 63;
inline constexpr int32_t kSwfPageRound = 8;
inline constexpr int32_t kSwfPagePadding = 1;
inline constexpr uint64_t kSwfTargetFactor1x = 1;
inline constexpr uint64_t kSwfTargetFactor4x = 5;
inline constexpr double kSwfPageMargin = 1.25;
inline constexpr uint32_t kSwfNoLayer = 0xffffffffu;
inline constexpr int32_t kSwfPageBaseWidth = 1280;
inline constexpr int32_t kSwfPageBaseHeight = 720;
inline constexpr uint64_t kSwfPageDemandPxAtBase = 1080512;
inline constexpr int32_t kSwfPageMaxSide = 16384;

uint64_t swf_target_bytes(int32_t p_width, int32_t p_height, uint64_t p_factor);

struct SwfBounds {
	double x0 = 1e30;
	double y0 = 1e30;
	double x1 = -1e30;
	double y1 = -1e30;

	bool empty() const { return x0 > x1 || y0 > y1; }
	void add(double p_x, double p_y) {
		x0 = std::min(x0, p_x);
		y0 = std::min(y0, p_y);
		x1 = std::max(x1, p_x);
		y1 = std::max(y1, p_y);
	}
	void add(const SwfBounds &p_b) {
		if (!p_b.empty()) {
			add(p_b.x0, p_b.y0);
			add(p_b.x1, p_b.y1);
		}
	}
	SwfBounds intersect(const SwfBounds &p_b) const {
		SwfBounds r;
		r.x0 = std::max(x0, p_b.x0);
		r.y0 = std::max(y0, p_b.y0);
		r.x1 = std::min(x1, p_b.x1);
		r.y1 = std::min(y1, p_b.y1);
		return r;
	}
	SwfRect rect() const { return SwfRect{ float(x0), float(x1), float(y0), float(y1) }; }
};

template <class Map> SwfBounds swf_map_rect(const SwfRect &p_r, Map p_map) {
	SwfBounds b;
	const double xs[2] = { p_r.x_min, p_r.x_max };
	const double ys[2] = { p_r.y_min, p_r.y_max };
	for (const double x : xs) {
		for (const double y : ys) {
			const SwfPoint q = p_map(x, y);
			b.add(q.x, q.y);
		}
	}
	return b;
}

SwfBounds swf_project_rect(const SwfRect &p_r, const SwfAffine &p_m);

struct SwfFilterKernel {
	std::vector<double> x;
	std::vector<double> y;
	double shift_x = 0.0;
	double shift_y = 0.0;
	double margin_x = 0.0;
	double margin_y = 0.0;
	uint32_t clamped = 0;
};

bool swf_filter_supported(const SwfFilter &p_filter);
SwfFilterKernel swf_filter_kernel(const SwfFilter &p_filter, double p_scale_x, double p_scale_y);

struct SwfGridWarp {
	bool active = false;
	double bx0 = 0.0;
	double bx1 = 0.0;
	double by0 = 0.0;
	double by1 = 0.0;
	double l = 0.0;
	double r = 0.0;
	double t = 0.0;
	double b = 0.0;
	double sx = 1.0;
	double sy = 1.0;

	SwfPoint warp(SwfPoint p_p) const;
};

const SwfSceneNode *swf_grid_owner(
		const SwfScene &p_scene, const SwfSceneNode &p_node, SwfAffine *r_to_owner = nullptr);
SwfGridWarp swf_grid_warp(const SwfScene &p_scene, const SwfSceneNode &p_node, const SwfAffine &p_stage_to_window);

struct SwfNodePieces {
	uint32_t count = 0;
	bool has_bounds = false;
	SwfRect local;
	const SwfSceneNode *frame = nullptr;
};

SwfNodePieces swf_node_pieces(const SwfScene &p_scene, const SwfFontSet *p_fonts, const SwfSceneNode &p_node);

struct SwfRectMask {
	SwfRect stage;
};

std::optional<SwfRectMask> swf_rect_mask(
		const SwfScene &p_scene, const SwfFontSet *p_fonts, SwfNodeId p_mask, std::span<const SwfNodeId> p_tilted = {});

enum class SwfLayerKind : uint8_t {
	Filter,
	Mask,
	Blend,
};

enum class SwfLayerClass : uint8_t {
	Direct,
	Page,
	Clip,
	OwnSurface,
	Empty,
};

const char *swf_layer_kind_name(SwfLayerKind p_kind);
const char *swf_layer_class_name(SwfLayerClass p_class);

struct SwfLayerWalkNode {
	SwfNodeId owner = kSwfNoNode;
	SwfLayerKind kind = SwfLayerKind::Filter;
	SwfLayerClass route = SwfLayerClass::OwnSurface;
	std::string path;
	uint32_t parent = kSwfNoLayer;
	std::vector<uint32_t> children;
	std::vector<SwfFilter> filters;
	bool has_nodes = false;
	SwfRect nodes;
	bool has_mask = false;
	SwfRect mask;
};

struct SwfLayerWalk {
	std::vector<SwfLayerWalkNode> layers;
	SwfRect stage{ 0.0f, 1280.0f, 0.0f, 720.0f };
	uint32_t root_items = 0;
	uint32_t direct_blends = 0;
	uint32_t plain_text_fields = 0;
	uint32_t engine_targets = 0;
	uint32_t multi_filter_layers = 0;
	uint32_t non_rect_masks = 0;
};

struct SwfLayerWalkOptions {
	const SwfFontSet *fonts = nullptr;
	std::vector<std::string> tilted_paths;
	std::vector<std::string> target_paths;
};

SwfLayerWalk swf_layer_walk(const SwfScene &p_scene, const SwfLayerWalkOptions &p_options);
SwfLayerWalk swf_layer_walk(const HudScreen &p_screen);

struct SwfPlannedLayer {
	SwfNodeId owner = kSwfNoNode;
	SwfLayerKind kind = SwfLayerKind::Filter;
	SwfLayerClass route = SwfLayerClass::OwnSurface;
	std::string path;
	uint32_t filters = 0;
	uint32_t taps_x = 1;
	uint32_t taps_y = 1;
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = 1;
	int32_t height = 1;
	bool has_content = false;
	bool clamped = false;

	SwfLayerClass klass() const { return has_content ? route : SwfLayerClass::Empty; }
};

struct SwfLayerCounts {
	uint32_t pages = 0;
	uint32_t clips = 0;
	uint32_t own_surfaces = 0;
	uint32_t empty = 0;
	uint32_t non_empty_filters = 0;
	uint32_t distinct_kernels = 0;
	uint64_t page_slot_px = 0;
	uint64_t own_surface_bytes = 0;
};

struct SwfLayerPlan {
	std::vector<SwfPlannedLayer> layers;
	SwfLayerCounts counts;
};

SwfLayerPlan swf_layer_plan(const SwfLayerWalk &p_walk, int32_t p_window_width, int32_t p_window_height);

int32_t swf_page_slot_side(int32_t p_side);

struct SwfPageSlot {
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = 0;
	int32_t height = 0;
};

class SwfShelfPacker {
public:
	SwfShelfPacker(int32_t p_width, int32_t p_height);

	std::optional<SwfPageSlot> alloc(int32_t p_width, int32_t p_height);
	void release(const SwfPageSlot &p_slot);
	int32_t width() const { return width_; }
	int32_t height() const { return height_; }
	uint64_t used_px() const { return used_px_; }
	uint32_t failures() const { return failures_; }
	uint32_t live_slots() const { return live_slots_; }

private:
	struct Run {
		int32_t x = 0;
		int32_t width = 0;
	};
	struct Shelf {
		int32_t y = 0;
		int32_t height = 0;
		std::vector<Run> free;
	};

	bool whole(const Shelf &p_shelf) const;
	std::optional<SwfPageSlot> take(size_t p_shelf, int32_t p_width, int32_t p_height);

	int32_t width_ = 0;
	int32_t height_ = 0;
	std::vector<Shelf> shelves_;
	uint64_t used_px_ = 0;
	uint32_t failures_ = 0;
	uint32_t live_slots_ = 0;
};

struct SwfDirtyLayer {
	int32_t width = 0;
	int32_t height = 0;
};

struct SwfScratchPass {
	std::vector<size_t> taken;
	std::vector<SwfPageSlot> slots;
	std::vector<size_t> waiting;
	std::vector<size_t> too_large;
};

SwfScratchPass swf_select_scratch_pass(
		std::span<const SwfDirtyLayer> p_dirty, int32_t p_page_width, int32_t p_page_height);

struct SwfPageSizes {
	int32_t result_width = 0;
	int32_t result_height = 0;
	int32_t scratch_width = 0;
	int32_t scratch_height = 0;

	uint64_t result_px() const { return uint64_t(result_width) * uint64_t(result_height); }
	uint64_t result_bytes() const { return swf_target_bytes(result_width, result_height, kSwfTargetFactor1x); }
	uint64_t scratch_bytes() const { return swf_target_bytes(scratch_width, scratch_height, kSwfTargetFactor4x); }
};

SwfPageSizes swf_page_sizes(int32_t p_window_width, int32_t p_window_height);

}

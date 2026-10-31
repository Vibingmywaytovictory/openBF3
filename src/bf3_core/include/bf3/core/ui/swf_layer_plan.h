#pragma once

#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"

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
inline constexpr uint32_t kSwfNoLayer = 0xffffffffu;

uint64_t swf_target_bytes(int32_t p_width, int32_t p_height, uint64_t p_factor);

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

}

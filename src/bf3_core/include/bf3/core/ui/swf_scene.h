#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_draw.h"
#include "bf3/core/ui/swf_image.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "bf3/core/ui/swf_text.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr uint32_t kSwfSceneMaxNodes = 1u << 16;
inline constexpr uint32_t kSwfSceneMaxNesting = 32;
inline constexpr uint32_t kSwfSceneMaxImportHops = 8;
inline constexpr uint32_t kSwfSceneMaxJumpsPerNode = 64;
inline constexpr uint32_t kSwfSceneMaxCatchUpFrames = 8;
inline constexpr int32_t kSwfAs2DepthOffset = 16384;
inline constexpr int32_t kSwfAs2MaxDepth = 1048575 - kSwfAs2DepthOffset;
inline constexpr size_t kSwfSceneBudgetUnits = size_t(1) << 24;

using SwfNodeId = uint32_t;
inline constexpr SwfNodeId kSwfNoNode = 0xffffffffu;

struct SwfColorXform {
	std::array<float, 4> mul{ 1.0f, 1.0f, 1.0f, 1.0f };
	std::array<float, 4> add{ 0.0f, 0.0f, 0.0f, 0.0f };
	bool operator==(const SwfColorXform &) const = default;
};

SwfColorXform swf_color_xform(const SwfColorTransform &p_cx);
SwfColorXform swf_concat(const SwfColorXform &p_parent, const SwfColorXform &p_child);
SwfColor swf_apply(const SwfColorXform &p_cx, SwfColor p_color);

enum class SwfNodeKind : uint8_t {
	Root,
	Sprite,
	Shape,
	EditText,
	StaticText,
	Bitmap,
	Unresolved,
};
const char *swf_node_kind_name(SwfNodeKind p_kind);

namespace swf_change {
constexpr uint32_t kCreated = 1u << 0;
constexpr uint32_t kRemoved = 1u << 1;
constexpr uint32_t kMatrix = 1u << 2;
constexpr uint32_t kColor = 1u << 3;
constexpr uint32_t kVisible = 1u << 4;
constexpr uint32_t kText = 1u << 5;
constexpr uint32_t kCharacter = 1u << 6;
constexpr uint32_t kChildren = 1u << 7;
constexpr uint32_t kMask = 1u << 8;
constexpr uint32_t kFrame = 1u << 9;
constexpr uint32_t kEffects = 1u << 10;
}

struct SwfSceneNode {
	SwfNodeId id = kSwfNoNode;
	SwfNodeId parent = kSwfNoNode;
	SwfNodeKind kind = SwfNodeKind::Unresolved;
	std::shared_ptr<const SwfMovie> movie;
	std::string movie_name;
	uint16_t character = 0;
	uint16_t placed_character = 0;
	std::string symbol;
	std::string unresolved;
	std::string name;
	std::string path;
	int32_t depth = 0;
	bool dynamic = false;
	uint32_t placed_by = 0;
	uint32_t nesting = 0;

	SwfMatrix matrix;
	SwfColorXform color;
	uint16_t ratio = 0;
	uint16_t clip_depth = 0;
	uint8_t blend_mode = 0;
	std::vector<SwfFilter> filters;
	bool cache_as_bitmap = false;
	bool visible = true;
	bool clip_actions = false;
	bool script_matrix = false;
	bool script_color = false;
	bool script_visible = false;
	bool script_filters = false;

	const SwfTimeline *timeline = nullptr;
	uint32_t frame = 0;
	bool playing = true;
	const SwfShape *shape = nullptr;
	const SwfEditText *edit_text = nullptr;
	const SwfSubImage *sub_image = nullptr;
	const SwfScalingGrid *scaling_grid = nullptr;
	std::shared_ptr<const SwfShape> drawing;
	std::string image;
	uint32_t image_width = 0;
	uint32_t image_height = 0;
	std::string text;
	bool html = false;
	SwfTextOptions text_options;
	std::optional<SwfRect> text_bounds;
	std::optional<SwfColor> text_color;
	std::optional<float> text_size;

	std::vector<SwfNodeId> children;

	SwfMatrix world;
	SwfColorXform world_color;
	bool world_visible = true;
	bool is_mask = false;
	std::vector<SwfNodeId> masks;
	SwfNodeId script_mask = kSwfNoNode;

	uint32_t text_revision = 0;
	uint32_t character_revision = 0;
	uint32_t children_revision = 0;
	uint32_t effects_revision = 0;

	int32_t as2_depth() const { return depth - kSwfAs2DepthOffset; }
	uint32_t total_frames() const { return timeline != nullptr ? timeline->frames : 0; }
};

struct SwfSceneChange {
	SwfNodeId node = kSwfNoNode;
	uint32_t what = 0;
};

struct SwfSceneOptions {
	uint32_t max_nodes = kSwfSceneMaxNodes;
	bool run_frame_scripts = true;
};

struct SwfSceneStats {
	uint32_t live_nodes = 0;
	uint32_t node_slots = 0;
	uint32_t created = 0;
	uint32_t removed = 0;
	uint32_t unresolved = 0;
	uint32_t ticks = 0;
	uint32_t frames_dropped = 0;
	uint32_t scripts_run = 0;
	uint32_t ops_run = 0;
	uint32_t ops_conditional_skipped = 0;
	uint32_t scripts_with_other_actions = 0;
	uint32_t jumps_capped = 0;
	uint32_t labels_missing = 0;
	uint32_t clip_action_nodes = 0;
	uint32_t replays = 0;
};

class SwfScene {
public:
	static Result<SwfScene> open(const SwfLibrary &p_library, std::string_view p_movie, const SwfSceneOptions &p_options = {});

	SwfScene();
	~SwfScene();
	SwfScene(SwfScene &&) noexcept;
	SwfScene &operator=(SwfScene &&) noexcept;

	const std::string &movie_name() const;
	const SwfMovie &movie() const;
	float frame_rate() const;
	SwfRect stage() const;

	SwfNodeId root() const { return 0; }
	const SwfSceneNode *node(SwfNodeId p_id) const;
	SwfNodeId find(std::string_view p_path) const;
	const SwfSceneNode *find_node(std::string_view p_path) const;
	std::vector<SwfNodeId> draw_order() const;
	const SwfSceneStats &stats() const;

	Status advance(uint32_t p_frames = 1);
	Status advance_seconds(double p_seconds);

	Status goto_frame(std::string_view p_path, uint32_t p_frame, bool p_play);
	Status goto_label(std::string_view p_path, std::string_view p_label, bool p_play);
	Status play(std::string_view p_path);
	Status stop(std::string_view p_path);

	Result<SwfNodeId> attach_movie(std::string_view p_parent_path, std::string_view p_linkage, std::string_view p_name, int32_t p_as2_depth);
	Status remove_movie(std::string_view p_path);
	Status swap_depths(std::string_view p_path, std::string_view p_other_path);
	Status swap_depths(std::string_view p_path, int32_t p_as2_depth);
	Result<SwfNodeId> create_empty_movie_clip(std::string_view p_parent_path, std::string_view p_name, int32_t p_as2_depth);
	void set_frame_script_handler(std::function<void(SwfNodeId, uint32_t)> p_handler);

	Status set_visible(std::string_view p_path, bool p_visible);
	Status set_alpha(std::string_view p_path, float p_alpha);
	Status set_matrix(std::string_view p_path, const SwfMatrix &p_matrix);
	Status set_color(std::string_view p_path, const SwfColorXform &p_color);
	Status set_text(std::string_view p_path, std::string_view p_text, bool p_html = false);
	Status set_text_options(std::string_view p_path, const SwfTextOptions &p_options);
	Status set_text_bounds(std::string_view p_path, const SwfRect &p_bounds);
	Status set_text_color(std::string_view p_path, SwfColor p_color);
	Status set_text_size(std::string_view p_path, float p_size);
	Status set_filters(std::string_view p_path, std::vector<SwfFilter> p_filters);
	Status set_mask(std::string_view p_path, std::string_view p_mask_path);
	Status clear_mask(std::string_view p_path);
	Status draw(std::string_view p_path, const SwfDrawOp &p_op);
	Status load_image(std::string_view p_path, std::string_view p_res_name, uint32_t p_width, uint32_t p_height);
	Status unload_image(std::string_view p_path);
	void set_image_source(std::shared_ptr<SwfImageSource> p_source);
	SwfImageSource *image_source() const;

	std::vector<SwfSceneChange> take_changes();

	std::optional<SwfBitmapRef> bitmap(const SwfSceneNode &p_node, uint16_t p_bitmap_id) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

SwfRect swf_node_text_bounds(const SwfSceneNode &p_node);

Result<SwfRichText> swf_node_rich_text(const SwfSceneNode &p_node, WorkBudget &r_budget);

Result<SwfTextLayout> swf_layout_node_text(const SwfFontSet &p_fonts, const SwfSceneNode &p_node, WorkBudget &r_budget);

}

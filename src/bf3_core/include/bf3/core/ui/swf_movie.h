#pragma once

#include "bf3/core/level/budget.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

using level::WorkBudget;

inline constexpr uint32_t kSwfMaxBodyBytes = 16u << 20;

inline constexpr uint32_t kSwfMaxFrames = 1u << 16;
inline constexpr uint32_t kSwfMaxTags = 1u << 22;

inline constexpr uint32_t kSwfMaxDisplayEvents = 1u << 18;

inline constexpr size_t kSwfDisplayEventUnits = 8;

constexpr size_t swf_default_budget_units(size_t p_declared_length) {
	return (size_t(1) << 20) + p_declared_length * 2;
}

namespace swf_tag {
constexpr uint16_t kEnd = 0;
constexpr uint16_t kShowFrame = 1;
constexpr uint16_t kDefineShape = 2;
constexpr uint16_t kPlaceObject = 4;
constexpr uint16_t kRemoveObject = 5;
constexpr uint16_t kSetBackgroundColor = 9;
constexpr uint16_t kDefineText = 11;
constexpr uint16_t kDoAction = 12;
constexpr uint16_t kDefineShape2 = 22;
constexpr uint16_t kPlaceObject2 = 26;
constexpr uint16_t kRemoveObject2 = 28;
constexpr uint16_t kDefineShape3 = 32;
constexpr uint16_t kDefineEditText = 37;
constexpr uint16_t kDefineSprite = 39;
constexpr uint16_t kFrameLabel = 43;
constexpr uint16_t kDefineFont2 = 48;
constexpr uint16_t kExportAssets = 56;
constexpr uint16_t kImportAssets = 57;
constexpr uint16_t kDoInitAction = 59;
constexpr uint16_t kFileAttributes = 69;
constexpr uint16_t kPlaceObject3 = 70;
constexpr uint16_t kImportAssets2 = 71;
constexpr uint16_t kDefineFontAlignZones = 73;
constexpr uint16_t kCsmTextSettings = 74;
constexpr uint16_t kDefineFont3 = 75;
constexpr uint16_t kMetadata = 77;
constexpr uint16_t kDefineScalingGrid = 78;
constexpr uint16_t kDoAbc = 82;
constexpr uint16_t kDefineShape4 = 83;
constexpr uint16_t kDefineFontName = 88;
constexpr uint16_t kGfxExporterInfo = 1000;
constexpr uint16_t kGfxDefineExternalImage = 1001;
constexpr uint16_t kGfxFontTextureInfo = 1002;
constexpr uint16_t kGfxDefineExternalGradient = 1003;
constexpr uint16_t kGfxDefineGradientMap = 1004;
constexpr uint16_t kGfxDefineCompactedFont = 1005;
constexpr uint16_t kGfxDefineExternalSound = 1006;
constexpr uint16_t kGfxDefineExternalStreamSound = 1007;
constexpr uint16_t kGfxDefineSubImage = 1008;
constexpr uint16_t kGfxDefineExternalImage2 = 1009;
}

const char *swf_tag_name(uint16_t p_code);

struct SwfMatrix {
	float a = 1.0f;
	float b = 0.0f;
	float c = 0.0f;
	float d = 1.0f;
	float tx = 0.0f;
	float ty = 0.0f;

	bool operator==(const SwfMatrix &) const = default;
};

constexpr SwfMatrix swf_concat(const SwfMatrix &p_parent, const SwfMatrix &p_child) {
	SwfMatrix m;
	m.a = p_parent.a * p_child.a + p_parent.c * p_child.b;
	m.b = p_parent.b * p_child.a + p_parent.d * p_child.b;
	m.c = p_parent.a * p_child.c + p_parent.c * p_child.d;
	m.d = p_parent.b * p_child.c + p_parent.d * p_child.d;
	m.tx = p_parent.a * p_child.tx + p_parent.c * p_child.ty + p_parent.tx;
	m.ty = p_parent.b * p_child.tx + p_parent.d * p_child.ty + p_parent.ty;
	return m;
}

struct SwfRect {
	float x_min = 0.0f;
	float x_max = 0.0f;
	float y_min = 0.0f;
	float y_max = 0.0f;

	float width() const { return x_max - x_min; }
	float height() const { return y_max - y_min; }
	bool operator==(const SwfRect &) const = default;
};

struct SwfColor {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
	uint8_t a = 255;
	bool operator==(const SwfColor &) const = default;
};

struct SwfColorTransform {
	std::array<int16_t, 4> mul{ 256, 256, 256, 256 };
	std::array<int16_t, 4> add{ 0, 0, 0, 0 };
	bool operator==(const SwfColorTransform &) const = default;
};

struct SwfFrameLabel {
	uint32_t frame = 0;

	std::string name;
};

enum class SwfDisplayOp : uint8_t {
	Place,

	Modify,
	Replace,
	Remove,
};

struct SwfDisplayEvent {
	uint32_t frame = 0;
	uint16_t depth = 0;
	SwfDisplayOp op = SwfDisplayOp::Place;
	uint8_t tag_version = 2;
	uint16_t character = 0;
	bool has_matrix = false;
	SwfMatrix matrix;
	bool has_color = false;
	SwfColorTransform color;
	bool has_ratio = false;
	uint16_t ratio = 0;
	uint16_t clip_depth = 0;
	bool has_name = false;
	std::string name;
	std::string class_name;
	uint8_t blend_mode = 0;
	bool has_filters = false;
};

struct SwfTimeline {
	uint16_t declared_frames = 0;
	uint32_t frames = 0;
	std::vector<SwfFrameLabel> labels;
	std::vector<SwfDisplayEvent> events;
	uint32_t do_actions = 0;
	uint64_t action_bytes = 0;
};

struct SwfSprite {
	uint16_t id = 0;
	SwfTimeline timeline;
};

struct SwfDisplayEntry {
	uint16_t depth = 0;
	uint16_t character = 0;
	SwfMatrix matrix;
	SwfColorTransform color;
	uint16_t clip_depth = 0;
	std::string_view name;
};

Status display_lists_at(const SwfTimeline &p_timeline, std::span<const uint32_t> p_frames, WorkBudget &r_budget,
		std::vector<std::vector<SwfDisplayEntry>> &r_lists);

enum class SwfCharacterKind : uint8_t {
	None,
	Shape,
	Sprite,
	EditText,
	StaticText,
	Font,
	SubImage,
	Imported,
};
const char *swf_character_kind_name(SwfCharacterKind p_kind);

struct SwfExport {
	uint16_t id = 0;
	std::string name;
};

struct SwfImport {
	std::string url;
	uint8_t tag_version = 2;
	std::vector<SwfExport> symbols;
};

struct SwfExporterInfo {
	bool present = false;
	uint16_t version = 0;
	uint32_t flags = 0;
	uint16_t bitmap_format = 0;
	std::string prefix;
	std::string swf_name;
	uint32_t extra_bytes = 0;
};

struct SwfExternalImage {
	uint16_t image_id = 0;
	uint16_t field_2 = 0;
	uint16_t bitmap_format = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	std::string export_name;
	std::string file_name;
	uint8_t tag_version = 2;
	uint32_t extra_bytes = 0;
};

struct SwfSubImage {
	uint16_t id = 0;
	uint16_t image_id = 0;
	uint16_t x1 = 0;
	uint16_t y1 = 0;
	uint16_t x2 = 0;
	uint16_t y2 = 0;

	uint16_t width() const { return uint16_t(x2 - x1); }
	uint16_t height() const { return uint16_t(y2 - y1); }
};

inline constexpr float kSwfFlattenTolerance = 0.025f;
inline constexpr uint32_t kSwfCurveMaxSegments = 256;
inline constexpr uint32_t kSwfShapeMaxPoints = 1u << 18;
inline constexpr uint32_t kSwfShapeMaxSlabs = 1u << 16;

enum class SwfFillKind : uint8_t {
	Solid,
	LinearGradient,
	RadialGradient,
	FocalGradient,
	Bitmap,
};
const char *swf_fill_kind_name(SwfFillKind p_kind);

struct SwfGradientStop {
	uint8_t ratio = 0;
	SwfColor color;
	bool operator==(const SwfGradientStop &) const = default;
};

struct SwfFillStyle {
	static constexpr uint8_t kRepeatingBitmap = 0x40;
	static constexpr uint8_t kClippedBitmap = 0x41;
	static constexpr uint8_t kRepeatingBitmapNoSmoothing = 0x42;
	static constexpr uint8_t kClippedBitmapNoSmoothing = 0x43;

	uint8_t type = 0;
	SwfFillKind kind = SwfFillKind::Solid;
	SwfColor color;
	uint16_t bitmap_id = 0;
	SwfMatrix matrix;
	uint8_t spread = 0;
	uint8_t interpolation = 0;
	float focal = 0.0f;
	std::vector<SwfGradientStop> stops;

	bool repeating() const { return type == kRepeatingBitmap || type == kRepeatingBitmapNoSmoothing; }
	bool clipped() const { return type == kClippedBitmap || type == kClippedBitmapNoSmoothing; }
	bool smoothed() const { return type == kRepeatingBitmap || type == kClippedBitmap; }
	SwfMatrix texel_to_shape() const;
};

struct SwfLineStyle {
	float width = 0.0f;
	SwfColor color;
	uint8_t start_cap = 0;
	uint8_t end_cap = 0;
	uint8_t join = 0;
	float miter_limit = 0.0f;
	bool no_hscale = false;
	bool no_vscale = false;
	bool pixel_hinting = false;
	bool no_close = false;
	bool has_fill = false;
	SwfFillStyle fill;
};

struct SwfPoint {
	float x = 0.0f;
	float y = 0.0f;
	bool operator==(const SwfPoint &) const = default;
};

struct SwfFillMesh {
	uint32_t fill = 0;
	std::vector<SwfPoint> outline;
	std::vector<uint32_t> contour_ends;
	uint32_t open_contours = 0;
	std::vector<SwfPoint> vertices;
	std::vector<uint32_t> indices;

	double outline_area() const;
	double triangle_area() const;
};

struct SwfStroke {
	uint32_t line = 0;
	std::vector<SwfPoint> points;
	std::vector<uint32_t> path_ends;
	std::vector<bool> closed;
};

struct SwfShape {
	static constexpr uint8_t kNonScalingStrokes = 0x02;
	static constexpr uint8_t kScalingStrokes = 0x01;
	static constexpr uint8_t kFillWindingRule = 0x04;

	uint16_t id = 0;
	uint8_t version = 1;
	SwfRect bounds;
	SwfRect edge_bounds;
	uint8_t shape4_flags = 0;
	std::vector<SwfFillStyle> fills;
	std::vector<SwfLineStyle> lines;
	uint32_t edges = 0;
	uint32_t style_tables = 0;
	std::vector<SwfFillMesh> meshes;
	std::vector<SwfStroke> strokes;

	const SwfFillMesh *mesh_of(uint32_t p_fill) const;
};

struct SwfEditText {

	static constexpr uint16_t kHasText = 0x8000;
	static constexpr uint16_t kWordWrap = 0x4000;
	static constexpr uint16_t kMultiline = 0x2000;
	static constexpr uint16_t kPassword = 0x1000;
	static constexpr uint16_t kReadOnly = 0x0800;
	static constexpr uint16_t kHasTextColor = 0x0400;
	static constexpr uint16_t kHasMaxLength = 0x0200;
	static constexpr uint16_t kHasFont = 0x0100;
	static constexpr uint16_t kHasFontClass = 0x0080;
	static constexpr uint16_t kAutoSize = 0x0040;
	static constexpr uint16_t kHasLayout = 0x0020;
	static constexpr uint16_t kNoSelect = 0x0010;
	static constexpr uint16_t kBorder = 0x0008;
	static constexpr uint16_t kWasStatic = 0x0004;
	static constexpr uint16_t kHtml = 0x0002;
	static constexpr uint16_t kUseOutlines = 0x0001;

	uint16_t id = 0;
	SwfRect bounds;
	uint16_t flags = 0;
	uint16_t font_id = 0;
	std::string font_class;
	float height = 0.0f;
	SwfColor color;
	uint16_t max_length = 0;
	uint8_t align = 0;
	float left_margin = 0.0f;
	float right_margin = 0.0f;
	float indent = 0.0f;
	float leading = 0.0f;
	std::string variable;
	std::string initial_text;

	bool has(uint16_t p_flag) const { return (flags & p_flag) != 0; }
};

struct SwfScalingGrid {
	uint16_t id = 0;
	SwfRect splitter;
};

enum class SwfFontKind : uint8_t {
	Outlines,
	NoGlyphs,
	CodesOnly,
};
const char *swf_font_kind_name(SwfFontKind p_kind);

struct SwfFontInfo {
	uint16_t id = 0;
	uint16_t tag_code = swf_tag::kDefineFont3;
	std::string name;
	uint8_t flags = 0;

	uint8_t language = 0;
	uint16_t glyph_count = 0;
	SwfFontKind kind = SwfFontKind::Outlines;
	uint32_t trailing_bytes = 0;
	uint32_t tag_offset = 0;
	uint32_t tag_length = 0;
};

struct SwfFontAlignZones {
	uint16_t font_id = 0;
	uint8_t csm_hint = 0;
	uint32_t glyphs = 0;
};

struct SwfFontName {
	uint16_t font_id = 0;
	std::string name;
	std::string copyright;
};

struct SwfTagCount {
	uint16_t code = 0;
	uint32_t count = 0;
};

struct SwfMovie {

	bool compressed = false;
	uint8_t version = 0;
	uint32_t file_length = 0;
	SwfRect stage;
	float frame_rate = 0.0f;
	uint16_t frame_count = 0;
	std::vector<uint8_t> body;

	SwfTimeline root;
	std::vector<SwfSprite> sprites;
	std::vector<SwfExport> exports;
	std::vector<SwfImport> imports;
	SwfExporterInfo exporter;
	std::vector<SwfExternalImage> external_images;
	std::vector<SwfSubImage> sub_images;
	std::vector<SwfShape> shapes;
	std::vector<SwfEditText> edit_texts;
	std::vector<uint16_t> static_texts;
	std::vector<SwfScalingGrid> scaling_grids;
	std::vector<SwfFontInfo> fonts;
	std::vector<SwfFontAlignZones> font_align_zones;
	std::vector<SwfFontName> font_names;
	uint32_t csm_text_settings = 0;

	std::vector<SwfTagCount> tag_counts;
	uint64_t action_bytes = 0;
	uint32_t do_init_actions = 0;
	uint64_t abc_bytes = 0;
	uint32_t unknown_tags = 0;
	uint32_t trailing_bytes = 0;
	size_t budget_used = 0;
	size_t retained_bytes = 0;

	static Result<SwfMovie> parse(std::span<const uint8_t> p_payload, WorkBudget *p_budget = nullptr);

	const SwfSprite *find_sprite(uint16_t p_id) const;
	const SwfShape *find_shape(uint16_t p_id) const;
	const SwfSubImage *find_sub_image(uint16_t p_id) const;
	const SwfEditText *find_edit_text(uint16_t p_id) const;
	const SwfFontInfo *find_font(uint16_t p_id) const;
	const SwfExternalImage *find_external_image(uint16_t p_image_id) const;

	const SwfImport *find_import(uint16_t p_id, const SwfExport **r_symbol = nullptr) const;
	SwfCharacterKind character_kind(uint16_t p_id) const;

	std::optional<uint16_t> export_id(std::string_view p_name) const;
	std::string_view export_name(uint16_t p_id) const;

	std::span<const uint8_t> font_tag(const SwfFontInfo &p_font) const;
	uint32_t tag_count(uint16_t p_code) const;
};

}

#pragma once
// BF3's SwfMovie res (resType 0x2D47A5FF) read as a symbol table: Scaleform GFx movies (SWF 7/8 plus the GFx
// extension tags), clean-room from the public SWF file format specification (v19) and the tag layouts measured on
// the retail install (docs/design/ui-swf-fonts.md §4; plan §3.8, D16; research gap-r1-ui-hud-minimap-localisation
// §3). No ActionScript is executed and no shape is tessellated: the reader keeps what a C++ Control layout needs
// (timelines, frame labels, placements with matrices and instance names, exports/imports, the GFx external atlas
// and its sub-image rectangles, scaling grids, edit-text fields, fonts) and counts the rest.
//
// Payload (the whole res payload; the 16-byte resMeta is zero on all 434 retail movies):
//   0x00 "CFX" (body zlib-compressed) or "GFX" (stored) | u8 version (8; one movie has 7) | u32 file length
//   (header + body, decompressed); then the body: RECT stage (twips) | u16 frame rate 8.8 | u16 frame count | tags.
// Tags: u16 code << 6 | length (0x3f = a u32 length follows). DefineSprite (39) nests a timeline of control tags;
// sprites never nest in retail data (a DefineSprite inside a sprite is an error here).
//
// GFx extension tags as measured (all 434 movies):
//   1000 ExporterInfo        u16 version (0x36f) | u32 flags (version >= 0x10a) | u16 bitmap format (13) |
//                            u8-prefixed prefix | u8-prefixed swf name | the rest is kept as extra bytes
//   1009 DefineExternalImage2 u16 image id | u16 (9 on all 135) | u16 bitmap format (13) | u16 width | u16 height |
//                            u8-prefixed export name (empty) | u8-prefixed file name ("<swf>_iN.tga") | u8 (0)
//   1001 DefineExternalImage  u16 image id | u16 format | u16 width | u16 height | names as above (0 in retail)
//   1008 DefineSubImage       u16 character id | u16 image id | u16 x1 | u16 y1 | u16 x2 | u16 y2 (atlas pixels,
//                            x2 and y2 exclusive: 737 shapes filled by one have exactly (x2-x1) x (y2-y1) px bounds)
// A shape fills a sub-image like any bitmap (fill type 0x41/0x43, bitmap id = the sub-image's character id).
//
// Units: everything in this header is in pixels (twips / 20) unless a field says twips. Matrices map a child's
// space into its parent's: x' = a*x + c*y + tx, y' = b*x + d*y + ty (SWF ScaleX, RotateSkew0, RotateSkew1, ScaleY,
// TranslateX/Y); y grows downwards as in Flash.
//
// Hostile input (docs §9): every offset and length is checked; the body is at most kSwfMaxBodyBytes and the CFX
// stream must inflate to exactly the declared length; every tag, record, string byte and display event charges
// the WorkBudget before it is used (default budget: swf_default_budget_units(declared length); a display event
// costs 8 units, about its size / 16, so the budget bounds memory too), at most kSwfMaxDisplayEvents events per
// movie, and a DefineSubImage with x2 < x1 or y2 < y1 is refused. Parse time and memory are linear in the payload.
// Errors are Result values ("swf: <what> at <offset>").

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

// Largest decompressed body accepted (retail maximum: 4,997,806 bytes, the zh EULA font library; 3.4x margin).
inline constexpr uint32_t kSwfMaxBodyBytes = 16u << 20;
// Structural caps (retail maxima in brackets): frames per timeline (526), tags per movie (8,300).
inline constexpr uint32_t kSwfMaxFrames = 1u << 16;
inline constexpr uint32_t kSwfMaxTags = 1u << 22;
// PlaceObject/RemoveObject events per movie, root and sprites (retail maximum: 4,075 PlaceObject events, compass):
// bounds the SwfDisplayEvent memory of a hostile body of tiny tags to about 34 MB.
inline constexpr uint32_t kSwfMaxDisplayEvents = 1u << 18;
// Budget units one display event costs (about sizeof(SwfDisplayEvent) / 16).
inline constexpr size_t kSwfDisplayEventUnits = 8;

// Default work budget of one parse: 2 units per byte of the header's declared (decompressed) length, which is
// checked against kSwfMaxBodyBytes before anything is inflated, plus a constant. A tag, a shape record (>= 6
// bits) or 16 string bytes cost one unit each and a display event kSwfDisplayEventUnits, so retail movies use about
// half of it (the census prints the peak, docs §9.1).
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
} // namespace swf_tag

// "PlaceObject2", "GFX_DefineSubImage", ...; nullptr for a code this reader does not name.
const char *swf_tag_name(uint16_t p_code);

struct SwfMatrix {
	float a = 1.0f; // ScaleX
	float b = 0.0f; // RotateSkew0
	float c = 0.0f; // RotateSkew1
	float d = 1.0f; // ScaleY
	float tx = 0.0f; // pixels
	float ty = 0.0f; // pixels

	bool operator==(const SwfMatrix &) const = default;
};

// p_child placed inside p_parent: the matrix that maps the child's space straight into p_parent's parent.
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

// CXFORMWITHALPHA: multiply terms in 8.8 (256 = 1.0), add terms in 0..255 units; r, g, b, a.
struct SwfColorTransform {
	std::array<int16_t, 4> mul{ 256, 256, 256, 256 };
	std::array<int16_t, 4> add{ 0, 0, 0, 0 };
	bool operator==(const SwfColorTransform &) const = default;
};

struct SwfFrameLabel {
	uint32_t frame = 0; // 0-based frame the label names (the frame its ShowFrame ends); may equal the timeline's
	                    // frames when no ShowFrame follows (0 retail labels; display_lists_at accepts it)
	std::string name;
};

enum class SwfDisplayOp : uint8_t {
	Place, // a character at a depth (PlaceObject, PlaceObject2/3 without the move flag); normally an empty depth,
	       // and a Place on an occupied depth replaces what is there, as Flash players do (docs §5.1)
	Modify, // move flag without a character: matrix / colour / ratio / name of the depth's character change
	Replace, // move flag with a character: the depth's character is swapped, unset fields kept
	Remove, // RemoveObject / RemoveObject2
};

// One display-list change, in tag order.
struct SwfDisplayEvent {
	uint32_t frame = 0;
	uint16_t depth = 0;
	SwfDisplayOp op = SwfDisplayOp::Place;
	uint8_t tag_version = 2; // 1 PlaceObject, 2 PlaceObject2, 3 PlaceObject3; RemoveObject: 1 or 2
	uint16_t character = 0; // Place / Replace
	bool has_matrix = false;
	SwfMatrix matrix;
	bool has_color = false;
	SwfColorTransform color;
	bool has_ratio = false;
	uint16_t ratio = 0;
	uint16_t clip_depth = 0; // 0 = not a mask
	bool has_name = false;
	std::string name; // instance name (AS2 target), e.g. "Ammo_01"
	std::string class_name; // PlaceObject3 class name (never set in retail)
	uint8_t blend_mode = 0; // PlaceObject3; 0 = not given
	bool has_filters = false; // PlaceObject3 filter list present (kept as a flag only)
};

struct SwfTimeline {
	uint16_t declared_frames = 0; // the header's / DefineSprite's FrameCount
	uint32_t frames = 0; // ShowFrame tags seen
	std::vector<SwfFrameLabel> labels; // tag order
	std::vector<SwfDisplayEvent> events; // tag order
	uint32_t do_actions = 0; // DoAction tags on this timeline
	uint64_t action_bytes = 0; // AS2 bytecode bytes of those DoActions
};

struct SwfSprite {
	uint16_t id = 0;
	SwfTimeline timeline;
};

// One entry of a timeline's display list at some frame.
struct SwfDisplayEntry {
	uint16_t depth = 0;
	uint16_t character = 0;
	SwfMatrix matrix;
	SwfColorTransform color;
	uint16_t clip_depth = 0;
	std::string_view name; // points into the timeline's event (keep the SwfMovie alive)
};

// The display lists of p_timeline after the given frames (0-based; each frame index <= timeline.frames, where
// timeline.frames means the state after every event), in ONE pass over the events: O(events + sum of the returned
// list sizes), every event and every copied entry charged. A frame > timeline.frames is an error.
// r_lists[i] belongs to p_frames[i] (any order, duplicates allowed); entries sorted by depth.
Status display_lists_at(const SwfTimeline &p_timeline, std::span<const uint32_t> p_frames, WorkBudget &r_budget,
		std::vector<std::vector<SwfDisplayEntry>> &r_lists);

enum class SwfCharacterKind : uint8_t {
	None,
	Shape,
	Sprite,
	EditText,
	StaticText, // DefineText (kept as an id only)
	Font,
	SubImage,
	Imported, // an ImportAssets(2) symbol: defined in another movie
};
const char *swf_character_kind_name(SwfCharacterKind p_kind);

struct SwfExport {
	uint16_t id = 0;
	std::string name;
};

struct SwfImport {
	std::string url; // as written, e.g. "Ammo.swf", "../Static/gfxfontlib.swf"
	uint8_t tag_version = 2; // ImportAssets (1) or ImportAssets2 (2)
	std::vector<SwfExport> symbols; // local character id -> the exporting movie's symbol name
};

struct SwfExporterInfo {
	bool present = false;
	uint16_t version = 0;
	uint32_t flags = 0;
	uint16_t bitmap_format = 0;
	std::string prefix;
	std::string swf_name; // e.g. "IconsIngame"
	uint32_t extra_bytes = 0;
};

// GFx external image: an atlas texture the engine supplies (BF3: DxTexture "ui/assets/<swf>/<file stem>").
struct SwfExternalImage {
	uint16_t image_id = 0;
	uint16_t field_2 = 0; // DefineExternalImage2's second u16: 9 on every retail tag, meaning unknown
	uint16_t bitmap_format = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	std::string export_name;
	std::string file_name; // e.g. "iconsingame_i1.tga"
	uint8_t tag_version = 2; // 1 = DefineExternalImage, 2 = DefineExternalImage2
	uint32_t extra_bytes = 0;
};

// A rectangle of an external image, usable as a bitmap character. The parser guarantees x1 <= x2 and y1 <= y2 (a
// reversed rectangle is a parse error), so width() / height() never wrap; inside the image is checked by the census
// and the atlas map (rects_outside_atlas).
struct SwfSubImage {
	uint16_t id = 0; // character id
	uint16_t image_id = 0; // SwfExternalImage::image_id
	uint16_t x1 = 0;
	uint16_t y1 = 0;
	uint16_t x2 = 0; // exclusive
	uint16_t y2 = 0; // exclusive

	uint16_t width() const { return uint16_t(x2 - x1); }
	uint16_t height() const { return uint16_t(y2 - y1); }
};

// A bitmap fill style of a shape (fill types 0x40-0x43). The matrix maps bitmap pixels into the shape:
// a..d in twips per bitmap pixel (20 = 1:1), tx / ty in pixels.
struct SwfBitmapFill {
	uint16_t bitmap_id = 0; // a sub-image id; 65535 = no bitmap (seen on clipped fills)
	uint8_t fill_type = 0x41;
	SwfMatrix matrix;
};

struct SwfShape {
	uint16_t id = 0;
	uint8_t version = 1; // DefineShape 1..4
	SwfRect bounds;
	std::vector<SwfBitmapFill> bitmap_fills; // from the initial style arrays and every StyleChangeRecord
	uint32_t solid_fills = 0;
	uint32_t gradient_fills = 0;
	uint32_t line_styles = 0;
	uint32_t edges = 0;
};

struct SwfEditText {
	// DefineEditText flags as (first byte << 8 | second byte).
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
	uint16_t font_id = 0; // a DefineFont3 id or an imported font symbol ("$HUD_medium")
	std::string font_class;
	float height = 0.0f; // font size in pixels
	SwfColor color;
	uint16_t max_length = 0;
	uint8_t align = 0; // 0 left, 1 right, 2 centre, 3 justify
	float left_margin = 0.0f;
	float right_margin = 0.0f;
	float indent = 0.0f;
	float leading = 0.0f;
	std::string variable;
	std::string initial_text; // often a SID ("ID_H_RELOAD_HINT") or placeholder digits ("000")

	bool has(uint16_t p_flag) const { return (flags & p_flag) != 0; }
};

struct SwfScalingGrid {
	uint16_t id = 0; // the sprite or button it nine-slices
	SwfRect splitter; // the centre rectangle in the character's space
};

enum class SwfFontKind : uint8_t {
	Outlines, // glyph shapes present (the font libraries: 63 retail tags)
	NoGlyphs, // NumGlyphs 0: a name-only reference to a device or library font (21 retail tags)
	CodesOnly, // GFx stripped form: OffsetTable[0] = 0, then CodeTableOffset and the codes, no shapes (2 tags)
};
const char *swf_font_kind_name(SwfFontKind p_kind);

// DefineFont2/3 header facts; the outlines are read on demand (swf_font.h) from [tag_offset, +tag_length).
struct SwfFontInfo {
	uint16_t id = 0;
	uint16_t tag_code = swf_tag::kDefineFont3;
	std::string name; // e.g. "Purista EA Medium", "$FONT_TITLE*", "Arial"
	uint8_t flags = 0; // 0x80 HasLayout, 0x40 ShiftJIS, 0x20 SmallText, 0x10 ANSI, 0x08 WideOffsets,
	                   // 0x04 WideCodes, 0x02 Italic, 0x01 Bold
	uint8_t language = 0;
	uint16_t glyph_count = 0;
	SwfFontKind kind = SwfFontKind::Outlines;
	uint32_t trailing_bytes = 0; // bytes after the last field the reader understands (NoGlyphs tags carry some)
	uint32_t tag_offset = 0; // into SwfMovie::body
	uint32_t tag_length = 0;
};

struct SwfFontAlignZones {
	uint16_t font_id = 0;
	uint8_t csm_hint = 0;
	uint32_t glyphs = 0; // zone records (none in retail)
};

struct SwfFontName {
	uint16_t font_id = 0;
	std::string name;
	std::string copyright; // none in retail
};

struct SwfTagCount {
	uint16_t code = 0;
	uint32_t count = 0;
};

// One parsed movie. Plain data; every vector is sorted by id where it has one. Owns its decompressed body.
struct SwfMovie {
	// Header.
	bool compressed = false; // "CFX"
	uint8_t version = 0;
	uint32_t file_length = 0;
	SwfRect stage; // retail HUD and menu screens: 0..1280 x 0..720
	float frame_rate = 0.0f;
	uint16_t frame_count = 0;
	std::vector<uint8_t> body; // decompressed, after the 8-byte header

	SwfTimeline root;
	std::vector<SwfSprite> sprites;
	std::vector<SwfExport> exports; // ExportAssets, tag order
	std::vector<SwfImport> imports;
	SwfExporterInfo exporter;
	std::vector<SwfExternalImage> external_images;
	std::vector<SwfSubImage> sub_images;
	std::vector<SwfShape> shapes;
	std::vector<SwfEditText> edit_texts;
	std::vector<uint16_t> static_texts; // DefineText ids
	std::vector<SwfScalingGrid> scaling_grids;
	std::vector<SwfFontInfo> fonts;
	std::vector<SwfFontAlignZones> font_align_zones;
	std::vector<SwfFontName> font_names;
	uint32_t csm_text_settings = 0;

	std::vector<SwfTagCount> tag_counts; // every code seen (root and sprites), sorted by code
	uint64_t action_bytes = 0; // AS2: DoAction lengths + DoInitAction lengths - 2 (the sprite id)
	uint32_t do_init_actions = 0;
	uint64_t abc_bytes = 0; // DoABC (AS3), 0 in retail
	uint32_t unknown_tags = 0; // codes swf_tag_name() does not name
	uint32_t trailing_bytes = 0; // after the root End tag
	size_t budget_used = 0; // units this parse charged
	size_t retained_bytes = 0; // estimate of what this movie holds: body + vector capacities x element sizes +
	                           // string capacities (the census prints the maximum and the sum)

	// Validates and decompresses p_payload and reads every tag (see the header comment). p_budget null: a local
	// budget of swf_default_budget_units(declared file length) (the size SwfLibrary::load and the census use).
	static Result<SwfMovie> parse(std::span<const uint8_t> p_payload, WorkBudget *p_budget = nullptr);

	const SwfSprite *find_sprite(uint16_t p_id) const;
	const SwfShape *find_shape(uint16_t p_id) const;
	const SwfSubImage *find_sub_image(uint16_t p_id) const;
	const SwfEditText *find_edit_text(uint16_t p_id) const;
	const SwfFontInfo *find_font(uint16_t p_id) const;
	const SwfExternalImage *find_external_image(uint16_t p_image_id) const;
	// The import that defines local id p_id (and its symbol), or nullptr.
	const SwfImport *find_import(uint16_t p_id, const SwfExport **r_symbol = nullptr) const;
	SwfCharacterKind character_kind(uint16_t p_id) const;
	// First export of that name / the name id p_id is exported under (empty when none).
	std::optional<uint16_t> export_id(std::string_view p_name) const;
	std::string_view export_name(uint16_t p_id) const;
	// The bytes of a font's tag (DefineFont2/3 body) for swf_font.h.
	std::span<const uint8_t> font_tag(const SwfFontInfo &p_font) const;
	uint32_t tag_count(uint16_t p_code) const;
};

} // namespace bf3::core::ui

#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/ui/swf_atlas.h"
#include "bf3/core/ui/swf_census.h"
#include "bf3/core/ui/swf_font.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "bf3/core/ui/ui_fonts.h"
#include "test_support.h"
#include "ui_swf_writer.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui1test;

namespace {

using Clock = std::chrono::steady_clock;

double seconds_of(const std::function<void()> &p_fn) {
	const auto t0 = Clock::now();
	p_fn();
	return std::chrono::duration<double>(Clock::now() - t0).count();
}

void hostile(const char *p_name, const std::function<void()> &p_fn) {
	const double s = seconds_of(p_fn);
	MESSAGE(std::string(p_name) << ": " << s << " s" << std::string(kAsan ? " (asan, reported)" : ""));
	if (!kAsan) {
		CHECK_MESSAGE(s < 1.0, p_name);
	}
}

std::string error_of(const Bytes &p_payload) {
	Result<SwfMovie> m = SwfMovie::parse(p_payload);
	return m ? std::string() : m.error().message;
}

SwfMovie parse_ok(const Bytes &p_payload) {
	Result<SwfMovie> m = SwfMovie::parse(p_payload);
	REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
	return std::move(*m);
}

bool near(float p_a, float p_b, float p_tol = 0.001f) {
	return std::fabs(p_a - p_b) <= p_tol;
}

Mat translate(float p_x, float p_y) {
	Mat m;
	m.tx = p_x;
	m.ty = p_y;
	return m;
}
Mat scale_at(float p_sx, float p_sy, float p_x, float p_y) {
	Mat m;
	m.a = p_sx;
	m.d = p_sy;
	m.tx = p_x;
	m.ty = p_y;
	return m;
}

Font3 square_font() {
	Font3 f;
	f.id = 40;
	Glyph g;
	g.push_back(Contour{ 1000, -10000, { { 9000, -10000 }, { 9000, 0 }, { 1000, 0 }, { 1000, -10000 } } });
	f.glyphs = { g, g };
	f.codes = { 0x41, 0x42 };
	f.advances = { 10240, 10240 };
	f.kerning = { { 0x41, 0x42, -500 } };
	return f;
}

Bytes rich_tags() {
	Place p2;
	p2.depth = 2;
	p2.character = 51;
	p2.matrix = scale_at(2, 2, 100, 50);
	p2.color = true;
	p2.ratio = 7;
	p2.name = "wrap";
	Place p3;
	p3.depth = 3;
	p3.character = 21;
	p3.class_name = "Cls";
	p3.filters = true;
	p3.blend = 3;
	p3.name = "p3";
	Place modify;
	modify.depth = 2;
	modify.move = true;
	modify.matrix = translate(200, 0);
	Place replace;
	replace.depth = 3;
	replace.move = true;
	replace.character = 22;
	Place clip;
	clip.depth = 4;
	clip.character = 23;
	clip.clip_depth = 9;
	clip.clip_actions = true;
	Place over;
	over.depth = 4;
	over.character = 20;

	EditText e1;
	e1.id = 30;
	e1.font_id = 40;
	e1.height = 21;
	e1.max_length = 12;
	EditText e2;
	e2.id = 31;
	e2.font_class = "$HUD_medium";
	e2.color = false;
	e2.layout = false;
	e2.text = std::nullopt;
	e2.extra_flags = SwfEditText::kWordWrap | SwfEditText::kMultiline | SwfEditText::kReadOnly | SwfEditText::kAutoSize | SwfEditText::kNoSelect |
			SwfEditText::kBorder | SwfEditText::kWasStatic | SwfEditText::kPassword;

	Bytes sprite50 = cat({ place_object2([] {
							   Place p;
							   p.depth = 1;
							   p.character = 20;
							   return p;
						   }()),
			frame_label("A"), show_frame(), place_object2([] {
				Place p;
				p.depth = 2;
				p.character = 21;
				p.matrix = translate(3, 4);
				return p;
			}()),
			frame_label("B"), show_frame(), remove_object2(1), frame_label("C"), show_frame(), end_tag() });
	Bytes sprite51 = cat({ place_object2([] {
							   Place p;
							   p.depth = 1;
							   p.character = 50;
							   p.matrix = translate(100, 10);
							   p.name = "inner";
							   return p;
						   }()),
			show_frame(), end_tag() });
	Bytes csm;
	tag(csm, swf_tag::kCsmTextSettings, Bytes(12, 0));
	Bytes unknown;
	tag(unknown, 900, Bytes(5, 1));
	Bytes file_attributes;
	tag(file_attributes, swf_tag::kFileAttributes, Bytes(4, 0));
	BitmapFill f10;
	f10.bitmap = 10;
	BitmapFill f11;
	f11.bitmap = 11;
	BitmapFill f12;
	f12.bitmap = 12;
	f12.type = 0x43;
	return cat({ file_attributes, exporter_info("Synth"), external_image2(1, 256, 64, "synth_i1.tga"), external_image1(2, 32, 32, "synth_ia.tga"),
			sub_image(10, 1, 0, 0, 16, 16), sub_image(11, 1, 16, 0, 48, 32), sub_image(12, 2, 0, 0, 32, 32), define_shape(1, 20, 16, 16, { f10 }),
			define_shape(2, 21, 32, 32, { f11 }, f12), define_shape(3, 22, 8, 8, {}), define_shape(4, 23, 16, 16, { f10 }), define_font3(square_font()),
			define_font3_no_glyphs(41, "$HUD_light", 5), define_font3_codes_only(42, "$FONT_TITLE*", { 0x30, 0x31, 0x32 }), align_zones(40, 2),
			font_name(40, "Synth Sans Medium", ""), csm, define_edit_text(e1), define_edit_text(e2), define_text(60), define_sprite(50, 3, sprite50),
			define_sprite(51, 1, sprite51), scaling_grid(50, 1, 15, 2, 14), do_init_action(50, 6),
			export_assets({ { 50, "mc_icons" }, { 51, "mc_wrap" }, { 20, "shape_twenty" } }), import_assets("Other.swf", { { 70, "OtherSym" } }, true),
			import_assets("../Static/gfxfontlib.swf", { { 71, "$HUD_medium" } }, false), unknown, do_action(10),
			place_object1(20, 1, translate(5, 6)), place_object2(p2), place_object2(p3, true), show_frame(), place_object2(modify),
			place_object2(replace), remove_object1(20, 1), place_object2(clip), place_object2(over), show_frame(), end_tag() });
}

Bytes rich_movie(bool p_cfx) {
	return movie(rich_tags(), 2, p_cfx);
}

Bytes atlas_movie() {
	BitmapFill f10;
	f10.bitmap = 10;
	BitmapFill f11;
	f11.bitmap = 11;
	f11.m = scale_at(40, 40, 3, 0);
	BitmapFill f12;
	f12.bitmap = 12;
	auto place = [](uint16_t p_depth, uint16_t p_char, Mat p_m) {
		Place p;
		p.depth = p_depth;
		p.character = p_char;
		p.matrix = p_m;
		return place_object2(p);
	};
	const Bytes child = cat({ place(1, 20, translate(5, 5)), show_frame(), end_tag() });
	const Bytes states = cat({ place(1, 20, Mat{}), frame_label("One"), show_frame(), remove_object2(1), place(2, 22, Mat{}), frame_label("Two"),
			show_frame(), remove_object2(2), frame_label("Empty"), show_frame(), place(1, 30, scale_at(2, 2, 100, 0)), frame_label("Nested"), show_frame(),
			end_tag() });
	return movie(cat({ exporter_info("Atl"), external_image2(1, 256, 64, "atl_i1.tga"), sub_image(10, 1, 0, 0, 16, 16), sub_image(11, 1, 16, 0, 48, 32),
						 sub_image(12, 1, 48, 0, 64, 64), sub_image(13, 1, 250, 0, 260, 8), define_shape(3, 20, 16, 16, { f10 }),
						 define_shape(3, 21, 32, 32, { f11 }), define_shape(3, 22, 64, 64, { f10, f12 }), define_sprite(30, 1, child),
						 define_sprite(31, 4, states), export_assets({ { 31, "mc_states" }, { 30, "mc_child" } }), show_frame(), end_tag() }));
}

Bytes dag_movie(int p_levels) {
	BitmapFill f10;
	f10.bitmap = 10;
	Bytes tags = cat({ external_image2(1, 64, 64, "dag_i1.tga"), sub_image(10, 1, 0, 0, 16, 16), define_shape(3, 20, 16, 16, { f10 }) });
	for (int k = 0; k < p_levels; ++k) {
		Bytes inner;
		const uint16_t child = k == 0 ? 20 : uint16_t(100 + k - 1);
		const int copies = k == 0 ? 1 : 2;
		for (int c = 0; c < copies; ++c) {
			Place p;
			p.depth = uint16_t(c + 1);
			p.character = child;
			p.matrix = translate(float(c), 0);
			inner = cat({ inner, place_object2(p) });
		}
		inner = cat({ inner, show_frame(), end_tag() });
		tags = cat({ tags, define_sprite(uint16_t(100 + k), 1, inner) });
	}
	tags = cat({ tags, export_assets({ { uint16_t(100 + p_levels - 1), "top" } }), show_frame(), end_tag() });
	return movie(tags);
}

const SwfDisplayEntry *at_depth(const std::vector<SwfDisplayEntry> &p_list, uint16_t p_depth) {
	for (const SwfDisplayEntry &e : p_list) {
		if (e.depth == p_depth) {
			return &e;
		}
	}
	return nullptr;
}

const SwfLayoutNode *child_named(const SwfLayoutNode &p_node, std::string_view p_instance) {
	for (const SwfLayoutNode &c : p_node.children) {
		if (c.instance == p_instance) {
			return &c;
		}
	}
	return nullptr;
}

SwfLibrary layout_library() {
	auto place = [](uint16_t p_depth, uint16_t p_char, Mat p_m, std::string p_name) {
		Place p;
		p.depth = p_depth;
		p.character = p_char;
		p.matrix = p_m;
		p.name = std::move(p_name);
		return place_object2(p);
	};
	const Bytes screen_sprite = cat({ place(1, 50, translate(1194, 669), "Widget_01"), place(2, 20, translate(10, 10), "bg"),
			place(3, 51, translate(20, 700), "Missing_01"), show_frame(), end_tag() });
	const Bytes screen = movie(cat({ import_assets("Widget.swf", { { 50, "Widget" } }), import_assets("Nowhere.swf", { { 51, "Nothing" } }),
			define_shape(3, 20, 4, 4, {}), define_sprite(10, 1, screen_sprite), export_assets({ { 10, "ScreenSym" } }),
			place(1, 10, Mat{}, "instance1"), show_frame(), end_tag() }));
	EditText t;
	t.id = 7;
	t.font_id = 8;
	t.height = 21;
	t.align = 1;
	t.text = "000";
	const Bytes grid_sprite = cat({ show_frame(), end_tag() });
	const Bytes widget_sprite = cat({ place(1, 7, translate(-10, 2), "txt"), place(2, 6, scale_at(1.5f, 2, 0, 0), "bgw"), show_frame(), end_tag() });
	const Bytes widget = movie(cat({ import_assets("../Static/gfxfontlib.swf", { { 8, "$HUD_medium" } }), define_edit_text(t),
			define_sprite(6, 1, grid_sprite), scaling_grid(6, 2, 10, 3, 9), define_sprite(5, 1, widget_sprite), export_assets({ { 5, "Widget" } }),
			show_frame(), end_tag() }));
	std::vector<std::pair<std::string, std::vector<uint8_t>>> payloads;
	payloads.emplace_back("ui/assets/screen", screen);
	payloads.emplace_back("UI/Assets/Widget", widget);
	return SwfLibrary::from_memory(std::move(payloads));
}

}

TEST_SUITE("ui-swf-unit") {
	TEST_CASE("swf: every tag the reader keeps (GFX and CFX)") {
		for (const bool cfx : { false, true }) {
			CAPTURE(cfx);
			const SwfMovie m = parse_ok(rich_movie(cfx));
			CHECK(m.compressed == cfx);
			CHECK(m.version == 8);
			CHECK((m.stage == SwfRect{ 0, 1280, 0, 720 }));
			CHECK(m.frame_rate == 30.0f);
			CHECK(m.frame_count == 2);
			CHECK(m.root.frames == 2);
			CHECK(m.trailing_bytes == 0);
			CHECK(m.unknown_tags == 1);
			CHECK(m.csm_text_settings == 1);

			CHECK(m.exporter.present);
			CHECK(m.exporter.version == 0x36f);
			CHECK(m.exporter.bitmap_format == 13);
			CHECK(m.exporter.swf_name == "Synth");
			CHECK(m.exporter.extra_bytes == 2);
			REQUIRE(m.external_images.size() == 2);
			CHECK(m.external_images[0].tag_version == 2);
			CHECK(m.external_images[0].field_2 == 9);
			CHECK(m.external_images[0].width == 256);
			CHECK(m.external_images[0].height == 64);
			CHECK(m.external_images[0].file_name == "synth_i1.tga");
			CHECK(m.external_images[0].extra_bytes == 1);
			CHECK(m.external_images[1].tag_version == 1);
			CHECK(m.external_images[1].image_id == 2);
			CHECK(m.external_images[1].file_name == "synth_ia.tga");
			CHECK(m.find_external_image(2) == &m.external_images[1]);
			REQUIRE(m.sub_images.size() == 3);
			CHECK(m.find_sub_image(11)->x1 == 16);
			CHECK(m.find_sub_image(11)->width() == 32);
			CHECK(m.find_sub_image(11)->height() == 32);
			CHECK(m.find_sub_image(12)->image_id == 2);

			REQUIRE(m.shapes.size() == 4);
			const SwfShape *s20 = m.find_shape(20);
			REQUIRE(s20 != nullptr);
			CHECK(s20->version == 1);
			CHECK((s20->bounds == SwfRect{ 0, 16, 0, 16 }));
			REQUIRE(s20->bitmap_fills.size() == 1);
			CHECK(s20->bitmap_fills[0].bitmap_id == 10);
			CHECK(s20->bitmap_fills[0].fill_type == 0x41);
			CHECK(near(s20->bitmap_fills[0].matrix.a, 20.0f));
			CHECK(s20->solid_fills == 1);
			CHECK(s20->line_styles == 1);
			CHECK(s20->edges == 4);
			const SwfShape *s21 = m.find_shape(21);
			REQUIRE(s21 != nullptr);
			CHECK(s21->version == 2);
			REQUIRE(s21->bitmap_fills.size() == 2);
			CHECK(s21->bitmap_fills[1].bitmap_id == 12);
			CHECK(s21->bitmap_fills[1].fill_type == 0x43);
			CHECK(s21->edges == 4);
			CHECK(m.find_shape(22)->version == 3);
			CHECK(m.find_shape(22)->bitmap_fills.empty());
			CHECK(m.find_shape(23)->version == 4);
			CHECK(m.find_shape(23)->bitmap_fills.size() == 1);

			REQUIRE(m.edit_texts.size() == 2);
			const SwfEditText *t30 = m.find_edit_text(30);
			REQUIRE(t30 != nullptr);
			CHECK((t30->bounds == SwfRect{ -1, 101, -2, 22 }));
			CHECK(t30->has(SwfEditText::kHasFont));
			CHECK(t30->has(SwfEditText::kUseOutlines));
			CHECK(t30->has(SwfEditText::kHtml));
			CHECK(t30->font_id == 40);
			CHECK(t30->height == 21.0f);
			CHECK((t30->color == SwfColor{ 0xff, 0x80, 0x10, 0xc0 }));
			CHECK(t30->max_length == 12);
			CHECK(t30->align == 1);
			CHECK(t30->left_margin == 2.0f);
			CHECK(t30->right_margin == 3.0f);
			CHECK(t30->indent == 1.0f);
			CHECK(t30->leading == -1.0f);
			CHECK(t30->variable == "v");
			CHECK(t30->initial_text == "000");
			const SwfEditText *t31 = m.find_edit_text(31);
			REQUIRE(t31 != nullptr);
			CHECK(t31->font_class == "$HUD_medium");
			CHECK(t31->height == 20.0f);
			CHECK_FALSE(t31->has(SwfEditText::kHasText));
			for (const uint16_t f : { SwfEditText::kWordWrap, SwfEditText::kMultiline, SwfEditText::kPassword, SwfEditText::kReadOnly,
						 SwfEditText::kAutoSize, SwfEditText::kNoSelect, SwfEditText::kBorder, SwfEditText::kWasStatic, SwfEditText::kHasFontClass }) {
				CHECK(t31->has(f));
			}
			CHECK(m.static_texts == std::vector<uint16_t>{ 60 });

			REQUIRE(m.fonts.size() == 3);
			CHECK(m.find_font(40)->kind == SwfFontKind::Outlines);
			CHECK(m.find_font(40)->name == "Synth Sans Medium");
			CHECK(m.find_font(40)->glyph_count == 2);
			CHECK(m.find_font(40)->flags == 0x8C);
			CHECK(m.find_font(41)->kind == SwfFontKind::NoGlyphs);
			CHECK(m.find_font(41)->trailing_bytes == 5);
			CHECK(m.find_font(42)->kind == SwfFontKind::CodesOnly);
			CHECK(m.find_font(42)->glyph_count == 3);
			CHECK(m.find_font(42)->name == "$FONT_TITLE*");
			REQUIRE(m.font_align_zones.size() == 1);
			CHECK(m.font_align_zones[0].glyphs == 2);
			CHECK(m.font_align_zones[0].csm_hint == 1);
			REQUIRE(m.font_names.size() == 1);
			CHECK(m.font_names[0].name == "Synth Sans Medium");
			REQUIRE(m.scaling_grids.size() == 1);
			CHECK((m.scaling_grids[0].splitter == SwfRect{ 1, 15, 2, 14 }));

			REQUIRE(m.sprites.size() == 2);
			const SwfSprite *s50 = m.find_sprite(50);
			REQUIRE(s50 != nullptr);
			CHECK(s50->timeline.declared_frames == 3);
			CHECK(s50->timeline.frames == 3);
			REQUIRE(s50->timeline.labels.size() == 3);
			CHECK(s50->timeline.labels[1].name == "B");
			CHECK(s50->timeline.labels[1].frame == 1);
			CHECK(s50->timeline.events.size() == 3);
			REQUIRE(m.exports.size() == 3);
			CHECK(m.export_id("mc_wrap") == std::optional<uint16_t>(51));
			CHECK(m.export_name(20) == "shape_twenty");
			CHECK_FALSE(m.export_id("nope").has_value());
			REQUIRE(m.imports.size() == 2);
			CHECK(m.imports[0].tag_version == 2);
			CHECK(m.imports[1].tag_version == 1);
			const SwfExport *sym = nullptr;
			const SwfImport *im = m.find_import(71, &sym);
			REQUIRE(im != nullptr);
			CHECK(im->url == "../Static/gfxfontlib.swf");
			CHECK(sym->name == "$HUD_medium");
			CHECK(m.character_kind(20) == SwfCharacterKind::Shape);
			CHECK(m.character_kind(50) == SwfCharacterKind::Sprite);
			CHECK(m.character_kind(30) == SwfCharacterKind::EditText);
			CHECK(m.character_kind(60) == SwfCharacterKind::StaticText);
			CHECK(m.character_kind(40) == SwfCharacterKind::Font);
			CHECK(m.character_kind(10) == SwfCharacterKind::SubImage);
			CHECK(m.character_kind(70) == SwfCharacterKind::Imported);
			CHECK(m.character_kind(999) == SwfCharacterKind::None);
			CHECK(m.action_bytes == 10 + 6);
			CHECK(m.do_init_actions == 1);
			CHECK(m.root.do_actions == 1);
			CHECK(m.tag_count(swf_tag::kPlaceObject2) == 8);
			CHECK(m.tag_count(swf_tag::kPlaceObject3) == 1);
			CHECK(m.tag_count(swf_tag::kPlaceObject) == 1);
			CHECK(m.tag_count(swf_tag::kRemoveObject) == 1);
			CHECK(m.tag_count(swf_tag::kRemoveObject2) == 1);
			CHECK(m.tag_count(swf_tag::kShowFrame) == 2 + 3 + 1);
			CHECK(m.tag_count(swf_tag::kEnd) == 3);
			CHECK(m.tag_count(swf_tag::kDefineSprite) == 2);
			CHECK(m.tag_count(swf_tag::kFrameLabel) == 3);
			CHECK(m.tag_count(900) == 1);
			CHECK(m.budget_used > 0);
			CHECK(m.budget_used < swf_default_budget_units(m.file_length));
			CHECK(m.retained_bytes >= m.body.size());

			const std::vector<SwfDisplayEvent> &ev = m.root.events;
			REQUIRE(ev.size() == 8);
			CHECK(ev[0].tag_version == 1);
			CHECK(ev[0].op == SwfDisplayOp::Place);
			CHECK(ev[0].character == 20);
			CHECK(ev[0].matrix.tx == 5.0f);
			CHECK(ev[0].matrix.ty == 6.0f);
			CHECK_FALSE(ev[0].has_color);
			CHECK(ev[1].op == SwfDisplayOp::Place);
			CHECK(ev[1].has_matrix);
			CHECK(ev[1].matrix.a == 2.0f);
			CHECK(ev[1].matrix.tx == 100.0f);
			CHECK(ev[1].has_color);
			CHECK((ev[1].color.mul == std::array<int16_t, 4>{ 128, 256, 256, 200 }));
			CHECK((ev[1].color.add == std::array<int16_t, 4>{ 10, -5, 0, 0 }));
			CHECK(ev[1].has_ratio);
			CHECK(ev[1].ratio == 7);
			CHECK(ev[1].name == "wrap");
			CHECK(ev[2].tag_version == 3);
			CHECK(ev[2].class_name == "Cls");
			CHECK(ev[2].has_filters);
			CHECK(ev[2].blend_mode == 3);
			CHECK(ev[2].name == "p3");
			CHECK(ev[3].op == SwfDisplayOp::Modify);
			CHECK(ev[3].frame == 1);
			CHECK(ev[4].op == SwfDisplayOp::Replace);
			CHECK(ev[4].character == 22);
			CHECK(ev[5].op == SwfDisplayOp::Remove);
			CHECK(ev[5].tag_version == 1);
			CHECK(ev[6].clip_depth == 9);
			CHECK(ev[7].op == SwfDisplayOp::Place);
		}
		CHECK(std::string(swf_tag_name(swf_tag::kGfxDefineSubImage)) == "GFX_DefineSubImage");
		CHECK(swf_tag_name(900) == nullptr);
		CHECK(std::string(swf_font_kind_name(SwfFontKind::CodesOnly)) == "codes_only");
		CHECK(std::string(swf_character_kind_name(SwfCharacterKind::Imported)) == "imported");
	}

	TEST_CASE("swf: display lists (place, modify, replace, remove; one pass, any order)") {
		const SwfMovie m = parse_ok(rich_movie(false));
		const uint32_t frames[] = { 2, 0, 1, 0 };
		std::vector<std::vector<SwfDisplayEntry>> lists;
		WorkBudget budget(1u << 20);
		REQUIRE(display_lists_at(m.root, frames, budget, lists).ok());
		REQUIRE(lists.size() == 4);

		const std::vector<SwfDisplayEntry> &f0 = lists[1];
		REQUIRE(f0.size() == 3);
		CHECK(f0[0].depth == 1);
		CHECK(f0[0].character == 20);
		CHECK(f0[1].character == 51);
		CHECK(f0[1].name == "wrap");
		CHECK(f0[1].matrix.a == 2.0f);
		CHECK(f0[2].character == 21);
		CHECK(lists[3].size() == 3);

		for (const size_t i : { size_t(0), size_t(2) }) {
			const std::vector<SwfDisplayEntry> &f = lists[i];
			REQUIRE(f.size() == 3);
			CHECK(at_depth(f, 1) == nullptr);
			const SwfDisplayEntry *d2 = at_depth(f, 2);
			REQUIRE(d2 != nullptr);
			CHECK(d2->character == 51);
			CHECK((d2->matrix == SwfMatrix{ 1, 0, 0, 1, 200, 0 }));
			CHECK(d2->name == "wrap");
			CHECK(d2->color.mul[0] == 128);
			const SwfDisplayEntry *d3 = at_depth(f, 3);
			REQUIRE(d3 != nullptr);
			CHECK(d3->character == 22);
			CHECK(d3->name == "p3");
			const SwfDisplayEntry *d4 = at_depth(f, 4);
			REQUIRE(d4 != nullptr);
			CHECK(d4->character == 20);
			CHECK(d4->clip_depth == 0);
		}

		CHECK(budget.spent() == 5 + 8 + 4 * 12);

		const uint32_t past[] = { 3 };
		CHECK_FALSE(display_lists_at(m.root, past, budget, lists).ok());

		WorkBudget tiny(10);
		CHECK_FALSE(display_lists_at(m.root, frames, tiny, lists).ok());
	}

	TEST_CASE("swf: the atlas rule (nested sprites, composed matrices, empty frames, several pieces)") {
		const SwfMovie m = parse_ok(atlas_movie());
		WorkBudget budget(kSwfAtlasBudgetUnits);
		Result<SwfAtlasMap> map = build_atlas_map(m, "ui/assets/atl", budget);
		REQUIRE_MESSAGE(map.ok(), (map.ok() ? std::string() : map.error().message));
		REQUIRE(map->images.size() == 1);
		CHECK(map->images[0].texture_res == "ui/assets/atl/atl_i1");
		CHECK(map->images[0].width == 256);
		REQUIRE(map->labels.size() == 4);
		const SwfAtlasRegion *one = map->find("mc_states/One");
		REQUIRE(one != nullptr);
		REQUIRE(one->pieces.size() == 1);
		CHECK(one->pieces[0].sub_image == 10);
		CHECK(one->pieces[0].x2 == 16);
		CHECK((one->pieces[0].placement == SwfMatrix{ 1, 0, 0, 1, 0, 0 }));
		const SwfAtlasRegion *two = map->find("Two");
		REQUIRE(two != nullptr);
		CHECK(two->name == "mc_states/Two");
		REQUIRE(two->pieces.size() == 2);
		CHECK(two->pieces[0].sub_image == 10);
		CHECK(two->pieces[1].sub_image == 12);
		CHECK(map->find("mc_states/Empty")->pieces.empty());
		const SwfAtlasRegion *nested = map->find("mc_states/Nested");
		REQUIRE(nested != nullptr);
		REQUIRE(nested->pieces.size() == 1);

		CHECK((nested->pieces[0].placement == SwfMatrix{ 2, 0, 0, 2, 110, 10 }));
		CHECK(nested->frame == 3);

		REQUIRE(map->symbols.size() == 2);
		CHECK(map->find("mc_child")->pieces.size() == 1);
		CHECK(map->find("mc_child")->pieces[0].placement.tx == 5.0f);
		CHECK(map->find("mc_states@1") != nullptr);
		CHECK(map->find("mc_states@2") == nullptr);
		CHECK(map->find("mc_child@0") != nullptr);
		CHECK(map->frames.size() == 4);
		CHECK(map->bare.size() == 4);
		CHECK(map->find("#11") != nullptr);
		const SwfAtlasStats &s = map->stats;
		CHECK(s.labels == 4);
		CHECK(s.labels_with_art == 3);
		CHECK(s.labels_one_piece == 2);
		CHECK(s.labels_several_pieces == 1);
		CHECK(s.labels_empty == 1);
		CHECK(s.sub_images == 4);
		CHECK(s.sub_images_from_labels == 2);
		CHECK(s.sub_images_from_symbols == 2);
		CHECK(s.sub_images_from_frames == 2);
		CHECK(s.rects_outside_atlas == 1);
		CHECK(s.pieces_without_image == 0);

		CHECK(map->find("nothing") == nullptr);
	}

	TEST_CASE("swf: library helpers and the in-memory library") {
		CHECK(swf_movie_name_for_url("Ammo.swf") == "ui/assets/ammo");
		CHECK(swf_movie_name_for_url("HudTextures.swf") == "ui/assets/hudtextures");
		CHECK(swf_movie_name_for_url("../Static/gfxfontlib.swf").empty());
		CHECK(is_font_library_url("../Static/gfxfontlib.swf"));
		CHECK(is_font_library_url("..\\static\\GFXFONTLIB.SWF"));
		CHECK_FALSE(is_font_library_url("Ammo.swf"));
		CHECK(swf_atlas_texture_name("ui/assets/iconsingame", "iconsingame_i1.tga") == "ui/assets/iconsingame/iconsingame_i1");
		CHECK(swf_atlas_texture_name("ui/assets/killcam", "killcam_ie.tga") == "ui/assets/killcam/killcam_ie");
		const SwfLibrary lib = layout_library();
		REQUIRE(lib.movies().size() == 2);
		CHECK(lib.movies()[0].name == "ui/assets/screen");
		CHECK(lib.find("UI\\Assets\\WIDGET") != nullptr);
		CHECK(lib.find("ui/assets/none") == nullptr);
		Result<std::shared_ptr<const SwfMovie>> a = lib.load("ui/assets/widget");
		REQUIRE(a.ok());
		Result<std::shared_ptr<const SwfMovie>> b = lib.load("ui/assets/widget");
		REQUIRE(b.ok());
		CHECK(a->get() == b->get());
		CHECK_FALSE(lib.load("ui/assets/none").ok());
		SwfExternalImage im;
		im.file_name = "x_i1.tga";
		CHECK_FALSE(lib.load_atlas_texture("ui/assets/widget", im).ok());
		CHECK_FALSE(lib.read_atlas_texture_header("ui/assets/widget", im).ok());

		std::vector<std::pair<std::string, std::vector<uint8_t>>> bad;
		bad.emplace_back("ui/assets/bad", Bytes{ 'X', 'Y', 'Z' });
		const SwfLibrary broken = SwfLibrary::from_memory(std::move(bad));
		CHECK_FALSE(broken.load("ui/assets/bad").ok());
		CHECK_FALSE(broken.load("ui/assets/bad").ok());
	}

	TEST_CASE("swf: layout trees and the HUD table over an in-memory library") {
		const SwfLibrary lib = layout_library();
		WorkBudget budget(kSwfLayoutBudgetUnits);
		Result<SwfLayoutNode> root = read_layout(lib, "screen", {}, budget);
		REQUIRE_MESSAGE(root.ok(), (root.ok() ? std::string() : root.error().message));
		CHECK(root->movie == "ui/assets/screen");
		REQUIRE(root->children.size() == 1);
		const SwfLayoutNode &screen = root->children[0];
		CHECK(screen.instance == "instance1");
		CHECK(screen.kind == SwfLayoutKind::Sprite);
		CHECK(screen.symbol == "ScreenSym");
		const SwfLayoutNode *w = child_named(screen, "Widget_01");
		REQUIRE(w != nullptr);
		CHECK(w->kind == SwfLayoutKind::Sprite);
		CHECK(w->movie == "ui/assets/widget");
		CHECK(w->symbol == "Widget");
		CHECK(w->stage.tx == 1194.0f);
		const SwfLayoutNode *txt = child_named(*w, "txt");
		REQUIRE(txt != nullptr);
		CHECK(txt->kind == SwfLayoutKind::EditText);
		CHECK(txt->text.font == "$HUD_medium");
		CHECK(txt->text.size == 21.0f);
		CHECK(txt->text.align == 1);
		CHECK(txt->text.initial_text == "000");
		CHECK(txt->text.html);
		CHECK(txt->stage.tx == 1184.0f);
		CHECK(txt->stage.ty == 671.0f);
		CHECK(txt->local.tx == -10.0f);
		const SwfLayoutNode *bgw = child_named(*w, "bgw");
		REQUIRE(bgw != nullptr);
		CHECK(bgw->has_scaling_grid);
		CHECK((bgw->scaling_grid == SwfRect{ 2, 10, 3, 9 }));
		CHECK(bgw->stage.a == 1.5f);
		CHECK(child_named(screen, "bg")->kind == SwfLayoutKind::Shape);
		const SwfLayoutNode *missing = child_named(screen, "Missing_01");
		REQUIRE(missing != nullptr);
		CHECK(missing->kind == SwfLayoutKind::Import);
		CHECK(missing->symbol == "Nothing");

		SwfLayoutOptions no_follow;
		no_follow.follow_imports = false;
		Result<SwfLayoutNode> flat = read_layout(lib, "ui/assets/screen", no_follow, budget);
		REQUIRE(flat.ok());
		CHECK(child_named(flat->children[0], "Widget_01")->kind == SwfLayoutKind::Import);
		SwfLayoutOptions shallow;
		shallow.max_depth = 1;
		Result<SwfLayoutNode> top = read_layout(lib, "screen", shallow, budget);
		REQUIRE(top.ok());
		REQUIRE(top->children.size() == 1);
		CHECK(top->children[0].children.empty());
		CHECK_FALSE(read_layout(lib, "nothing", {}, budget).ok());

		WorkBudget hud_budget(kSwfLayoutBudgetUnits);
		Result<std::vector<HudPlacement>> rows = read_hud_placements(lib, "screen", hud_budget);
		REQUIRE(rows.ok());
		REQUIRE(rows->size() == 3);
		const HudPlacement &p = (*rows)[0];
		CHECK(p.instance == "Widget_01");
		CHECK(p.widget == "Widget");
		CHECK(p.widget_movie == "ui/assets/widget");
		CHECK(p.screen_symbol == "ScreenSym");
		CHECK(p.matrix.tx == 1194.0f);
		CHECK(p.anchor == HudAnchor::BottomRight);
		CHECK(p.anchor_dx == -86.0f);
		CHECK(p.anchor_dy == -51.0f);
		CHECK((*rows)[1].widget == "#20");
		CHECK((*rows)[1].anchor == HudAnchor::TopLeft);
		CHECK((*rows)[2].anchor == HudAnchor::BottomLeft);
		const SwfRect stage{ 0, 1280, 0, 720 };
		CHECK(hud_anchor_for(640, 360, stage) == HudAnchor::Centre);
		CHECK(hud_anchor_for(1274, 3, stage) == HudAnchor::TopRight);
		CHECK(hud_anchor_for(640, 700, stage) == HudAnchor::Bottom);
		CHECK(hud_anchor_for(0, 360, stage) == HudAnchor::Left);
		CHECK(std::string(hud_anchor_name(HudAnchor::BottomRight)) == "bottom_right");
		CHECK(hud_screen_movies().size() == 14);
	}

	TEST_CASE("swf: every truncation of each synthetic movie fails cleanly") {
		hostile("truncations", [] {
			for (const Bytes &full : { rich_movie(false), rich_movie(true), atlas_movie() }) {

				for (size_t n = 0; n < full.size(); ++n) {
					const Bytes cut(full.begin(), full.begin() + ptrdiff_t(n));
					CHECK_FALSE(SwfMovie::parse(cut).ok());
				}
			}

			const Bytes rich = rich_movie(false);
			const Bytes body(rich.begin() + 8, rich.end());
			size_t failed = 0;
			for (size_t n = 0; n < body.size(); ++n) {
				Result<SwfMovie> m = SwfMovie::parse(gfx_from_body(Bytes(body.begin(), body.begin() + ptrdiff_t(n))));
				if (!m) {
					++failed;
					continue;
				}
				WorkBudget b(kSwfAtlasBudgetUnits);
				(void)build_atlas_map(*m, "ui/assets/cut", b);
			}
			CHECK(failed > body.size() / 2);
		});
	}

	TEST_CASE("swf: 2,000 seeded mutants of each synthetic movie") {
		hostile("mutants", [] {
			std::mt19937_64 rng(0x5f3759df);
			size_t ok = 0;
			size_t failed = 0;
			const Bytes rich = rich_movie(false);
			const Bytes atl = atlas_movie();
			const Bytes cfx = rich_movie(true);
			for (const Bytes *base : { &rich, &atl, &cfx }) {
				for (size_t i = 0; i < fuzz_iters(2000); ++i) {
					Bytes b;
					if (base == &cfx || i % 4 == 0) {
						b = *base;
						mutate(b, rng);
					} else {
						Bytes body(base->begin() + 8, base->end());
						mutate(body, rng);
						b = gfx_from_body(body);
					}
					Result<SwfMovie> m = SwfMovie::parse(b);
					if (!m) {
						++failed;
						CHECK(!m.error().message.empty());
						continue;
					}
					++ok;
					WorkBudget ab(kSwfAtlasBudgetUnits);
					(void)build_atlas_map(*m, "ui/assets/mutant", ab);
					for (const SwfFontInfo &f : m->fonts) {
						WorkBudget fb(kUiFontConvertBudgetUnits);
						Result<SwfFontOutlines> o = load_font_outlines(*m, f, fb);
						if (o && o->kind == SwfFontKind::Outlines) {
							(void)write_truetype(*o, {}, fb);
						}
					}
					std::vector<std::vector<SwfDisplayEntry>> lists;
					const uint32_t last = m->root.frames;
					WorkBudget db(1u << 20);
					CHECK(display_lists_at(m->root, std::span<const uint32_t>(&last, 1), db, lists).ok());
				}
			}
			MESSAGE("mutants: " << ok << " parsed, " << failed << " refused");
			CHECK(failed > 0);
			CHECK(ok > 0);
		});
	}

	TEST_CASE("swf: hostile headers (zlib bomb, 2^32 and 16 MiB + 1 declared lengths, GFX length, signature)") {
		hostile("zlib bomb", [] {

			const std::string e = error_of(cfx_with_stream(1000, zlib_bomb(8u << 20)));
			CHECK(e.find("inflates past") != std::string::npos);

			const std::string e2 = error_of(cfx_with_stream(8 + kSwfMaxBodyBytes, zlib_bomb(kSwfMaxBodyBytes + (1u << 20))));
			CHECK(e2.find("inflates past") != std::string::npos);
		});
		hostile("declared lengths", [] {
			CHECK(error_of(cfx_with_stream(0xFFFFFFFFu, zlib_stored(Bytes(16, 0)))).find("cap") != std::string::npos);
			CHECK(error_of(cfx_with_stream(8 + kSwfMaxBodyBytes + 1, zlib_stored(Bytes(16, 0)))).find("cap") != std::string::npos);
			Bytes g = gfx_from_body(Bytes(16, 0));
			g[4] = uint8_t(g[4] + 1);
			CHECK(error_of(g).find("GFX body") != std::string::npos);
			CHECK(error_of(cfx_with_stream(4, {})).find("shorter than the header") != std::string::npos);
			CHECK(error_of(Bytes{ 'F', 'W', 'S', 8, 20, 0, 0, 0 }).find("signature") != std::string::npos);
			CHECK(error_of(Bytes{ 'G', 'F', 'X' }).find("8-byte header") != std::string::npos);

			CHECK(error_of(cfx_with_stream(8 + 100, zlib_stored(Bytes(50, 0)))).find("inflates to 50") != std::string::npos);
			CHECK_FALSE(SwfMovie::parse(cfx_with_stream(8 + 100, Bytes(20, 0x55))).ok());
		});
	}

	TEST_CASE("swf: hostile structure (tag past the end, nested sprite, tag count, frames, events, labels, rectangles)") {
		hostile("tag past the end", [] {
			Bytes t;
			tag(t, swf_tag::kDefineShape3, Bytes(1000, 0), true);
			t.resize(40);
			CHECK(error_of(movie(t)).find("past the end") != std::string::npos);
			CHECK(error_of(movie(Bytes{ 0x3f })).find("tag header past the end") != std::string::npos);
		});
		hostile("nested DefineSprite", [] {
			const Bytes inner = define_sprite(2, 1, cat({ show_frame(), end_tag() }));
			CHECK(error_of(movie(cat({ define_sprite(1, 1, cat({ inner, show_frame(), end_tag() })), end_tag() }))).find("inside a sprite") !=
					std::string::npos);
		});
		hostile("2^22 + 1 tags", [] {
			Bytes many;
			many.reserve(2 * (kSwfMaxTags + 2));
			for (uint32_t i = 0; i < kSwfMaxTags; ++i) {
				many.push_back(0x00);
				many.push_back(uint8_t((900 << 6) >> 8));
			}
			Bytes at_cap = many;
			at_cap.resize(at_cap.size() - 2);
			const Bytes end = end_tag();
			at_cap.insert(at_cap.end(), end.begin(), end.end());
			Result<SwfMovie> ok = SwfMovie::parse(movie(at_cap));
			CHECK_MESSAGE(ok.ok(), (ok.ok() ? std::string() : ok.error().message));
			many.insert(many.end(), end.begin(), end.end());
			CHECK(error_of(movie(many)).find("more than 4194304 tags") != std::string::npos);
		});
		hostile("65,536 / 65,537 frames", [] {
			Bytes frames;
			for (uint32_t i = 0; i < kSwfMaxFrames; ++i) {
				const Bytes f = show_frame();
				frames.insert(frames.end(), f.begin(), f.end());
			}
			const Bytes end = end_tag();
			Result<SwfMovie> ok = SwfMovie::parse(movie(cat({ frames, end }), 65535));
			REQUIRE(ok.ok());
			CHECK(ok->root.frames == kSwfMaxFrames);
			CHECK(error_of(movie(cat({ frames, show_frame(), end }), 65535)).find("frames") != std::string::npos);
		});
		hostile("2^18 + 1 display events", [] {
			Bytes events;
			events.reserve(4 * (kSwfMaxDisplayEvents + 1));
			for (uint32_t i = 0; i < kSwfMaxDisplayEvents; ++i) {
				const Bytes r = remove_object2(uint16_t(i));
				events.insert(events.end(), r.begin(), r.end());
			}
			Result<SwfMovie> ok = SwfMovie::parse(movie(cat({ events, show_frame(), end_tag() })));
			REQUIRE_MESSAGE(ok.ok(), (ok.ok() ? std::string() : ok.error().message));
			CHECK(ok->root.events.size() == kSwfMaxDisplayEvents);
			CHECK(error_of(movie(cat({ events, remove_object2(1), show_frame(), end_tag() }))).find("display events") != std::string::npos);
		});
		hostile("label after the last ShowFrame", [] {
			BitmapFill f10;
			f10.bitmap = 10;
			Place p;
			p.depth = 1;
			p.character = 20;
			const Bytes sprite = cat({ show_frame(), frame_label("tail"), place_object2(p), end_tag() });
			const SwfMovie m = parse_ok(movie(cat({ external_image2(1, 64, 64, "t_i1.tga"), sub_image(10, 1, 0, 0, 8, 8), define_shape(3, 20, 8, 8, { f10 }),
					define_sprite(30, 1, sprite), export_assets({ { 30, "mc_tail" } }), show_frame(), end_tag() })));
			const SwfSprite *s = m.find_sprite(30);
			REQUIRE(s != nullptr);
			REQUIRE(s->timeline.labels.size() == 1);
			CHECK(s->timeline.labels[0].frame == s->timeline.frames);
			WorkBudget b(kSwfAtlasBudgetUnits);
			Result<SwfAtlasMap> map = build_atlas_map(m, "ui/assets/t", b);
			REQUIRE(map.ok());
			const SwfAtlasRegion *tail = map->find("mc_tail/tail");
			REQUIRE(tail != nullptr);
			CHECK(tail->pieces.size() == 1);
		});
		hostile("reversed sub-image rectangle", [] {
			CHECK(error_of(movie(cat({ sub_image(10, 1, 16, 0, 8, 8), end_tag() }))).find("reversed rectangle") != std::string::npos);
			CHECK(error_of(movie(cat({ sub_image(10, 1, 0, 9, 8, 8), end_tag() }))).find("reversed rectangle") != std::string::npos);
			CHECK(SwfMovie::parse(movie(cat({ sub_image(10, 1, 8, 8, 8, 8), end_tag() }))).ok());
		});
		hostile("short records", [] {
			Bytes t;
			tag(t, swf_tag::kDoInitAction, Bytes(1, 0));
			CHECK(error_of(movie(cat({ t, end_tag() }))).find("shorter than its sprite id") != std::string::npos);
			Bytes s;
			tag(s, swf_tag::kDefineSprite, Bytes(3, 0));
			CHECK(error_of(movie(cat({ s, end_tag() }))).find("shorter than its header") != std::string::npos);
			Bytes x;
			tag(x, swf_tag::kGfxDefineSubImage, Bytes(11, 0));
			CHECK(error_of(movie(cat({ x, end_tag() }))).find("truncated") != std::string::npos);
			Bytes shape = define_shape(3, 20, 8, 8, {});
			shape.push_back(0);
			Bytes body(shape.begin() + 2, shape.end());
			Bytes padded;
			tag(padded, swf_tag::kDefineShape3, body);
			CHECK(error_of(movie(cat({ padded, end_tag() }))).find("bytes before its tag") != std::string::npos);
		});
	}

	TEST_CASE("swf: hostile sprite graphs (a sprite placing itself, a 16-deep binary DAG, nesting past 16)") {
		hostile("self-placing sprite", [] {
			Place p;
			p.depth = 1;
			p.character = 30;
			const SwfMovie m = parse_ok(movie(cat({ define_sprite(30, 1, cat({ place_object2(p), show_frame(), end_tag() })),
					export_assets({ { 30, "loop" } }), show_frame(), end_tag() })));
			WorkBudget b(kSwfAtlasBudgetUnits);
			Result<SwfAtlasMap> map = build_atlas_map(m, "ui/assets/loop", b);
			REQUIRE_FALSE(map.ok());
			CHECK(map.error().message.find("cycle") != std::string::npos);

			std::vector<std::pair<std::string, std::vector<uint8_t>>> payloads;
			Place root;
			root.depth = 1;
			root.character = 30;
			payloads.emplace_back("ui/assets/loop", movie(cat({ define_sprite(30, 1, cat({ place_object2(p), show_frame(), end_tag() })),
															  place_object2(root), show_frame(), end_tag() })));
			const SwfLibrary lib = SwfLibrary::from_memory(std::move(payloads));
			WorkBudget lb(kSwfLayoutBudgetUnits);
			Result<SwfLayoutNode> tree = read_layout(lib, "loop", {}, lb);
			REQUIRE(tree.ok());
			size_t depth = 0;
			for (const SwfLayoutNode *n = &*tree; !n->children.empty(); n = &n->children[0]) {
				++depth;
			}
			CHECK(depth <= kSwfLayoutMaxDepth);
		});
		hostile("13-level DAG: 4,096 pieces, linear", [] {
			const SwfMovie m = parse_ok(dag_movie(13));
			WorkBudget b(kSwfAtlasBudgetUnits);
			Result<SwfAtlasMap> map = build_atlas_map(m, "ui/assets/dag", b);
			REQUIRE_MESSAGE(map.ok(), (map.ok() ? std::string() : map.error().message));
			CHECK(map->find("top")->pieces.size() == 4096);
			MESSAGE("13-level DAG: " << b.spent() << " units");
			CHECK(b.spent() < (size_t(1) << 20));
		});
		hostile("16-level DAG: the piece cap", [] {
			const SwfMovie m = parse_ok(dag_movie(16));
			WorkBudget b(kSwfAtlasBudgetUnits);
			Result<SwfAtlasMap> map = build_atlas_map(m, "ui/assets/dag", b);
			REQUIRE_FALSE(map.ok());
			CHECK(map.error().message.find("pieces") != std::string::npos);
		});
		hostile("a chain of 18 sprites", [] {
			BitmapFill f10;
			f10.bitmap = 10;
			Bytes tags = cat({ external_image2(1, 64, 64, "c_i1.tga"), sub_image(10, 1, 0, 0, 16, 16), define_shape(3, 20, 16, 16, { f10 }) });
			for (int k = 0; k < 18; ++k) {
				Place p;
				p.depth = 1;
				p.character = k == 0 ? 20 : uint16_t(100 + k - 1);
				tags = cat({ tags, define_sprite(uint16_t(100 + k), 1, cat({ place_object2(p), show_frame(), end_tag() })) });
			}
			const SwfMovie m = parse_ok(movie(cat({ tags, export_assets({ { 117, "deep" } }), show_frame(), end_tag() })));
			WorkBudget b(kSwfAtlasBudgetUnits);
			Result<SwfAtlasMap> map = build_atlas_map(m, "ui/assets/chain", b);
			REQUIRE_FALSE(map.ok());
			CHECK(map.error().message.find("nested deeper") != std::string::npos);
		});
		hostile("an atlas budget too small", [] {
			const SwfMovie m = parse_ok(atlas_movie());
			WorkBudget b(20);
			CHECK_FALSE(build_atlas_map(m, "ui/assets/atl", b).ok());
		});
	}
}

namespace {

struct InstallFixture {
	InstallReader reader;
	std::shared_ptr<Install> install;
	InstallSnapshot before;

	bool open() {
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return false;
		}
		Result<std::shared_ptr<Install>> opened = Install::open(reader);
		REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
		install = *opened;
		before = reader.snapshot();
		return true;
	}
	void check_unchanged() const {
		const SnapshotDiff diff = diff_snapshots(before, reader.snapshot());
		MESSAGE("install.unchanged: " << before.files.size() << " index files, " << (diff.clean() ? "clean" : diff.summary()));
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
};

uint32_t tag_total(const SwfCensusTotals &p_totals, uint16_t p_code) {
	for (const SwfTagCount &t : p_totals.tags) {
		if (t.code == p_code) {
			return t.count;
		}
	}
	return 0;
}

const HudPlacement *placement(const std::vector<HudPlacement> &p_rows, std::string_view p_instance) {
	for (const HudPlacement &p : p_rows) {
		if (p.instance == p_instance) {
			return &p;
		}
	}
	return nullptr;
}

void check_at(const std::vector<HudPlacement> &p_rows, const char *p_instance, float p_x, float p_y, float p_sx = 1.0f, float p_sy = 1.0f) {
	const std::string instance_name = p_instance;
	CAPTURE(instance_name);
	const HudPlacement *p = placement(p_rows, p_instance);
	REQUIRE(p != nullptr);
	CHECK(near(p->matrix.tx, p_x, 0.05f));
	CHECK(near(p->matrix.ty, p_y, 0.05f));
	CHECK(near(p->matrix.a, p_sx, 0.0006f));
	CHECK(near(p->matrix.d, p_sy, 0.0006f));
}

void collect(const SwfLayoutNode &p_node, std::vector<const SwfLayoutNode *> &r_out) {
	r_out.push_back(&p_node);
	for (const SwfLayoutNode &c : p_node.children) {
		collect(c, r_out);
	}
}

}

TEST_SUITE("ui-swf") {
	TEST_CASE("swf: the retail movies (G1 census, G2 iconsingame, G6 font names, G7 HUD table, T1, install unchanged)") {
		InstallFixture fx;
		if (!fx.open()) {
			return;
		}
		Result<SwfLibrary> lib = SwfLibrary::open(fx.install);
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
		Result<std::unique_ptr<EbxLibrary>> ui = EbxLibrary::open(fx.install, { "Win32/UI" });
		REQUIRE(ui.ok());
		Result<FontMappingCollection> en = font_mapping_for_text_database(**ui, "localization/us_loc");
		REQUIRE_MESSAGE(en.ok(), (en.ok() ? std::string() : en.error().message));
		Result<std::vector<FontMappingCollection>> all = read_all_font_mappings(**ui);
		REQUIRE(all.ok());
		CHECK(all->size() == 33);
		CHECK(en->partition == "ui/static/fontcollection_en_fontmapcollection");
		CHECK(en->font_library == "ui/static/fontcollection_en_fontlib");
		CHECK(en->fonts.size() == 4);

		SwfCensusOptions options;
		options.font_mapping = &*en;
		options.library_mappings = &*all;
		Result<SwfCensus> census = run_swf_census(*lib, options);
		REQUIRE_MESSAGE(census.ok(), (census.ok() ? std::string() : census.error().message));
		const SwfCensusTotals &t = census->totals;
		MESSAGE(format_census_totals(*census));
		for (const SwfMovieCensus &m : census->movies) {
			if (!m.error.empty()) {
				MESSAGE(m.name << ": " << m.error);
			}
		}

		CHECK(t.movies == 434);
		CHECK(t.parsed == 434);
		CHECK(t.errors == 0);
		CHECK(t.cfx == 371);
		CHECK(t.gfx == 63);
		CHECK(t.multi_sha1_names == 0);
		CHECK(t.labels_past_last_frame == 0);
		const std::pair<uint16_t, uint32_t> golden[] = {
			{ swf_tag::kPlaceObject2, 27832 },
			{ swf_tag::kShowFrame, 23917 },
			{ swf_tag::kEnd, 3217 },
			{ swf_tag::kDefineSprite, 2783 },
			{ swf_tag::kDoAction, 2456 },
			{ swf_tag::kFrameLabel, 2441 },
			{ swf_tag::kExportAssets, 1855 },
			{ swf_tag::kDoInitAction, 1538 },
			{ swf_tag::kDefineEditText, 1488 },
			{ swf_tag::kImportAssets2, 1483 },
			{ swf_tag::kDefineShape, 982 },
			{ swf_tag::kPlaceObject3, 980 },
			{ swf_tag::kCsmTextSettings, 916 },
			{ swf_tag::kGfxDefineSubImage, 820 },
			{ swf_tag::kRemoveObject2, 629 },
			{ swf_tag::kGfxExporterInfo, 434 },
			{ swf_tag::kFileAttributes, 434 },
			{ swf_tag::kMetadata, 401 },
			{ swf_tag::kSetBackgroundColor, 401 },
			{ swf_tag::kGfxDefineExternalImage2, 135 },
			{ swf_tag::kDefineShape3, 117 },
			{ swf_tag::kDefineFont3, 86 },
			{ swf_tag::kDefineShape2, 53 },
			{ swf_tag::kDefineScalingGrid, 29 },
			{ swf_tag::kDefineText, 12 },
			{ swf_tag::kDefineShape4, 7 },
			{ swf_tag::kImportAssets, 1 },
			{ swf_tag::kDoAbc, 0 },
			{ swf_tag::kDefineFont2, 0 },
			{ swf_tag::kDefineFontAlignZones, 0 },
			{ swf_tag::kDefineFontName, 0 },
			{ swf_tag::kGfxDefineExternalImage, 0 },
			{ swf_tag::kGfxFontTextureInfo, 0 },
			{ swf_tag::kGfxDefineCompactedFont, 0 },
		};
		uint64_t golden_sum = 0;
		for (const auto &[code, count] : golden) {
			const std::string tag_name = swf_tag_name(code);
			CAPTURE(tag_name);
			CHECK(tag_total(t, code) == count);
			golden_sum += count;
		}
		uint64_t all_tags = 0;
		for (const SwfTagCount &c : t.tags) {
			all_tags += c.count;
		}
		CHECK(all_tags == golden_sum);
		CHECK(t.action_bytes == 1638265);
		CHECK(t.abc_bytes == 0);
		CHECK(t.labels == 2441);
		CHECK(t.external_images == 135);
		CHECK(t.atlas_textures_missing == 0);
		CHECK(t.sub_images == 820);
		CHECK(t.imported_symbols == 1268);
		CHECK(t.missing_imports == 1);
		CHECK(t.fonts_outlines == 63);
		CHECK(t.fonts_no_glyphs == 21);
		CHECK(t.fonts_codes_only == 2);
		bool deck = false;
		uint32_t outside = 0;
		for (const SwfMovieCensus &m : census->movies) {
			outside += m.sub_images_outside;
			if (m.name == "ui/assets/deckofcards") {
				REQUIRE(m.missing_import_symbols.size() == 1);
				deck = m.missing_import_symbols[0].find("IconsShared_Check") != std::string::npos;
			}
		}
		CHECK(deck);
		CHECK(outside == 0);
		CHECK(t.max_budget_used < swf_default_budget_units(0) + 2 * size_t(kSwfMaxBodyBytes));
		MESSAGE("budgets: parse max " << t.max_budget_used << ", atlas max " << t.max_atlas_budget_used << "; retained max " << t.max_retained_bytes
									  << ", sum " << t.sum_retained_bytes);

		const auto &rule = t.font_names_by_rule;
		MESSAGE("G6: Exact " << rule[0] << ", CaseFolded " << rule[1] << ", Family " << rule[2] << ", Device " << rule[3] << ", Unresolved " << rule[4]);
		CHECK(rule[size_t(FontResolution::Exact)] == 5);
		CHECK(rule[size_t(FontResolution::CaseFolded)] == 2);
		CHECK(rule[size_t(FontResolution::Family)] == 4);
		CHECK(rule[size_t(FontResolution::Device)] == 5);
		CHECK(rule[size_t(FontResolution::Unresolved)] == 0);
		CHECK((t.family_names == std::vector<std::string>{ "$FONT_TITLE", "$HUD", "$HUD_light", "$Menu_light" }));
		std::string family_line = "G6 family-inferred:";
		for (const std::string &n : t.family_names) {
			family_line += " " + n;
		}
		MESSAGE(family_line);
		std::map<std::string, FontResolution> names(t.font_names.begin(), t.font_names.end());
		CHECK(names["$HUD_medium"] == FontResolution::Exact);
		CHECK(names["Venice-SemiBold"] == FontResolution::Exact);
		CHECK(names["$Menu_bold"] == FontResolution::CaseFolded);
		CHECK(names["Arial"] == FontResolution::Device);
		CHECK(names["Purista SemiBold"] == FontResolution::Device);
		CHECK(t.library_font_names > 0);
		CHECK(t.library_font_names_exact == t.library_font_names);
		CHECK(t.library_font_names_not_exact.empty());

		const double t1 = lib->open_seconds() + census->seconds;
		MESSAGE("T1: library open " << lib->open_seconds() << " s + census " << census->seconds << " s = " << t1 << " s"
									<< std::string(kAsan ? " (asan, reported)" : " (limit 15 s; shared machine, noisy)"));
		if (!kAsan) {
			CHECK(t1 <= 15.0);
		}

		Result<std::shared_ptr<const SwfMovie>> icons = lib->load("ui/assets/iconsingame");
		REQUIRE(icons.ok());
		WorkBudget ab(kSwfAtlasBudgetUnits);
		Result<SwfAtlasMap> map = build_atlas_map(**icons, "ui/assets/iconsingame", ab);
		REQUIRE_MESSAGE(map.ok(), (map.ok() ? std::string() : map.error().message));
		REQUIRE(map->images.size() == 1);
		CHECK(map->images[0].file_name == "iconsingame_i1.tga");
		CHECK(map->images[0].texture_res == "ui/assets/iconsingame/iconsingame_i1");
		CHECK(map->images[0].width == 4096);
		CHECK(map->images[0].height == 128);
		Result<res::DxTexture> header = lib->read_atlas_texture_header("ui/assets/iconsingame", (*icons)->external_images[0]);
		REQUIRE_MESSAGE(header.ok(), (header.ok() ? std::string() : header.error().message));
		CHECK(header->width == 4096);
		CHECK(header->height == 128);
		CHECK(header->format == res::TextureFormat::Dxt5);
		const SwfAtlasStats &s = map->stats;
		CHECK(s.sub_images == 162);
		CHECK(s.rects_outside_atlas == 0);
		CHECK(s.labels == 427);
		CHECK(s.labels_with_art == 135);
		CHECK(s.labels_one_piece == 110);
		CHECK(s.labels_several_pieces == 25);
		CHECK(s.labels_empty == 292);
		CHECK(s.sub_images_from_labels == 95);
		CHECK(s.sub_images_from_symbols == 96);
		CHECK(s.sub_images_from_frames == 162);
		std::set<uint16_t> label_sprites;
		for (const SwfAtlasRegion &r : map->labels) {
			label_sprites.insert(r.sprite);
		}
		CHECK(label_sprites.size() == 6);
		CHECK(label_sprites.count(0) == 0);
		struct Spot {
			const char *region;
			std::vector<std::array<uint16_t, 5>> pieces;
		};
		const Spot spots[] = {
			{ "mc_iconsIngameBig/LocalPlayer", { { 123, 576, 58, 599, 83 } } },
			{ "mc_iconsIngameBig/FriendlyPlayer", { { 125, 1070, 58, 1089, 80 } } },
			{ "mc_iconsIngameBig/EnemyPlayer", { { 127, 1028, 58, 1047, 80 } } },
			{ "mc_iconsIngameBig/AmmoCrate", { { 157, 498, 0, 550, 52 } } },
			{ "mc_iconsIngameBig/EnemyFlag", { { 152, 114, 0, 168, 54 } } },
			{ "mc_iconsIngameBig/Tank", { { 197, 2386, 0, 2411, 34 } } },
			{ "mc_iconsIngameBig/CTFFlagEnemyDropped", { { 376, 780, 0, 811, 43 }, { 370, 961, 58, 984, 80 } } },
			{ "mc_iconHUDVehicleFriend/friendDriver", { { 9, 1091, 58, 1115, 79 } } },
			{ "mc_iconsIngameSmall/EnemyFlag", { { 46, 2013, 0, 2047, 34 } } },
			{ "mc_iconsIngameSmall/FriendlyPlayer", {} },
		};
		for (const Spot &spot : spots) {
			const std::string region_name = spot.region;
			CAPTURE(region_name);
			const SwfAtlasRegion *r = map->find(spot.region);
			REQUIRE(r != nullptr);
			REQUIRE(r->pieces.size() == spot.pieces.size());
			for (size_t i = 0; i < spot.pieces.size(); ++i) {
				const SwfAtlasPiece &p = r->pieces[i];
				CHECK((std::array<uint16_t, 5>{ p.sub_image, p.x1, p.y1, p.x2, p.y2 } == spot.pieces[i]));
			}
		}

		const uint32_t expected_rows[14] = { 14, 8, 9, 7, 8, 9, 10, 7, 0, 1, 4, 8, 1, 0 };
		std::map<std::string, std::vector<HudPlacement>> hud;
		size_t total = 0;
		for (size_t i = 0; i < hud_screen_movies().size(); ++i) {
			const std::string screen(hud_screen_movies()[i]);
			CAPTURE(screen);
			WorkBudget hb(kSwfLayoutBudgetUnits);
			Result<std::vector<HudPlacement>> rows = read_hud_placements(*lib, screen, hb);
			REQUIRE_MESSAGE(rows.ok(), (rows.ok() ? std::string() : rows.error().message));
			CHECK(rows->size() == expected_rows[i]);
			Result<std::shared_ptr<const SwfMovie>> sm = lib->load("ui/assets/" + screen);
			REQUIRE(sm.ok());
			CHECK(((*sm)->stage == SwfRect{ 0, 1280, 0, 720 }));
			total += rows->size();
			hud[screen] = std::move(*rows);
		}
		CHECK(total == 86);
		const std::vector<HudPlacement> &mp = hud["hudmpscreen"];
		check_at(mp, "LatencyIndicator_01", 1274, 3);
		check_at(mp, "Hitindicator_01", 640, 360);
		check_at(mp, "PassangerList_01", 707, 547);
		check_at(mp, "Ammo_01", 1194, 669);
		check_at(mp, "Health_01", 1193, 605);
		check_at(mp, "VehicleHealth_01", 1195, 555);
		check_at(mp, "HudMessage_05", 640, 632.2f);
		check_at(mp, "HudMessage_04", 500, 120, 1.0f, 0.999f);
		check_at(mp, "HudMessage_02", 1280, 0);
		check_at(mp, "HudMessage_01", 640, 506);
		check_at(mp, "ScoreAggregator_01", 773, 486);
		check_at(mp, "SupportIconManager_01", 0, 0);
		CHECK(placement(mp, "Ammo_01")->widget == "Ammo");
		CHECK(placement(mp, "Ammo_01")->widget_movie == "ui/assets/ammo");
		CHECK(placement(mp, "Ammo_01")->anchor == HudAnchor::BottomRight);
		const std::vector<HudPlacement> &tdm = hud["hudtdmscreen"];
		check_at(tdm, "HudBackgroundWidget_01", -3, 486.95f, 1.288f, 3.689f);
		check_at(tdm, "Compass_01", 85.05f, 696, 0.99985f);
		check_at(tdm, "Minimap_01", -3, 527);
		check_at(tdm, "TicketCounter_01", -4, 530);
		check_at(tdm, "SquadList_00", 179, 619);
		check_at(tdm, "HudMessage_00", 640, 675);
		check_at(tdm, "MapmarkerManager_00", 0, 0);
		const std::vector<HudPlacement> &cq = hud["hudconquestscreen"];
		check_at(cq, "HudBackgroundWidget_01", -3, 456, 1.288f, 4.172f);
		check_at(cq, "ObjectiveBar_01", -4, 459);
		check_at(cq, "Minimap_01", -3, 527);
		const std::vector<HudPlacement> &hs = hud["hudscreen"];
		check_at(hs, "HudMessage_02", 640, 711);
		check_at(hs, "HudMessage_03", 640, 579);
		check_at(hs, "LogMessage_01", 0, 534);
		check_at(hs, "TooltipMessage_01", 1280, 360);
		check_at(hs, "DamageIndicator_01", 640, 360);
		const std::vector<HudPlacement> &ks = hud["killscreen"];
		check_at(ks, "KillCam_01", 0, 0);
		check_at(ks, "HudMessage_01", 640, 514);
		check_at(ks, "HudMessage_02", 640, 604);
		check_at(ks, "ScoreAggregator_01", 773, 494);

		WorkBudget lb(kSwfLayoutBudgetUnits);
		Result<SwfLayoutNode> ammo = read_layout(*lib, "ammo", {}, lb);
		REQUIRE_MESSAGE(ammo.ok(), (ammo.ok() ? std::string() : ammo.error().message));
		std::vector<const SwfLayoutNode *> nodes;
		collect(*ammo, nodes);

		const SwfLayoutNode *ammo_sprite = nullptr;
		for (const SwfLayoutNode *n : nodes) {
			if (n->kind == SwfLayoutKind::Sprite && child_named(*n, "bgAmmo") != nullptr) {
				ammo_sprite = n;
				break;
			}
		}
		REQUIRE(ammo_sprite != nullptr);
		CHECK(near(ammo_sprite->stage.tx, 640, 0.05f));
		CHECK(near(ammo_sprite->stage.ty, 360, 0.05f));
		const SwfLayoutNode *clip = child_named(*ammo_sprite, "clip");
		REQUIRE(clip != nullptr);
		CHECK(near(clip->local.tx, 2, 0.05f));
		CHECK(near(clip->local.ty, -46, 0.05f));
		const SwfLayoutNode *bg = child_named(*ammo_sprite, "bgAmmo");
		REQUIRE(bg != nullptr);
		CHECK(near(bg->local.tx, -89, 0.05f));
		CHECK(near(bg->local.a, 1.326f, 0.0006f));
		bool hud43 = false;
		bool menu20 = false;
		for (const SwfLayoutNode *n : nodes) {
			if (n->kind != SwfLayoutKind::EditText) {
				continue;
			}
			hud43 = hud43 || (n->text.font == "$HUD_medium" && n->text.size == 43.0f && n->text.initial_text == "000");
			menu20 = menu20 || (n->text.font == "$Menu_medium" && n->text.size == 20.0f);
		}
		CHECK(hud43);
		CHECK(menu20);

		fx.check_unchanged();
	}
}

TEST_SUITE("ui-swf-fuzz") {
	TEST_CASE("swf: seeded mutants of four retail movies fail cleanly or parse fully") {
		InstallFixture fx;
		if (!fx.open()) {
			return;
		}
		Result<SwfLibrary> lib = SwfLibrary::open(fx.install);
		REQUIRE(lib.ok());
		const char *names[] = { "ui/assets/iconsingame", "ui/assets/hudmpscreen", "ui/assets/ammo", "ui/static/fontcollection_en_fontlib" };
		std::mt19937_64 rng(0xb5f3);
		const size_t iters = fuzz_iters(300);
		for (const char *name : names) {
			const std::string movie_name = name;
			CAPTURE(movie_name);
			Result<std::vector<uint8_t>> payload = lib->read_payload(name);
			REQUIRE(payload.ok());
			Result<std::shared_ptr<const SwfMovie>> original = lib->load(name);
			REQUIRE(original.ok());
			const Bytes body = (*original)->body;

			std::vector<std::pair<std::string, std::vector<uint8_t>>> imported;
			for (const SwfImport &im : (*original)->imports) {
				const std::string target = swf_movie_name_for_url(im.url);
				if (target.empty() || lib->find(target) == nullptr) {
					continue;
				}
				bool have = false;
				for (const auto &[n, b] : imported) {
					have = have || n == target;
				}
				if (!have) {
					Result<std::vector<uint8_t>> p = lib->read_payload(target);
					REQUIRE(p.ok());
					imported.emplace_back(target, std::move(*p));
				}
			}
			size_t parsed = 0;
			size_t refused = 0;
			const auto t0 = Clock::now();
			for (size_t i = 0; i < iters; ++i) {
				Bytes b;
				if (i % 4 == 0) {
					b = *payload;
					mutate(b, rng);
				} else {
					Bytes mb = body;
					mutate(mb, rng);
					b = gfx_from_body(mb, (*original)->version);
				}
				Result<SwfMovie> m = SwfMovie::parse(b);
				if (!m) {
					++refused;
					CHECK(!m.error().message.empty());
					continue;
				}
				++parsed;
				WorkBudget ab(kSwfAtlasBudgetUnits);
				(void)build_atlas_map(*m, name, ab);
				for (const SwfFontInfo &f : m->fonts) {
					WorkBudget fb(kUiFontConvertBudgetUnits);
					Result<SwfFontOutlines> o = load_font_outlines(*m, f, fb);
					if (o && o->kind == SwfFontKind::Outlines) {
						(void)write_truetype(*o, {}, fb);
					}
				}
				if (i % 10 == 1) {
					std::vector<std::pair<std::string, std::vector<uint8_t>>> payloads = imported;
					payloads.emplace_back(name, std::move(b));
					const SwfLibrary mem = SwfLibrary::from_memory(std::move(payloads));
					WorkBudget lb(kSwfLayoutBudgetUnits);
					(void)read_layout(mem, name, {}, lb);
				}
			}
			const double s = std::chrono::duration<double>(Clock::now() - t0).count();
			MESSAGE(std::string(name) << ": " << iters << " mutants, " << parsed << " parsed, " << refused << " refused, " << s << " s");
			CHECK(parsed + refused == iters);
		}
		fx.check_unchanged();
	}
}

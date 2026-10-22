#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/ui/ui_fonts.h"
#include "test_support.h"
#include "ui_swf_writer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui1test;

namespace {

using Clock = std::chrono::steady_clock;

bool near(float a, float b, float tol) {
	return std::fabs(a - b) <= tol;
}

Bytes action_tag(const Bytes &p_code) {
	Bytes t;
	tag(t, 12, p_code);
	return t;
}

struct Avm {
	Bytes b;
	Avm &op(uint8_t p_op) {
		b.push_back(p_op);
		return *this;
	}
	Avm &rec(uint8_t p_op, const Bytes &p_payload) {
		b.push_back(p_op);
		b.push_back(uint8_t(p_payload.size()));
		b.push_back(uint8_t(p_payload.size() >> 8));
		b.insert(b.end(), p_payload.begin(), p_payload.end());
		return *this;
	}
	Avm &stop() { return op(0x07); }
	Avm &play() { return op(0x06); }
	Avm &goto_frame(uint16_t p_frame) { return rec(0x81, { uint8_t(p_frame), uint8_t(p_frame >> 8) }); }
	Avm &goto_label(std::string_view p_label) {
		Bytes s(p_label.begin(), p_label.end());
		s.push_back(0);
		return rec(0x8C, s);
	}
	Avm &push_string(std::string_view p_s) {
		Bytes s{ 0 };
		s.insert(s.end(), p_s.begin(), p_s.end());
		s.push_back(0);
		return rec(0x96, s);
	}
	Avm &push_int(int32_t p_v) {
		const uint32_t v = uint32_t(p_v);
		return rec(0x96, { 7, uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) });
	}
	Avm &goto_frame2(bool p_play) { return rec(0x9F, { uint8_t(p_play ? 1 : 0) }); }
	Avm &set_target(std::string_view p_t) {
		Bytes s(p_t.begin(), p_t.end());
		s.push_back(0);
		return rec(0x8B, s);
	}
	Avm &branch_if(int16_t p_offset) { return rec(0x9D, { uint8_t(p_offset), uint8_t(uint16_t(p_offset) >> 8) }); }
	Avm &end() { return op(0x00); }
};

SwfFrameScript scan_ok(const Bytes &p_code) {
	Result<SwfFrameScript> s = scan_frame_actions(p_code, 3);
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	return std::move(*s);
}

ShapePath square_path(int32_t p_size) {
	ShapePath p;
	p.fill1 = 1;
	p.segs = { { p_size, 0 }, { p_size, p_size }, { 0, p_size }, { 0, 0 } };
	return p;
}

Bytes square_shape(uint16_t p_id) {
	ShapeFill f;
	f.r = 10;
	f.g = 20;
	f.b = 30;
	return define_shape_paths(3, p_id, 10, 10, { f }, {}, { square_path(200) });
}

Place at(uint16_t p_depth, std::optional<uint16_t> p_char, float p_tx, std::optional<std::string> p_name = std::nullopt, bool p_move = false) {
	Place p;
	p.depth = p_depth;
	p.character = p_char;
	p.matrix = Mat{ 1, 0, 0, 1, p_tx, 0 };
	p.name = std::move(p_name);
	p.move = p_move;
	return p;
}

EditText label_text(uint16_t p_id) {
	EditText t;
	t.id = p_id;
	t.font_class = "$Synth";
	t.height = 20;
	t.text = "000";
	t.variable = "";
	t.extra_flags = SwfEditText::kUseOutlines | SwfEditText::kAutoSize;
	return t;
}

Bytes widget_movie() {
	return movie(cat({ square_shape(1), define_sprite(2, 1, cat({ place_object2(at(1, 1, 5, std::string("face"))), show_frame(), end_tag() })),
			export_assets({ { 2, "Widget" } }), show_frame(), end_tag() }));
}

Bytes scene_movie() {
	const Bytes anim = cat({ place_object2(at(1, 1, 0, std::string("box"))), show_frame(), place_object2(at(1, std::nullopt, 10, std::nullopt, true)),
			place_object2(at(2, 1, 50, std::string("second"))), show_frame(), frame_label("mid"), place_object2(at(1, std::nullopt, 20, std::nullopt, true)),
			remove_object2(2), action_tag(Avm().stop().end().b), show_frame(), frame_label("end"), remove_object2(1), place_object2(at(1, 1, 30, std::string("box"))),
			action_tag(Avm().goto_label("mid").play().end().b), show_frame(), end_tag() });
	Place mask = at(1, 1, 0, std::string("mask"));
	mask.clip_depth = 3;
	const Bytes masked = cat({ place_object2(mask), place_object2(at(2, 1, 0, std::string("in"))), place_object2(at(4, 1, 0, std::string("out"))), show_frame(),
			end_tag() });
	const Bytes row = cat({ place_object2(at(1, 4, 0, std::string("label"))), show_frame(), show_frame(), action_tag(Avm().stop().end().b), show_frame(),
			end_tag() });
	Place effects = at(3, 9, 7, std::string("widget"));
	effects.blend = 8;
	return movie(cat({ import_assets("widget.swf", { { 9, "Widget" } }), square_shape(1), define_sprite(2, 4, anim), define_sprite(3, 1, masked),
			define_edit_text(label_text(4)), define_sprite(5, 3, row), define_sprite(6, 1, cat({ show_frame(), end_tag() })),
			export_assets({ { 2, "Anim" }, { 5, "Row" }, { 6, "Holder" } }), place_object2(at(1, 2, 100, std::string("anim"))),
			place_object2(at(2, 3, 0, std::string("masked"))), place_object2(effects, true), place_object2(at(4, 6, 0, std::string("holder"))),
			place_object2(at(5, 1, 0)), show_frame(), end_tag() }));
}

SwfLibrary scene_library() {
	return SwfLibrary::from_memory({ { "ui/assets/synth", scene_movie() }, { "ui/assets/widget", widget_movie() } });
}

SwfScene open_scene(const SwfLibrary &p_lib) {
	Result<SwfScene> s = SwfScene::open(p_lib, "synth");
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	return std::move(*s);
}

uint32_t change_of(const std::vector<SwfSceneChange> &p_changes, SwfNodeId p_id) {
	uint32_t what = 0;
	for (const SwfSceneChange &c : p_changes) {
		what |= c.node == p_id ? c.what : 0;
	}
	return what;
}

SwfFontOutlines synth_outlines() {
	SwfFontOutlines o;
	o.name = "Synth Sans Medium";
	o.has_layout = true;
	o.ascent = 16384;
	o.descent = 4096;
	o.codes = { 0x20, 0x30, 0x41, 0x56, 0x57, 0x26, 0x42, 0x78 };
	o.advances = { 5120, 10240, 12288, 12288, 20480, 10240, 10240, 10240 };
	o.glyphs.resize(o.codes.size());
	o.bounds.resize(o.codes.size());
	o.kerning = { { 0x41, 0x56, -2048 } };
	return o;
}

SwfFontSet synth_fonts() {
	FontMappingCollection m;
	m.name = "synth";
	m.fonts.push_back({ "Synth Sans Medium", { "$Synth" } });
	return SwfFontSet::from_outlines(m, { synth_outlines() });
}

SwfEditText synth_field(float p_width, float p_height, uint16_t p_extra = 0) {
	SwfEditText f;
	f.bounds = SwfRect{ 0, p_width, 0, p_height };
	f.flags = uint16_t(SwfEditText::kHasText | SwfEditText::kHasFontClass | SwfEditText::kHasLayout | SwfEditText::kHasTextColor | p_extra);
	f.font_class = "$Synth";
	f.height = 20;
	f.color = SwfColor{ 1, 2, 3, 255 };
	return f;
}

SwfTextFormat synth_format() {
	SwfTextFormat f;
	f.font = "$Synth";
	f.size = 20;
	return f;
}

SwfTextLayout lay(const SwfFontSet &p_fonts, const SwfEditText &p_field, const SwfRichText &p_text, const SwfTextOptions &p_options = {}) {
	WorkBudget b(kSwfTextBudgetUnits);
	Result<SwfTextLayout> l = swf_layout_text(p_fonts, p_field, p_text, p_options, b);
	REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
	return std::move(*l);
}

SwfRichText plain(std::string_view p_text, const SwfTextFormat &p_format) {
	Result<SwfRichText> r = swf_plain_text(p_text, p_format);
	REQUIRE(r.ok());
	return std::move(*r);
}

SwfRichText html(std::string_view p_html, const SwfTextFormat &p_format) {
	WorkBudget b(kSwfTextBudgetUnits);
	Result<SwfRichText> r = swf_parse_html(p_html, p_format, b);
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	return std::move(*r);
}

Bytes po3_with_filter_bytes(const Bytes &p_filters, std::optional<uint8_t> p_blend, std::optional<uint8_t> p_cache, size_t p_trailing = 0) {
	Writer w;
	w.u8(0x02 | 0x20);
	w.u8(0x01 | (p_blend ? 0x02 : 0) | (p_cache ? 0x04 : 0));
	w.u16(1);
	w.u16(1);
	w.str("fx");
	w.raw(p_filters);
	if (p_blend) {
		w.u8(*p_blend);
	}
	if (p_cache) {
		w.u8(*p_cache);
	}
	w.raw(Bytes(p_trailing, 0));
	Bytes t;
	tag(t, 70, w.b);
	return t;
}

void fixed(Writer &w, double p_v) {
	w.u32(uint32_t(int32_t(std::lround(p_v * 65536.0))));
}
void fixed8(Writer &w, double p_v) {
	w.s16(int32_t(std::lround(p_v * 256.0)));
}
void rgba(Writer &w, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	w.u8(r);
	w.u8(g);
	w.u8(b);
	w.u8(a);
}
void f32(Writer &w, float p_v) {
	uint32_t u = 0;
	std::memcpy(&u, &p_v, 4);
	w.u32(u);
}

Bytes all_filters() {
	Writer w;
	w.u8(8);
	w.u8(0);
	rgba(w, 1, 2, 3, 128);
	fixed(w, 4.5);
	fixed(w, 5.25);
	fixed(w, 0.785);
	fixed(w, 3);
	fixed8(w, 1.5);
	w.u8(0x80 | 0x20 | 2);
	w.u8(1);
	fixed(w, 2);
	fixed(w, 3);
	w.u8(3 << 3);
	w.u8(2);
	rgba(w, 9, 8, 7, 255);
	fixed(w, 6);
	fixed(w, 7);
	fixed8(w, 2.5);
	w.u8(0x40 | 0x20 | 3);
	w.u8(3);
	rgba(w, 1, 1, 1, 255);
	rgba(w, 2, 2, 2, 255);
	fixed(w, 1);
	fixed(w, 1);
	fixed(w, 0.5);
	fixed(w, 2);
	fixed8(w, 1);
	w.u8(0x20 | 0x10 | 1);
	w.u8(4);
	w.u8(2);
	rgba(w, 10, 0, 0, 255);
	rgba(w, 0, 20, 0, 128);
	w.u8(0);
	w.u8(255);
	fixed(w, 8);
	fixed(w, 8);
	fixed(w, 0);
	fixed(w, 0);
	fixed8(w, 1);
	w.u8(0x20 | 1);
	w.u8(5);
	w.u8(3);
	w.u8(2);
	f32(w, 9.0f);
	f32(w, 0.5f);
	for (int i = 0; i < 6; ++i) {
		f32(w, float(i));
	}
	rgba(w, 4, 5, 6, 7);
	w.u8(0x02 | 0x01);
	w.u8(6);
	for (int i = 0; i < 20; ++i) {
		f32(w, float(i) / 4.0f);
	}
	w.u8(7);
	w.u8(1);
	rgba(w, 50, 60, 70, 255);
	w.u8(128);
	fixed(w, 1);
	fixed(w, 2);
	fixed(w, 3);
	fixed(w, 4);
	fixed8(w, 0.5);
	w.u8(0x20 | 2);
	return w.b;
}

Bytes filter_movie(const Bytes &p_po3) {
	return movie(cat({ square_shape(1), p_po3, show_frame(), end_tag() }));
}

}

TEST_SUITE("ui-swf-unit") {
	TEST_CASE("swf actions: frame scripts scan to stop/play/goto ops; branches, targets and functions are kept apart") {
		const SwfFrameScript a = scan_ok(Avm().stop().play().goto_frame(4).goto_label("intro").end().b);
		REQUIRE(a.ops.size() == 4);
		CHECK(a.frame == 3);
		CHECK(a.ops[0].kind == SwfFrameOpKind::Stop);
		CHECK(a.ops[1].kind == SwfFrameOpKind::Play);
		CHECK(a.ops[2].kind == SwfFrameOpKind::GotoFrame);
		CHECK(a.ops[2].frame == 4);
		CHECK(a.ops[3].kind == SwfFrameOpKind::GotoLabel);
		CHECK(a.ops[3].label == "intro");
		CHECK(a.other_actions == 0);
		CHECK_FALSE(a.ops[3].after_branch);

		const SwfFrameScript b = scan_ok(Avm().push_string("onExit").goto_frame2(true).push_int(7).goto_frame2(false).push_string("12").goto_frame2(false).end().b);
		REQUIRE(b.ops.size() == 3);
		CHECK(b.ops[0].kind == SwfFrameOpKind::GotoLabel);
		CHECK(b.ops[0].label == "onExit");
		CHECK(b.ops[0].and_play);
		CHECK(b.ops[1].kind == SwfFrameOpKind::GotoFrame);
		CHECK(b.ops[1].frame == 6);
		CHECK_FALSE(b.ops[1].and_play);
		CHECK(b.ops[2].frame == 11);

		const SwfFrameScript c = scan_ok(Avm().branch_if(1).stop().set_target("other").play().set_target("").stop().goto_frame2(true).end().b);
		CHECK(c.branches == 1);
		CHECK(c.set_target);
		CHECK(c.unresolved_goto);
		REQUIRE(c.ops.size() == 2);
		CHECK(c.ops[0].after_branch);
		CHECK(c.ops[1].after_branch);
		CHECK(c.other_actions == 2);

		Bytes fn = { 0x9B };
		const Bytes body = Avm().stop().stop().b;
		const Bytes header = { 'f', 0, 0, 0, uint8_t(body.size()), 0 };
		fn.push_back(uint8_t(header.size()));
		fn.push_back(0);
		fn.insert(fn.end(), header.begin(), header.end());
		fn.insert(fn.end(), body.begin(), body.end());
		const Bytes pool = Avm().rec(0x88, { 2, 0, 'a', 0, 'z', 'z', 0 }).rec(0x96, { 8, 1 }).goto_frame2(false).b;
		Bytes code = fn;
		code.insert(code.end(), pool.begin(), pool.end());
		code.push_back(0x07);
		code.push_back(0);
		const SwfFrameScript d = scan_ok(code);
		CHECK(d.functions == 1);
		REQUIRE(d.ops.size() == 2);
		CHECK(d.ops[0].kind == SwfFrameOpKind::GotoLabel);
		CHECK(d.ops[0].label == "zz");
		CHECK(d.ops[1].kind == SwfFrameOpKind::Stop);

		CHECK_FALSE(scan_frame_actions(Bytes{ 0x81, 9, 0, 1 }, 0).ok());
		CHECK_FALSE(scan_frame_actions(Avm().rec(0x96, { 42, 0 }).end().b, 0).ok());
		CHECK_FALSE(scan_frame_actions(Avm().rec(0x96, { 8, 5 }).end().b, 0).ok());
		const Result<SwfFrameScript> past = scan_frame_actions(Bytes{ 0x9B, 6, 0, 'f', 0, 0, 0, 200, 0 }, 0);
		REQUIRE_FALSE(past.ok());
		CHECK(past.error().message.find("runs past the script") != std::string::npos);

		const Result<SwfMovie> m = SwfMovie::parse(movie(cat({ action_tag(Avm().stop().end().b), action_tag(Avm().branch_if(0).play().end().b), show_frame(),
				action_tag(Avm().goto_label("x").end().b), show_frame(), end_tag() })));
		REQUIRE(m.ok());
		REQUIRE(m->root.scripts.size() == 2);
		REQUIRE(m->root.script_at(0) != nullptr);
		CHECK(m->root.script_at(0)->ops.size() == 2);
		CHECK_FALSE(m->root.script_at(0)->ops[0].after_branch);
		CHECK(m->root.script_at(0)->ops[1].after_branch);
		CHECK(m->root.script_at(1)->ops[0].label == "x");
		CHECK(m->root.script_at(2) == nullptr);
		CHECK_FALSE(SwfMovie::parse(movie(cat({ action_tag({ 0x96, 9, 0, 0 }), show_frame(), end_tag() }))).ok());
	}

	TEST_CASE("swf placements: PlaceObject3 filters, blend, cache and clip depth decode; unknown filters and trailing bytes are refused") {
		const Result<SwfMovie> m = SwfMovie::parse(filter_movie(po3_with_filter_bytes(all_filters(), uint8_t(8), uint8_t(1))));
		REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
		REQUIRE(m->root.events.size() == 1);
		const SwfDisplayEvent &e = m->root.events[0];
		CHECK(e.has_filters);
		CHECK(e.has_blend_mode);
		CHECK(e.blend_mode == swf_blend::kAdd);
		CHECK(e.has_cache_as_bitmap);
		CHECK(e.cache_as_bitmap == 1);
		CHECK(e.name == "fx");
		REQUIRE(e.filters.size() == 8);
		const SwfFilter &ds = e.filters[0];
		CHECK(ds.kind == SwfFilterKind::DropShadow);
		CHECK(ds.color == SwfColor{ 1, 2, 3, 128 });
		CHECK(near(ds.blur_x, 4.5f, 1e-4f));
		CHECK(near(ds.blur_y, 5.25f, 1e-4f));
		CHECK(near(ds.angle, 0.785f, 1e-4f));
		CHECK(near(ds.distance, 3.0f, 1e-4f));
		CHECK(near(ds.strength, 1.5f, 1e-4f));
		CHECK(ds.inner);
		CHECK_FALSE(ds.knockout);
		CHECK(ds.composite_source);
		CHECK(ds.passes == 2);
		CHECK(e.filters[1].kind == SwfFilterKind::Blur);
		CHECK(near(e.filters[1].blur_y, 3.0f, 1e-4f));
		CHECK(e.filters[1].passes == 3);
		const SwfFilter &glow = e.filters[2];
		CHECK(glow.kind == SwfFilterKind::Glow);
		CHECK(glow.color == SwfColor{ 9, 8, 7, 255 });
		CHECK(near(glow.strength, 2.5f, 1e-4f));
		CHECK(glow.knockout);
		CHECK(glow.passes == 3);
		const SwfFilter &bevel = e.filters[3];
		CHECK(bevel.highlight == SwfColor{ 2, 2, 2, 255 });
		CHECK(bevel.on_top);
		CHECK(bevel.passes == 1);
		const SwfFilter &gg = e.filters[4];
		CHECK(gg.kind == SwfFilterKind::GradientGlow);
		REQUIRE(gg.gradient.size() == 2);
		CHECK(gg.gradient[1].ratio == 255);
		CHECK(gg.gradient[1].color == SwfColor{ 0, 20, 0, 128 });
		const SwfFilter &conv = e.filters[5];
		CHECK(conv.kind == SwfFilterKind::Convolution);
		CHECK(conv.matrix_x == 3);
		CHECK(conv.matrix_y == 2);
		CHECK(conv.divisor == 9.0f);
		REQUIRE(conv.matrix.size() == 6);
		CHECK(conv.matrix[5] == 5.0f);
		CHECK(conv.default_color == SwfColor{ 4, 5, 6, 7 });
		CHECK(conv.clamp);
		CHECK(conv.preserve_alpha);
		CHECK(e.filters[6].color_matrix[19] == 4.75f);
		CHECK(e.filters[7].kind == SwfFilterKind::GradientBevel);
		CHECK(e.filters[7].gradient.size() == 1);
		CHECK(std::string(swf_filter_kind_name(SwfFilterKind::GradientBevel)) == "gradient_bevel");

		Bytes unknown;
		unknown.push_back(1);
		unknown.push_back(9);
		const Result<SwfMovie> bad = SwfMovie::parse(filter_movie(po3_with_filter_bytes(unknown, std::nullopt, std::nullopt)));
		REQUIRE_FALSE(bad.ok());
		CHECK(bad.error().message.find("filter id 9 is not a SWF filter") != std::string::npos);
		const Result<SwfMovie> trailing = SwfMovie::parse(filter_movie(po3_with_filter_bytes(Bytes{ 0 }, std::nullopt, std::nullopt, 3)));
		REQUIRE_FALSE(trailing.ok());
		CHECK(trailing.error().message.find("ends 3 bytes before its tag") != std::string::npos);

		Place clip = at(2, 1, 0);
		clip.clip_depth = 9;
		const Result<SwfMovie> c = SwfMovie::parse(filter_movie(place_object2(clip)));
		REQUIRE(c.ok());
		CHECK(c->root.events[0].has_clip_depth);
		CHECK(c->root.events[0].clip_depth == 9);
		CHECK_FALSE(c->root.events[0].has_filters);
	}

	TEST_CASE("swf scene: a display tree across an import, with paths, masks, timelines, goto replay and attachMovie") {
		const SwfLibrary lib = scene_library();
		SwfScene s = open_scene(lib);
		const std::vector<SwfSceneChange> first = s.take_changes();
		CHECK(first.size() == s.stats().live_nodes);
		CHECK(std::all_of(first.begin(), first.end(), [](const SwfSceneChange &c) { return c.what == swf_change::kCreated; }));
		CHECK(s.stats().unresolved == 0);
		CHECK(s.frame_rate() == 30.0f);

		const SwfSceneNode *anim = s.find_node("anim");
		REQUIRE(anim != nullptr);
		CHECK(anim->kind == SwfNodeKind::Sprite);
		CHECK(anim->symbol == "Anim");
		CHECK(anim->total_frames() == 4);
		CHECK(anim->frame == 0);
		CHECK(anim->playing);
		const SwfSceneNode *box = s.find_node("anim.box");
		REQUIRE(box != nullptr);
		CHECK(box->kind == SwfNodeKind::Shape);
		REQUIRE(box->shape != nullptr);
		CHECK(near(box->world.tx, 100.0f, 1e-4f));
		CHECK(s.find_node("_root.anim.box") == box);
		CHECK(s.find_node("anim.nothing") == nullptr);

		const SwfSceneNode *widget = s.find_node("widget");
		REQUIRE(widget != nullptr);
		CHECK(widget->movie_name == "ui/assets/widget");
		CHECK(widget->symbol == "Widget");
		CHECK(widget->blend_mode == swf_blend::kAdd);
		const SwfSceneNode *face = s.find_node("widget.face");
		REQUIRE(face != nullptr);
		CHECK(near(face->world.tx, 12.0f, 1e-4f));
		CHECK(s.find_node("#5") != nullptr);
		CHECK(s.find_node("#5")->path == "#5");

		const SwfSceneNode *mask = s.find_node("masked.mask");
		const SwfSceneNode *in = s.find_node("masked.in");
		const SwfSceneNode *out = s.find_node("masked.out");
		REQUIRE((mask && in && out));
		CHECK(mask->is_mask);
		CHECK(in->masks == std::vector<SwfNodeId>{ mask->id });
		CHECK(out->masks.empty());

		const SwfNodeId box_id = box->id;
		REQUIRE(s.advance().ok());
		std::vector<SwfSceneChange> ch = s.take_changes();
		CHECK(s.find_node("anim")->frame == 1);
		CHECK((change_of(ch, s.find("anim")) & swf_change::kFrame) != 0);
		CHECK((change_of(ch, box_id) & swf_change::kMatrix) != 0);
		CHECK(near(s.node(box_id)->world.tx, 110.0f, 1e-4f));
		REQUIRE(s.find_node("anim.second") != nullptr);
		const SwfNodeId second = s.find("anim.second");
		CHECK(change_of(ch, second) == swf_change::kCreated);
		CHECK(change_of(ch, s.find("masked")) == 0);

		REQUIRE(s.advance().ok());
		ch = s.take_changes();
		CHECK(s.find_node("anim")->frame == 2);
		CHECK_FALSE(s.find_node("anim")->playing);
		CHECK(change_of(ch, second) == swf_change::kRemoved);
		CHECK(s.node(second) == nullptr);
		REQUIRE(s.advance(5).ok());
		CHECK(s.find_node("anim")->frame == 2);
		CHECK(s.take_changes().empty());

		REQUIRE(s.goto_label("anim", "end", false).ok());
		CHECK(s.find_node("anim")->frame == 2);
		CHECK_FALSE(s.find_node("anim")->playing);
		CHECK(s.node(box_id) == nullptr);
		REQUIRE(s.find_node("anim.box") != nullptr);
		const SwfNodeId box2 = s.find("anim.box");
		CHECK(box2 != box_id);
		CHECK(s.stats().labels_missing == 0);
		ch = s.take_changes();
		CHECK(change_of(ch, box_id) == swf_change::kRemoved);
		CHECK(change_of(ch, box2) == swf_change::kCreated);
		CHECK(near(s.node(box2)->world.tx, 120.0f, 1e-4f));

		REQUIRE(s.goto_frame("anim", 3, false).ok());
		CHECK(s.find_node("anim")->frame == 2);
		const SwfNodeId box3 = s.find("anim.box");
		CHECK(box3 != box2);
		REQUIRE(s.stop("anim").ok());
		REQUIRE(s.set_matrix("anim.box", SwfMatrix{ 2, 0, 0, 2, 1, 1 }).ok());
		REQUIRE(s.goto_frame("anim", 1, false).ok());
		CHECK(s.find_node("anim")->frame == 1);
		CHECK(s.find_node("anim.box")->id == box3);
		CHECK(near(s.find_node("anim.box")->matrix.a, 2.0f, 1e-6f));
		CHECK(s.find_node("anim.second") != nullptr);
		CHECK(s.stats().replays >= 1);

		REQUIRE(s.set_visible("anim.box", false).ok());
		s.take_changes();
		CHECK_FALSE(s.find_node("anim.box")->world_visible);
		REQUIRE(s.goto_frame("anim", 0, true).ok());
		CHECK(s.find_node("anim.box")->id == box3);
		CHECK_FALSE(s.find_node("anim.box")->visible);
		REQUIRE(s.advance().ok());
		REQUIRE(s.stop("anim").ok());
		CHECK(s.find_node("anim")->frame == 1);
		ch = s.take_changes();
		CHECK(s.find_node("anim.box")->id == box3);
		CHECK_FALSE(s.find_node("anim.box")->visible);
		CHECK_FALSE(s.find_node("anim.box")->world_visible);
		CHECK((change_of(ch, box3) & swf_change::kVisible) == 0);
		REQUIRE(s.set_visible("anim.box", true).ok());
		ch = s.take_changes();
		CHECK(s.find_node("anim.box")->world_visible);
		CHECK((change_of(ch, box3) & swf_change::kVisible) != 0);
		CHECK_FALSE(s.goto_frame("anim", 4, false).ok());
		CHECK_FALSE(s.goto_label("anim", "nope", false).ok());
		CHECK_FALSE(s.goto_frame("anim.box", 0, false).ok());

		REQUIRE(s.set_alpha("masked", 0.5f).ok());
		REQUIRE(s.set_visible("masked.out", false).ok());
		ch = s.take_changes();
		CHECK((change_of(ch, s.find("masked.in")) & swf_change::kColor) != 0);
		CHECK(near(s.find_node("masked.in")->world_color.mul[3], 0.5f, 1e-6f));
		CHECK_FALSE(s.find_node("masked.out")->world_visible);
		CHECK((change_of(ch, s.find("masked.out")) & swf_change::kVisible) != 0);

		Result<SwfNodeId> row = s.attach_movie("holder", "Row", "row0", 3);
		REQUIRE_MESSAGE(row.ok(), (row.ok() ? std::string() : row.error().message));
		const SwfSceneNode *r = s.node(*row);
		CHECK(r->path == "holder.row0");
		CHECK(r->dynamic);
		CHECK(r->as2_depth() == 3);
		CHECK(r->symbol == "Row");
		const SwfSceneNode *label = s.find_node("holder.row0.label");
		REQUIRE(label != nullptr);
		CHECK(label->kind == SwfNodeKind::EditText);
		CHECK(label->text == "000");
		CHECK(label->edit_text->align == swf_align::kRight);
		CHECK(label->text_options.auto_size == SwfTextAutoSize::Right);
		REQUIRE(s.set_text("holder.row0.label", "<font face=\"$Synth\">A</font>", true).ok());
		CHECK_FALSE(s.set_text("holder.row0", "x").ok());
		CHECK(swf_node_text_bounds(*label) == label->edit_text->bounds);
		const uint32_t text_rev = label->text_revision;
		const SwfRect wide{ label->edit_text->bounds.x_min, label->edit_text->bounds.x_max + 40.0f, label->edit_text->bounds.y_min,
			label->edit_text->bounds.y_max };
		REQUIRE(s.set_text_bounds("holder.row0.label", wide).ok());
		CHECK(swf_node_text_bounds(*label) == wide);
		CHECK(label->edit_text->bounds != wide);
		CHECK(label->text_revision == text_rev + 1);
		REQUIRE(s.set_text_bounds("holder.row0.label", wide).ok());
		CHECK(label->text_revision == text_rev + 1);
		CHECK_FALSE(s.set_text_bounds("holder.row0", wide).ok());
		CHECK_FALSE(s.set_text_bounds("holder.row0.label", SwfRect{ 1.0f, 0.0f, 0.0f, 1.0f }).ok());
		CHECK_FALSE(s.set_text_bounds("holder.row0.label", SwfRect{ 0.0f, std::nanf(""), 0.0f, 1.0f }).ok());
		ch = s.take_changes();
		CHECK(change_of(ch, *row) == swf_change::kCreated);
		CHECK((change_of(ch, s.find("holder")) & swf_change::kChildren) != 0);
		REQUIRE(s.advance(4).ok());
		CHECK(s.node(*row)->frame == 2);
		CHECK_FALSE(s.node(*row)->playing);
		CHECK_FALSE(s.attach_movie("holder", "Nope", "x", 1).ok());
		CHECK_FALSE(s.attach_movie("holder", "Row", "x", 2000000).ok());
		CHECK_FALSE(s.remove_movie("anim").ok());
		const SwfNodeId label_id = s.find("holder.row0.label");
		REQUIRE(s.remove_movie("holder.row0").ok());
		ch = s.take_changes();
		CHECK(change_of(ch, *row) == swf_change::kRemoved);
		CHECK(change_of(ch, label_id) == swf_change::kRemoved);
		CHECK(s.find_node("holder.row0") == nullptr);
		CHECK(s.stats().live_nodes == s.draw_order().size());

		const SwfSceneNode *before = s.find_node("anim");
		REQUIRE(s.play("anim").ok());
		REQUIRE(s.advance_seconds(1.0 / 30.0 + 1e-6).ok());
		CHECK(before->frame == 2);
		CHECK_FALSE(s.advance_seconds(-1.0).ok());
		REQUIRE(s.advance_seconds(100.0).ok());
		CHECK(s.stats().frames_dropped == 3000 - kSwfSceneMaxCatchUpFrames);
	}

	TEST_CASE("swf scene: node slots are reused as a timeline churns, and a removed node's id never resolves again") {
		const Bytes churn = cat({ place_object2(at(1, 1, 0, std::string("box"))), show_frame(), remove_object2(1), place_object2(at(1, 1, 5, std::string("box"))),
				show_frame(), end_tag() });
		const Bytes m = movie(cat({ square_shape(1), define_sprite(2, 2, churn), place_object2(at(1, 2, 0, std::string("spin"))), show_frame(), end_tag() }));
		const SwfLibrary lib = SwfLibrary::from_memory({ { "ui/assets/churn", m } });
		Result<SwfScene> s = SwfScene::open(lib, "churn");
		REQUIRE(s.ok());
		s->take_changes();
		CHECK(s->stats().live_nodes == 3);
		CHECK(s->stats().node_slots == 3);
		constexpr uint32_t kFrames = 1000;
		SwfNodeId prev = s->find("spin.box");
		REQUIRE(prev != kSwfNoNode);
		std::vector<SwfNodeId> ids{ prev };
		uint32_t churned = 0;
		for (uint32_t i = 0; i < kFrames; ++i) {
			REQUIRE(s->advance().ok());
			const std::vector<SwfSceneChange> ch = s->take_changes();
			const SwfNodeId now = s->find("spin.box");
			REQUIRE(now != kSwfNoNode);
			const bool fresh = now != prev && s->node(prev) == nullptr && change_of(ch, prev) == swf_change::kRemoved && change_of(ch, now) == swf_change::kCreated;
			churned += fresh ? 1 : 0;
			ids.push_back(now);
			prev = now;
		}
		CHECK(churned == kFrames);
		std::sort(ids.begin(), ids.end());
		CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
		CHECK(s->stats().created == 3 + kFrames);
		CHECK(s->stats().live_nodes == 3);
		CHECK(s->stats().node_slots == 3);
		CHECK(s->node(kSwfNoNode) == nullptr);
	}

	TEST_CASE("swf scene: hostile movies are refused or bounded (self-nesting sprite, node cap, missing import)") {
		const Bytes self = movie(cat({ define_sprite(2, 1, cat({ place_object2(at(1, 2, 0)), show_frame(), end_tag() })), place_object2(at(1, 2, 0)), show_frame(),
				end_tag() }));
		const SwfLibrary a = SwfLibrary::from_memory({ { "ui/assets/self", self } });
		const Result<SwfScene> nested = SwfScene::open(a, "self");
		REQUIRE_FALSE(nested.ok());
		CHECK(nested.error().message.find("nests deeper than") != std::string::npos);

		const SwfLibrary b = SwfLibrary::from_memory({ { "ui/assets/synth", scene_movie() } });
		const Result<SwfScene> lonely = SwfScene::open(b, "synth");
		REQUIRE(lonely.ok());
		CHECK(lonely->stats().unresolved == 1);
		CHECK(lonely->find_node("widget")->kind == SwfNodeKind::Unresolved);
		CHECK(lonely->find_node("widget")->unresolved.find("widget.swf") != std::string::npos);

		const SwfLibrary c = scene_library();
		SwfSceneOptions o;
		o.max_nodes = 4;
		const Result<SwfScene> capped = SwfScene::open(c, "synth", o);
		REQUIRE_FALSE(capped.ok());
		CHECK(capped.error().message.find("live nodes") != std::string::npos);

		const Bytes ping = movie(cat({ define_sprite(2, 2, cat({ action_tag(Avm().goto_frame(1).play().end().b), show_frame(), action_tag(Avm().goto_frame(0).play().end().b),
				show_frame(), end_tag() })), place_object2(at(1, 2, 0, std::string("p"))), show_frame(), end_tag() }));
		const SwfLibrary d = SwfLibrary::from_memory({ { "ui/assets/ping", ping } });
		Result<SwfScene> pinged = SwfScene::open(d, "ping");
		REQUIRE(pinged.ok());
		CHECK(pinged->stats().jumps_capped == 1);
		const auto t0 = Clock::now();
		REQUIRE(pinged->advance(30).ok());
		CHECK(pinged->stats().jumps_capped == 31);
		const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
		MESSAGE("ping-pong gotos: 30 ticks in " << secs << " s");
	}

	TEST_CASE("swf text: HTML subset, plain text, UTF-8") {
		const SwfTextFormat base = synth_format();
		const SwfRichText r = html("<p align=\"center\"><font face=\"$Synth\" size=\"24\" color=\"#ff0010\" letterSpacing=\"1.5\" kerning=\"1\">A&amp;B</font></p>"
								   "<p><b>x</b><br>&lt;&#65;&#x42;&gt;</p><blink>q</blink>",
				base);
		CHECK(r.text == U"A&B\rx\r<AB>\rq");
		CHECK(r.unknown_tags == 2);
		CHECK(r.unknown_entities == 0);
		const SwfTextFormat &a = r.format_at(0, base);
		CHECK(a.align == swf_align::kCenter);
		CHECK(a.size == 24.0f);
		CHECK(a.color == SwfColor{ 255, 0, 16, 255 });
		CHECK(a.letter_spacing == 1.5f);
		CHECK(a.kerning);
		CHECK(r.format_at(4, base).bold);
		CHECK(r.format_at(4, base).size == 20.0f);
		CHECK(r.format_at(4, base).align == swf_align::kLeft);
		CHECK(html("<font size=\"+4\">a</font><font size=\"-30\">b</font>", base).format_at(0, base).size == 24.0f);
		CHECK(html("<font size=\"-30\">b</font>", base).format_at(0, base).size == 0.0f);
		CHECK(html("&bogus; &#xZZ;", base).unknown_entities == 2);
		CHECK(html("a <unterminated", base).text == U"a <unterminated");
		CHECK(html("<textformat leading=\"3\" leftmargin=\"4\" indent=\"5\">t</textformat>", base).format_at(0, base).leading == 3.0f);
		std::string deep;
		for (int i = 0; i < 70; ++i) {
			deep += "<b>";
		}
		WorkBudget b(kSwfTextBudgetUnits);
		CHECK_FALSE(swf_parse_html(deep + "x", base, b).ok());
		CHECK(plain("a\r\nb\nc", base).text == U"a\rb\rc");
		CHECK(swf_utf8_decode("\xc3\xa9\xff\xe2\x82\xac") == std::u32string{ char32_t(0xe9), char32_t(0xfffd), char32_t(0x20ac) });
		CHECK(swf_utf8_decode("\xc0\xaf") == std::u32string{ char32_t(0xfffd), char32_t(0xfffd) });
	}

	TEST_CASE("swf text: layout with DefineFont3 advances and kerning, align, wrap, leading, autoSize, shrink and fit") {
		const SwfFontSet fonts = synth_fonts();
		REQUIRE(fonts.find("$Synth") != nullptr);
		REQUIRE(fonts.find("Synth Sans Medium") != nullptr);
		CHECK(fonts.find("$Nope") == nullptr);
		const SwfTextFormat base = synth_format();

		SwfTextLayout l = lay(fonts, synth_field(104, 30), plain("000", base));
		REQUIRE(l.glyphs.size() == 3);
		CHECK(near(l.glyphs[0].x, 2.0f, 1e-4f));
		CHECK(near(l.glyphs[1].x, 12.0f, 1e-4f));
		CHECK(near(l.glyphs[2].x, 22.0f, 1e-4f));
		CHECK(near(l.glyphs[0].baseline, 18.0f, 1e-4f));
		CHECK(near(l.text_width, 30.0f, 1e-4f));
		CHECK(near(l.text_height, 20.0f, 1e-4f));
		CHECK(l.missing_glyphs == 0);

		SwfTextFormat right = base;
		right.align = swf_align::kRight;
		CHECK(near(lay(fonts, synth_field(104, 30), plain("000", right)).glyphs[0].x, 72.0f, 1e-4f));
		SwfTextFormat centre = base;
		centre.align = swf_align::kCenter;
		CHECK(near(lay(fonts, synth_field(104, 30), plain("000", centre)).glyphs[0].x, 37.0f, 1e-4f));

		const SwfTextLayout kerned = lay(fonts, synth_field(104, 30), html("<font face=\"$Synth\" kerning=\"1\">AV</font>", base));
		CHECK(near(kerned.glyphs[1].x, 12.0f, 1e-4f));
		const SwfTextLayout unkerned = lay(fonts, synth_field(104, 30), html("<font face=\"$Synth\">AV</font>", base));
		CHECK(near(unkerned.glyphs[1].x, 14.0f, 1e-4f));
		const SwfTextLayout spaced = lay(fonts, synth_field(104, 30), html("<font letterSpacing=\"3\">00</font>", base));
		CHECK(near(spaced.glyphs[1].x, 15.0f, 1e-4f));

		SwfTextFormat lead = base;
		lead.leading = 4;
		const SwfTextLayout wrapped = lay(fonts, synth_field(40, 30, SwfEditText::kWordWrap | SwfEditText::kMultiline), plain("000 000 000", lead));
		REQUIRE(wrapped.lines.size() == 3);
		CHECK(wrapped.glyphs.size() == 9);
		CHECK(near(wrapped.lines[1].top, 26.0f, 1e-4f));
		CHECK(near(wrapped.lines[0].width, 30.0f, 1e-4f));
		CHECK(near(wrapped.text_height, 68.0f, 1e-4f));
		const SwfTextLayout breaks = lay(fonts, synth_field(104, 30), plain("0\r\r0", base));
		CHECK(breaks.lines.size() == 3);
		const SwfTextLayout long_word = lay(fonts, synth_field(20, 30, SwfEditText::kWordWrap), plain("0000", base));
		CHECK(long_word.lines.size() == 4);

		CHECK(swf_edit_text_options(synth_field(10, 10)).auto_size == SwfTextAutoSize::None);
		SwfEditText auto_field = synth_field(10, 10, SwfEditText::kAutoSize);
		CHECK(swf_edit_text_options(auto_field).auto_size == SwfTextAutoSize::Left);
		auto_field.align = swf_align::kRight;
		CHECK(swf_edit_text_options(auto_field).auto_size == SwfTextAutoSize::Right);
		auto_field.align = swf_align::kCenter;
		CHECK(swf_edit_text_options(auto_field).auto_size == SwfTextAutoSize::Center);
		auto_field.align = swf_align::kJustify;
		CHECK(swf_edit_text_options(auto_field).auto_size == SwfTextAutoSize::Left);
		CHECK(swf_edit_text_options(auto_field).fit == SwfTextFit::None);

		SwfTextOptions o;
		o.auto_size = SwfTextAutoSize::Left;
		SwfTextLayout sized = lay(fonts, synth_field(104, 30), plain("000", base), o);
		CHECK(sized.bounds == SwfRect{ 0, 34, 0, 24 });
		o.auto_size = SwfTextAutoSize::Center;
		sized = lay(fonts, synth_field(104, 30), plain("000", base), o);
		CHECK(sized.bounds == SwfRect{ 35, 69, 0, 24 });
		CHECK(near(sized.glyphs[0].x, 37.0f, 1e-4f));
		o.auto_size = SwfTextAutoSize::Right;
		sized = lay(fonts, synth_field(104, 30), plain("000", base), o);
		CHECK(sized.bounds == SwfRect{ 70, 104, 0, 24 });

		SwfTextOptions shrink;
		shrink.fit = SwfTextFit::Shrink;
		const SwfTextLayout shrunk = lay(fonts, synth_field(24, 30), plain("000", base), shrink);
		CHECK(near(shrunk.scale, 2.0f / 3.0f, 1e-3f));
		CHECK(shrunk.text_width <= 20.0f + 1e-3f);
		CHECK(near(lay(fonts, synth_field(104, 30), plain("000", base), shrink).scale, 1.0f, 1e-6f));
		SwfTextOptions fit;
		fit.fit = SwfTextFit::Fit;
		CHECK(near(lay(fonts, synth_field(104, 30), plain("000", base), fit).scale, 1.3f, 1e-3f));

		CHECK(lay(fonts, synth_field(104, 30), plain("0Z0", base)).missing_glyphs == 1);
		SwfTextFormat missing = base;
		missing.font = "$Nope";
		CHECK(lay(fonts, synth_field(104, 30), plain("00", missing)).missing_fonts == 2);
		SwfTextFormat just = base;
		just.align = swf_align::kJustify;
		const SwfTextLayout justified = lay(fonts, synth_field(50, 60, SwfEditText::kWordWrap), plain("0 0 0 000", just));
		REQUIRE(justified.lines.size() == 2);
		CHECK(near(justified.lines[0].width, 46.0f, 1e-4f));
		CHECK(near(justified.glyphs[2].x, 20.0f, 1e-4f));
		CHECK(near(justified.glyphs[4].x, 38.0f, 1e-4f));
		CHECK(near(justified.lines[1].width, 30.0f, 1e-4f));
	}

	TEST_CASE("swf scene and text: seeded mutants of the scene movie and of HTML strings fail cleanly or build") {
		std::mt19937_64 rng(0x5ce4e);
		const Bytes base_movie = scene_movie();
		const Bytes widget = widget_movie();
		const SwfFontSet fonts = synth_fonts();
		size_t built = 0;
		size_t refused = 0;
		const auto t0 = Clock::now();
		const size_t iters = fuzz_iters(400);
		for (size_t i = 0; i < iters; ++i) {
			Bytes b = base_movie;
			mutate(b, rng);
			const SwfLibrary lib = SwfLibrary::from_memory({ { "ui/assets/synth", b }, { "ui/assets/widget", widget } });
			Result<SwfScene> s = SwfScene::open(lib, "synth");
			if (!s) {
				++refused;
				CHECK(!s.error().message.empty());
				continue;
			}
			++built;
			Status st = s->advance(6);
			CHECK((st.ok() || !st.error().message.empty()));
			const std::vector<SwfSceneChange> ch = s->take_changes();
			for (const SwfNodeId id : s->draw_order()) {
				const SwfSceneNode *n = s->node(id);
				REQUIRE(n != nullptr);
				CHECK((id == 0 || s->node(n->parent) != nullptr));
				if (n->kind == SwfNodeKind::EditText) {
					WorkBudget tb(kSwfTextBudgetUnits);
					(void)swf_layout_node_text(fonts, *n, tb);
				}
			}
			CHECK(ch.size() <= s->stats().created);
		}
		size_t html_ok = 0;
		const std::string pieces[] = { "<p align=\"center\">", "</p>", "<font face=\"$Synth\" size=\"", "30\">", "</font>", "<br>", "&amp;", "&#", "<", ">", "\"", "0",
			"A", " ", "\xc3", "\xa9", "<textformat leading=\"", "-5\">", "<b>", "</b>" };
		for (size_t i = 0; i < iters; ++i) {
			std::string h;
			const size_t n = rng() % 24;
			for (size_t k = 0; k < n; ++k) {
				h += pieces[rng() % std::size(pieces)];
			}
			WorkBudget hb(kSwfTextBudgetUnits);
			Result<SwfRichText> r = swf_parse_html(h, synth_format(), hb);
			if (!r) {
				continue;
			}
			++html_ok;
			Result<SwfTextLayout> l = swf_layout_text(fonts, synth_field(60, 40, SwfEditText::kWordWrap), *r, { SwfTextAutoSize::Center, SwfTextFit::Shrink }, hb);
			CHECK((l.ok() || !l.error().message.empty()));
		}
		const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
		MESSAGE(iters << " scene mutants: " << built << " built, " << refused << " refused; " << html_ok << " html strings laid out; " << secs << " s");
		CHECK(built + refused == iters);
	}
}

namespace {

const SwfSceneNode *need(const SwfScene &p_scene, std::string_view p_path) {
	const std::string path_name(p_path);
	CAPTURE(path_name);
	const SwfSceneNode *n = p_scene.find_node(p_path);
	REQUIRE(n != nullptr);
	return n;
}

void check_world(const SwfScene &p_scene, std::string_view p_path, float p_x, float p_y) {
	const SwfSceneNode *n = need(p_scene, p_path);
	const std::string path_name(p_path);
	CAPTURE(path_name);
	CHECK(near(n->world.tx, p_x, 0.05f));
	CHECK(near(n->world.ty, p_y, 0.05f));
}

void check_text(const SwfScene &p_scene, std::string_view p_path, std::string_view p_font, float p_size, SwfColor p_color, uint8_t p_align,
		std::optional<std::string_view> p_text = std::nullopt) {
	const SwfSceneNode *n = need(p_scene, p_path);
	const std::string path_name(p_path);
	CAPTURE(path_name);
	REQUIRE(n->kind == SwfNodeKind::EditText);
	const SwfTextFormat f = swf_edit_text_format(*n->movie, *n->edit_text);
	CHECK(f.font == p_font);
	CHECK(f.size == p_size);
	CHECK(f.color == p_color);
	CHECK(f.align == p_align);
	if (p_text) {
		CHECK(n->text == *p_text);
	}
}

void check_against_timeline(const SwfScene &p_scene, const SwfSceneNode &p_node) {
	std::vector<std::vector<SwfDisplayEntry>> lists;
	const uint32_t frame = p_node.frame;
	WorkBudget b(1u << 20);
	REQUIRE(display_lists_at(*p_node.timeline, std::span<const uint32_t>(&frame, 1), b, lists).ok());
	std::vector<const SwfSceneNode *> timeline_children;
	for (const SwfNodeId c : p_node.children) {
		if (!p_scene.node(c)->dynamic) {
			timeline_children.push_back(p_scene.node(c));
		}
	}
	REQUIRE(timeline_children.size() == lists[0].size());
	for (size_t i = 0; i < lists[0].size(); ++i) {
		const SwfDisplayEntry &e = lists[0][i];
		const SwfSceneNode &c = *timeline_children[i];
		CHECK(c.depth == e.depth);
		CHECK(c.placed_character == e.character);
		CHECK(c.matrix == e.matrix);
		CHECK(c.color == swf_color_xform(e.color));
		CHECK(c.name == e.name);
	}
}

}

TEST_SUITE("ui-scene") {
	TEST_CASE("swf scene: the retail HUD trees, section 4 positions, text fields, the bgAmmo intro and hudmessage's Kill row (install)") {
		bf3::test::Mounted fx;
		if (!bf3::test::open_or_skip(fx)) {
			return;
		}
		const auto t0 = Clock::now();
		Result<SwfLibrary> lib = SwfLibrary::open(fx.install);
		REQUIRE(lib.ok());

		struct Tree {
			const char *movie;
			uint32_t nodes;
			uint32_t scripts;
		};
		const Tree trees[] = { { "hudmpscreen", 417, 101 }, { "hudtdmscreen", 141, 52 }, { "hudscreen", 234, 61 }, { "crosshairdefault", 18, 4 } };
		std::map<std::string, SwfScene> scenes;
		for (const Tree &t : trees) {
			const std::string movie_name = t.movie;
			CAPTURE(movie_name);
			Result<SwfScene> s = SwfScene::open(*lib, t.movie);
			REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
			const std::vector<SwfSceneChange> ch = s->take_changes();
			CHECK(s->stats().live_nodes == t.nodes);
			CHECK(ch.size() == t.nodes);
			CHECK(s->draw_order().size() == t.nodes);
			CHECK(s->stats().unresolved == 0);
			CHECK(s->stats().scripts_run == t.scripts);
			CHECK(s->stats().ops_conditional_skipped == 0);
			CHECK(s->frame_rate() > 0.0f);
			scenes.emplace(t.movie, std::move(*s));
		}
		SwfScene &mp = scenes.at("hudmpscreen");
		SwfScene &tdm = scenes.at("hudtdmscreen");
		SwfScene &hud = scenes.at("hudscreen");
		SwfScene &cross = scenes.at("crosshairdefault");

		check_world(mp, "instance1.Ammo_01", 1194, 669);
		check_world(mp, "instance1.Health_01", 1193, 605);
		check_world(mp, "instance1.HudMessage_02", 1280, 0);
		check_world(mp, "instance1.ScoreAggregator_01", 773, 486);
		check_world(mp, "instance1.HudAlertMessage_01", 640, 156);
		check_world(mp, "instance1.HudAlertMessage_02", 640, 156);
		check_world(tdm, "instance1.HudBackgroundWidget_01", -3, 486.95f);
		CHECK(near(need(tdm, "instance1.HudBackgroundWidget_01")->world.a, 1.28787f, 0.0006f));
		CHECK(near(need(tdm, "instance1.HudBackgroundWidget_01")->world.d, 3.68855f, 0.0006f));
		check_world(tdm, "instance1.Compass_01", 85.05f, 696);
		check_world(tdm, "instance1.Minimap_01", -3, 527);
		check_world(tdm, "instance1.TicketCounter_01", -4, 530);
		check_world(tdm, "instance1.TicketCounter_01.tickets0", -4, 530 - 27);
		check_world(tdm, "instance1.TicketCounter_01.tickets1", -4, 530 - 46);
		check_world(tdm, "instance1.SquadList_00", 179, 619);
		check_world(hud, "instance1.LogMessage_01", 0, 534);
		check_world(cross, "instance1.Crosshair_01", 640, 360);
		check_world(cross, "instance1.Crosshair_01.crosshair.left", 640 - 11, 357);
		check_world(cross, "instance1.Crosshair_01.crosshair.right", 640 + 11, 357);
		check_world(cross, "instance1.Crosshair_01.crosshair.up", 637, 360 - 11);
		check_world(cross, "instance1.Crosshair_01.crosshair.down", 637, 360 + 11);

		for (const char *screen : { "hudmpscreen", "hudtdmscreen", "hudscreen" }) {
			WorkBudget hb(kSwfLayoutBudgetUnits);
			Result<std::vector<HudPlacement>> rows = read_hud_placements(*lib, screen, hb);
			REQUIRE(rows.ok());
			REQUIRE(!rows->empty());
			for (const HudPlacement &p : *rows) {
				const SwfSceneNode *n = need(scenes.at(screen), "instance1." + p.instance);
				CHECK(near(n->world.tx, p.matrix.tx, 1e-3f));
				CHECK(near(n->world.ty, p.matrix.ty, 1e-3f));
				CHECK(near(n->world.a, p.matrix.a, 1e-5f));
				CHECK(near(n->world.d, p.matrix.d, 1e-5f));
				CHECK(n->movie_name == p.widget_movie);
			}
		}

		const SwfColor ticket{ 0xae, 0xea, 0xf8, 255 };
		check_text(tdm, "instance1.TicketCounter_01.tickets0.tickets", "$HUD_medium", 16, ticket, swf_align::kRight, "0");
		check_text(tdm, "instance1.TicketCounter_01.tickets0.team", "$Menu_medium", 16, ticket, swf_align::kLeft);
		check_text(tdm, "instance1.SquadList_00.squadMember0.label.txt", "$Menu_medium", 16, SwfColor{ 0xe4, 0xff, 0x9d, 255 }, swf_align::kLeft);
		check_text(mp, "instance1.ScoreAggregator_01.aggHolder.scoringMessage.txt", "$Menu_bold", 24, SwfColor{ 255, 255, 255, 255 }, swf_align::kCenter,
				"100");
		CHECK(need(tdm, "instance1.TicketCounter_01.tickets0.tickets")->filters.size() == 1);
		CHECK(need(tdm, "instance1.TicketCounter_01.tickets0.tickets")->filters[0].kind == SwfFilterKind::DropShadow);

		const SwfSceneNode *bg = need(mp, "instance1.Ammo_01.Ammo.bgAmmo");
		REQUIRE(bg->timeline != nullptr);
		CHECK(bg->symbol == "HudBackgroundWidget");
		CHECK(bg->total_frames() == 41);
		CHECK(bg->timeline->label_frame("onExit") == std::optional<uint32_t>(21));
		uint32_t first_stop = 0;
		for (const SwfFrameScript &sc : bg->timeline->scripts) {
			const bool stops = std::any_of(sc.ops.begin(), sc.ops.end(), [](const SwfFrameOp &op) { return op.kind == SwfFrameOpKind::Stop && !op.after_branch; });
			if (stops && first_stop == 0 && sc.frame > 0) {
				first_stop = sc.frame;
			}
		}
		CHECK(first_stop == 20);
		const SwfNodeId bg_id = bg->id;
		CHECK(bg->frame == 0);
		CHECK(bg->playing);
		check_against_timeline(mp, *bg);
		uint32_t moving = 0;
		for (uint32_t step = 1; step <= 30; ++step) {
			REQUIRE(mp.advance().ok());
			const std::vector<SwfSceneChange> ch = mp.take_changes();
			const SwfSceneNode *n = mp.node(bg_id);
			REQUIRE(n != nullptr);
			CHECK(n->frame == std::min(step, first_stop));
			CHECK(n->playing == (step < first_stop));
			const bool frame_changed = (change_of(ch, bg_id) & swf_change::kFrame) != 0;
			CHECK(frame_changed == (step <= first_stop));
			moving += frame_changed ? 1 : 0;
			check_against_timeline(mp, *n);
		}
		CHECK(moving == first_stop);
		const SwfSceneNode *background = need(mp, "instance1.Ammo_01.Ammo.bgAmmo.Background");
		CHECK(background->world_color.mul[3] == 1.0f);

		check_text(mp, "instance1.Ammo_01.Ammo.clip.cltxt.txtDisplay", "$HUD_medium", 43, SwfColor{ 255, 255, 255, 255 }, swf_align::kRight, "000");
		check_text(mp, "instance1.Health_01.Health.healthHolder.amtxt.txtDisplay", "$HUD_medium", 21, SwfColor{ 255, 255, 255, 255 }, swf_align::kRight, "100");

		Result<SwfNodeId> kill = mp.attach_movie("instance1.HudMessage_02", "Kill", "kill0", 1);
		REQUIRE_MESSAGE(kill.ok(), (kill.ok() ? std::string() : kill.error().message));
		const SwfSceneNode *k = mp.node(*kill);
		CHECK(k->symbol == "Kill");
		CHECK(k->movie_name == "ui/assets/hudmessage");
		CHECK(k->path == "instance1.HudMessage_02.kill0");
		CHECK(k->as2_depth() == 1);
		CHECK(k->total_frames() == 15);
		CHECK(k->timeline->label_frame("startAnim") == std::optional<uint32_t>(1));
		CHECK(k->timeline->label_frame("outAnim") == std::optional<uint32_t>(9));
		std::vector<SwfSceneChange> ch = mp.take_changes();
		CHECK(change_of(ch, *kill) == swf_change::kCreated);
		check_world(mp, "instance1.HudMessage_02.kill0", 1280, 0);
		const SwfColor kill_colour{ 0xff, 0xea, 0xb9, 255 };
		for (const char *part : { "killer", "msg", "victim" }) {
			check_text(mp, std::string("instance1.HudMessage_02.kill0.container.") + part + ".txt", "$Menu_bold", 20, kill_colour, swf_align::kLeft, "enemy");
		}
		check_world(mp, "instance1.HudMessage_02.kill0.container.killer", 1280, 0);
		check_world(mp, "instance1.HudMessage_02.kill0.container.msg", 1350, 0);
		check_world(mp, "instance1.HudMessage_02.kill0.container.victim", 1420, 0);
		REQUIRE(mp.set_text("instance1.HudMessage_02.kill0.container.killer.txt", "SOLDIER").ok());
		for (int i = 0; i < 10; ++i) {
			REQUIRE(mp.advance().ok());
		}
		CHECK(mp.node(*kill)->frame == 4);
		CHECK_FALSE(mp.node(*kill)->playing);
		ch = mp.take_changes();
		CHECK((change_of(ch, mp.find("instance1.HudMessage_02.kill0.container.killer.txt")) & swf_change::kText) != 0);
		REQUIRE(mp.goto_label("instance1.HudMessage_02.kill0", "outAnim", true).ok());
		CHECK(mp.node(*kill)->frame == 9);
		for (int i = 0; i < 10; ++i) {
			REQUIRE(mp.advance().ok());
		}
		REQUIRE(k->timeline->script_at(14) != nullptr);
		REQUIRE(k->timeline->script_at(14)->ops.size() == 1);
		CHECK(k->timeline->script_at(14)->ops[0].kind == SwfFrameOpKind::GotoLabel);
		CHECK(k->timeline->script_at(14)->ops[0].label == "start");
		CHECK(mp.node(*kill)->frame == 0);
		CHECK_FALSE(mp.node(*kill)->playing);
		REQUIRE(mp.remove_movie("instance1.HudMessage_02.kill0").ok());
		ch = mp.take_changes();
		CHECK(change_of(ch, *kill) == swf_change::kRemoved);

		Result<std::unique_ptr<EbxLibrary>> ui = EbxLibrary::open(fx.install, { "Win32/UI" });
		REQUIRE(ui.ok());
		Result<FontMappingCollection> mapping = font_mapping_for_text_database(**ui, "localization/us_loc");
		REQUIRE(mapping.ok());
		WorkBudget fb(kUiFontConvertBudgetUnits);
		Result<SwfFontSet> fonts = SwfFontSet::load(*lib, *mapping, fb);
		REQUIRE_MESSAGE(fonts.ok(), (fonts.ok() ? std::string() : fonts.error().message));
		CHECK(fonts->faces().size() == 4);

		const SwfSceneNode *clip = need(mp, "instance1.Ammo_01.Ammo.clip.cltxt.txtDisplay");
		WorkBudget tb(kSwfTextBudgetUnits);
		Result<SwfTextLayout> ammo = swf_layout_node_text(*fonts, *clip, tb);
		REQUIRE(ammo.ok());
		const SwfFontFace *hud_face = fonts->find("$HUD_medium");
		REQUIRE(hud_face != nullptr);
		const std::optional<uint16_t> zero = hud_face->glyph(U'0');
		REQUIRE(zero.has_value());
		const float zero_px = float(hud_face->outlines.advances[*zero]) * 43.0f / float(kSwfFontUnitsPerEm);
		CHECK(ammo->glyphs.size() == 3);
		CHECK(ammo->missing_glyphs == 0);
		CHECK(ammo->missing_fonts == 0);
		CHECK(near(ammo->text_width, 3.0f * zero_px, 1e-3f));
		CHECK(near(ammo->glyphs[2].x + ammo->glyphs[2].advance, clip->edit_text->bounds.x_max - kSwfTextGutter, 1e-3f));
		CHECK(near(ammo->text_height, float(hud_face->outlines.ascent + hud_face->outlines.descent) * 43.0f / float(kSwfFontUnitsPerEm), 1e-3f));
		MESSAGE("ammo clip '000' at 43 px: textWidth " << ammo->text_width << ", textHeight " << ammo->text_height);

		uint32_t fields = 0;
		uint32_t missing_glyphs = 0;
		uint32_t missing_fonts = 0;
		for (auto &[name, scene] : scenes) {
			for (const SwfNodeId id : scene.draw_order()) {
				const SwfSceneNode *n = scene.node(id);
				if (n->kind != SwfNodeKind::EditText) {
					continue;
				}
				++fields;
				CHECK(n->text_options.auto_size == swf_edit_text_options(*n->edit_text).auto_size);
				Result<SwfTextLayout> l = swf_layout_node_text(*fonts, *n, tb);
				REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
				missing_glyphs += l->missing_glyphs;
				missing_fonts += l->missing_fonts;
			}
		}
		MESSAGE(fields << " text fields in the four trees: " << missing_glyphs << " missing glyphs, " << missing_fonts << " characters without a font");
		uint32_t bitmap_meshes = 0;
		uint32_t unresolved_bitmaps = 0;
		uint32_t outside_atlas = 0;
		std::map<std::string, uint32_t> textures;
		for (auto &[name, scene] : scenes) {
			for (const SwfNodeId id : scene.draw_order()) {
				const SwfSceneNode *n = scene.node(id);
				if (n->shape == nullptr) {
					continue;
				}
				for (const SwfFillMesh &mesh : n->shape->meshes) {
					const SwfFillStyle &fill = n->shape->fills[mesh.fill];
					if (fill.kind != SwfFillKind::Bitmap) {
						continue;
					}
					++bitmap_meshes;
					const std::optional<SwfBitmapRef> ref = scene.bitmap(*n, fill.bitmap_id);
					if (!ref) {
						++unresolved_bitmaps;
						continue;
					}
					textures[ref->texture_res]++;
					outside_atlas += (ref->x2 > ref->atlas_width || ref->y2 > ref->atlas_height || ref->x1 > ref->x2 || ref->y1 > ref->y2) ? 1 : 0;
				}
			}
		}
		std::string texture_list;
		for (const auto &[t, count] : textures) {
			texture_list += " " + t + " " + std::to_string(count);
		}
		MESSAGE(bitmap_meshes << " bitmap meshes in the four trees over " << textures.size() << " atlas textures:" << texture_list);
		CHECK(bitmap_meshes == 209);
		CHECK(textures.size() == 12);
		CHECK(unresolved_bitmaps == 0);
		CHECK(outside_atlas == 0);
		CHECK(missing_fonts == 0);
		CHECK(missing_glyphs == 0);

		uint32_t html_fields = 0;
		uint32_t unknown_tags = 0;
		uint32_t unknown_entities = 0;
		uint32_t html_missing_glyphs = 0;
		uint32_t html_missing_fonts = 0;
		uint32_t kerning_runs = 0;
		std::map<SwfTextAutoSize, uint32_t> library_auto_sizes;
		for (const SwfMovieSource &src : lib->movies()) {
			Result<std::shared_ptr<const SwfMovie>> m = lib->load(src.name);
			REQUIRE(m.ok());
			for (const SwfEditText &t : (*m)->edit_texts) {
				library_auto_sizes[swf_edit_text_options(t).auto_size]++;
				if (!t.has(SwfEditText::kHtml) || t.initial_text.empty()) {
					continue;
				}
				++html_fields;
				WorkBudget hb(kSwfTextBudgetUnits);
				Result<SwfRichText> r = swf_parse_html(t.initial_text, swf_edit_text_format(**m, t), hb);
				REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
				unknown_tags += r->unknown_tags;
				unknown_entities += r->unknown_entities;
				for (const SwfTextRun &run : r->runs) {
					kerning_runs += run.format.kerning ? 1 : 0;
				}
				Result<SwfTextLayout> l = swf_layout_text(*fonts, t, *r, {}, hb);
				REQUIRE(l.ok());
				html_missing_glyphs += l->missing_glyphs;
				html_missing_fonts += l->missing_fonts;
			}
		}
		MESSAGE(html_fields << " html fields: " << unknown_tags << " unknown tags or attributes, " << unknown_entities << " unknown entities, " << kerning_runs
								<< " kerned runs, " << html_missing_glyphs << " characters missing a glyph, " << html_missing_fonts
								<< " characters in Arial or Verdana, which no font mapping names");
		MESSAGE("DefineEditText autoSize over the library: none " << library_auto_sizes[SwfTextAutoSize::None] << ", left "
																  << library_auto_sizes[SwfTextAutoSize::Left] << ", center " << library_auto_sizes[SwfTextAutoSize::Center]
																  << ", right " << library_auto_sizes[SwfTextAutoSize::Right]);
		CHECK(library_auto_sizes[SwfTextAutoSize::None] == 1488);
		CHECK(library_auto_sizes[SwfTextAutoSize::Left] + library_auto_sizes[SwfTextAutoSize::Center] + library_auto_sizes[SwfTextAutoSize::Right] == 0);
		CHECK(html_fields == 79);
		CHECK(unknown_tags == 0);
		CHECK(unknown_entities == 0);
		CHECK(kerning_runs == 71);
		CHECK(html_missing_glyphs == 20);
		CHECK(html_missing_fonts == 105);
		Result<SwfScene> long_run = SwfScene::open(*lib, "hudmpscreen");
		REQUIRE(long_run.ok());
		REQUIRE(long_run->advance(3000).ok());
		MESSAGE("hudmpscreen after 3,000 frames: " << long_run->stats().created << " nodes created, " << long_run->stats().live_nodes << " live, "
												   << long_run->stats().node_slots << " node slots");
		CHECK(long_run->stats().created == 3849);
		CHECK(long_run->stats().live_nodes == 441);
		CHECK(long_run->stats().node_slots == 441);

		const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
		MESSAGE("ui-scene install case: " << secs << " s");
		const SnapshotDiff diff = diff_snapshots(fx.before, fx.reader.snapshot());
		MESSAGE("install.unchanged: " << fx.before.files.size() << " index files, " << (diff.clean() ? "clean" : diff.summary()));
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}

#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/as2_clip_loader.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_scene.h"
#include "test_support.h"
#include "ui_swf_writer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui1test;

namespace {

constexpr uint16_t kSquare = 1;
constexpr uint16_t kBox = 2;

Bytes square_shape(uint16_t p_id, int32_t p_side) {
	ShapeFill f;
	f.r = 200;
	f.g = 100;
	f.b = 50;
	ShapePath path;
	path.fill1 = 1;
	const int32_t side = p_side * 20;
	path.segs = { { side, 0 }, { side, side }, { 0, side }, { 0, 0 } };
	return define_shape_paths(3, p_id, float(p_side), float(p_side), { f }, {}, { path });
}

Bytes place(uint16_t p_depth, uint16_t p_char, std::string p_name, float p_x, float p_y) {
	Place p;
	p.depth = p_depth;
	p.character = p_char;
	p.matrix = Mat{ 1, 0, 0, 1, p_x, p_y };
	p.name = std::move(p_name);
	return place_object2(p);
}

SwfLibrary two_boxes_library() {
	const Bytes box = define_sprite(kBox, 1, cat({ place(1, kSquare, "fill", 0, 0), show_frame(), end_tag() }));
	const Bytes tags = cat(
			{ square_shape(kSquare, 40), box, export_assets({ { kBox, "Box" } }), place(1, kBox, "content", 100, 100),
					place(2, kBox, "maskclip", 120, 110), place(3, kBox, "other", 300, 100), show_frame(), end_tag() });
	return SwfLibrary::from_memory({ { "ui/assets/boxes", movie(tags) } });
}

SwfScene open_scene(const SwfLibrary &p_lib, std::string_view p_movie) {
	Result<SwfScene> s = SwfScene::open(p_lib, p_movie);
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	return std::move(*s);
}

const SwfSceneNode &need(const SwfScene &p_scene, std::string_view p_path) {
	const SwfSceneNode *n = p_scene.find_node(p_path);
	REQUIRE_MESSAGE(n != nullptr, std::string(p_path));
	return *n;
}

uint32_t change_of(const std::vector<SwfSceneChange> &p_changes, SwfNodeId p_id) {
	uint32_t what = 0;
	for (const SwfSceneChange &c : p_changes) {
		what |= c.node == p_id ? c.what : 0;
	}
	return what;
}

double filled_area(const SwfShape &p_shape) {
	double area = 0.0;
	for (const SwfFillMesh &m : p_shape.meshes) {
		for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
			const SwfPoint a = m.vertices[m.indices[i]];
			const SwfPoint b = m.vertices[m.indices[i + 1]];
			const SwfPoint c = m.vertices[m.indices[i + 2]];
			area += std::abs(double(b.x - a.x) * double(c.y - a.y) - double(c.x - a.x) * double(b.y - a.y)) * 0.5;
		}
	}
	return area;
}

std::shared_ptr<const SwfShape> drawn(const std::vector<SwfDrawOp> &p_ops) {
	SwfDrawing d;
	for (const SwfDrawOp &op : p_ops) {
		REQUIRE(d.apply(op).ok());
	}
	Result<std::shared_ptr<const SwfShape>> shape = d.shape();
	REQUIRE_MESSAGE(shape.ok(), (shape.ok() ? std::string() : shape.error().message));
	return *shape;
}

class FakeImages final : public SwfImageSource {
public:
	SwfImageInfo request(std::string_view p_res_name) override {
		++requests;
		SwfImageInfo info;
		if (p_res_name == "ui/art/missing") {
			info.status = SwfImageStatus::Missing;
			info.error = "no such res";
		} else if (requests > loading_polls) {
			info.status = SwfImageStatus::Ready;
			info.width = 64;
			info.height = 32;
		}
		return info;
	}
	uint32_t loading_polls = 2;
	uint32_t requests = 0;
};

std::vector<SwfDrawOp> square_ops(float p_x, float p_y, float p_side) {
	return { SwfDrawOp::move_to(p_x, p_y), SwfDrawOp::line_to(p_x + p_side, p_y), SwfDrawOp::line_to(p_x + p_side, p_y + p_side),
		SwfDrawOp::line_to(p_x, p_y + p_side), SwfDrawOp::line_to(p_x, p_y) };
}

}

TEST_SUITE("ui-scene2-unit") {
	TEST_CASE("swf scene: setMask masks a clip and its subtree, moves to a new clip, clears and drops a removed mask") {
		const SwfLibrary lib = two_boxes_library();
		SwfScene s = open_scene(lib, "boxes");
		s.take_changes();
		const SwfNodeId content = need(s, "content").id;
		const SwfNodeId fill = need(s, "content.fill").id;
		const SwfNodeId mask = need(s, "maskclip").id;
		const SwfNodeId other = need(s, "other").id;

		REQUIRE(s.set_mask("content", "maskclip").ok());
		std::vector<SwfSceneChange> ch = s.take_changes();
		CHECK(need(s, "content").script_mask == mask);
		CHECK(need(s, "content").masks == std::vector<SwfNodeId>{ mask });
		CHECK(need(s, "content.fill").masks == std::vector<SwfNodeId>{ mask });
		CHECK(need(s, "maskclip").is_mask);
		CHECK(need(s, "maskclip").masks.empty());
		CHECK(need(s, "other").masks.empty());
		CHECK((change_of(ch, content) & swf_change::kMask) != 0);
		CHECK((change_of(ch, fill) & swf_change::kMask) != 0);
		CHECK((change_of(ch, mask) & swf_change::kMask) != 0);
		CHECK(change_of(ch, other) == 0);

		REQUIRE(s.set_mask("other", "maskclip").ok());
		s.take_changes();
		CHECK(need(s, "content").script_mask == kSwfNoNode);
		CHECK(need(s, "content").masks.empty());
		CHECK(need(s, "other").masks == std::vector<SwfNodeId>{ mask });
		CHECK(need(s, "maskclip").is_mask);

		REQUIRE(s.clear_mask("other").ok());
		ch = s.take_changes();
		CHECK(need(s, "other").masks.empty());
		CHECK_FALSE(need(s, "maskclip").is_mask);
		CHECK((change_of(ch, mask) & swf_change::kMask) != 0);

		As2Clip c(&s, "content");
		Result<SwfNodeId> dyn = s.attach_movie("", "Box", "dynamicMask", 5);
		REQUIRE(dyn.ok());
		c.set_mask(c.parent().child("dynamicMask"));
		s.take_changes();
		CHECK(need(s, "content").masks == std::vector<SwfNodeId>{ *dyn });
		REQUIRE(s.remove_movie("dynamicMask").ok());
		s.take_changes();
		CHECK(need(s, "content").script_mask == kSwfNoNode);
		CHECK(need(s, "content").masks.empty());

		CHECK_FALSE(s.set_mask("content", "content").ok());
		CHECK_FALSE(s.set_mask("content", "").ok());
		CHECK_FALSE(s.set_mask("missing", "maskclip").ok());
		CHECK_FALSE(s.set_mask("content", "missing").ok());
	}

	TEST_CASE("swf draw: fills close their contours, curves flatten, holes are even-odd, an open fill draws, clear resets") {
		const SwfColor red{ 255, 0, 0, 255 };
		std::vector<SwfDrawOp> ops{ SwfDrawOp::begin_fill(red) };
		for (const SwfDrawOp &op : square_ops(0.0f, 0.0f, 10.0f)) {
			ops.push_back(op);
		}
		ops.push_back(SwfDrawOp::end_fill());
		std::shared_ptr<const SwfShape> sq = drawn(ops);
		REQUIRE(sq->meshes.size() == 1);
		CHECK(sq->meshes[0].outline.size() == 4);
		CHECK(sq->fills[0].color == red);
		CHECK(sq->bounds == SwfRect{ 0.0f, 10.0f, 0.0f, 10.0f });
		CHECK(filled_area(*sq) == doctest::Approx(100.0));

		std::vector<SwfDrawOp> holed{ SwfDrawOp::begin_fill(red) };
		for (const SwfDrawOp &op : square_ops(0.0f, 0.0f, 20.0f)) {
			holed.push_back(op);
		}
		for (const SwfDrawOp &op : square_ops(5.0f, 5.0f, 10.0f)) {
			holed.push_back(op);
		}
		holed.push_back(SwfDrawOp::end_fill());
		CHECK(filled_area(*drawn(holed)) == doctest::Approx(300.0));

		const std::shared_ptr<const SwfShape> arch = drawn({ SwfDrawOp::begin_fill(red), SwfDrawOp::move_to(0.0f, 0.0f), SwfDrawOp::curve_to(10.0f, 20.0f, 20.0f, 0.0f),
				SwfDrawOp::line_to(0.0f, 0.0f), SwfDrawOp::end_fill() });
		CHECK(arch->meshes[0].outline.size() > 4);
		CHECK(filled_area(*arch) == doctest::Approx(20.0 * 10.0 * 2.0 / 3.0).epsilon(0.01));

		const std::shared_ptr<const SwfShape> open = drawn({ SwfDrawOp::begin_fill(red), SwfDrawOp::move_to(0.0f, 0.0f), SwfDrawOp::line_to(8.0f, 0.0f),
				SwfDrawOp::line_to(8.0f, 4.0f) });
		CHECK(filled_area(*open) == doctest::Approx(16.0));
		CHECK(drawn({ SwfDrawOp::move_to(0.0f, 0.0f), SwfDrawOp::line_to(8.0f, 0.0f), SwfDrawOp::line_to(8.0f, 4.0f) })->meshes.empty());

		SwfDrawing d;
		for (const SwfDrawOp &op : ops) {
			REQUIRE(d.apply(op).ok());
		}
		CHECK_FALSE(d.empty());
		REQUIRE(d.apply(SwfDrawOp::clear()).ok());
		CHECK(d.empty());
		CHECK(d.commands() == 0);
		for (uint32_t i = 0; i < kSwfDrawMaxCommands; ++i) {
			REQUIRE(d.apply(SwfDrawOp::line_to(float(i), 0.0f)).ok());
		}
		CHECK_FALSE(d.apply(SwfDrawOp::line_to(0.0f, 0.0f)).ok());
		CHECK_FALSE(SwfDrawing{}.apply(SwfDrawOp::line_to(std::nanf(""), 0.0f)).ok());
		CHECK(swf_draw_fill_color(0xff8000u, 100.0) == SwfColor{ 255, 128, 0, 255 });
		CHECK(swf_draw_fill_color(0x000000u, 0.0).a == 0);
	}

	TEST_CASE("swf scene: drawRect into an empty clip draws, sizes the clip, masks, and clears") {
		const SwfLibrary lib = two_boxes_library();
		SwfScene s = open_scene(lib, "boxes");
		s.take_changes();
		As2Clip content(&s, "content");
		const As2Clip mask = content.create_empty_movie_clip("mask", 7);
		REQUIRE(mask.exists());
		mask.begin_fill(0x000000u);
		mask.move_to(0.0f, 0.0f);
		mask.line_to(30.0f, 0.0f);
		mask.line_to(30.0f, 12.0f);
		mask.line_to(0.0f, 12.0f);
		mask.line_to(0.0f, 0.0f);
		mask.end_fill();
		const SwfSceneNode &m = need(s, "content.mask");
		REQUIRE(m.drawing != nullptr);
		CHECK(m.drawing->fills[0].color == SwfColor{ 0, 0, 0, 255 });
		CHECK(mask.width() == doctest::Approx(30.0f));
		CHECK(mask.height() == doctest::Approx(12.0f));
		CHECK(content.width() == doctest::Approx(40.0f));
		std::vector<SwfSceneChange> ch = s.take_changes();
		CHECK((change_of(ch, m.id) & swf_change::kCreated) != 0);
		mask.set_width(18.0f);
		CHECK(mask.width() == doctest::Approx(18.0f));
		CHECK(need(s, "content.mask").matrix.a == doctest::Approx(0.6f));
		REQUIRE(s.set_mask("content.fill", "content.mask").ok());
		s.take_changes();
		CHECK(need(s, "content.fill").masks == std::vector<SwfNodeId>{ m.id });
		mask.clear();
		ch = s.take_changes();
		CHECK(need(s, "content.mask").drawing == nullptr);
		CHECK((change_of(ch, m.id) & swf_change::kCharacter) != 0);
		CHECK(mask.width() == 0.0f);
		CHECK_FALSE(s.draw("content.fill", SwfDrawOp::begin_fill(SwfColor{})).ok());
		CHECK(s.draw("", SwfDrawOp::begin_fill(SwfColor{})).ok());
	}

	TEST_CASE("swf scene: swapDepths swaps two siblings or moves to an empty depth, and a swapped timeline clip leaves its timeline") {
		const SwfLibrary lib = two_boxes_library();
		SwfScene s = open_scene(lib, "boxes");
		As2Clip root(&s, "");
		const As2Clip a = root.attach_movie("Box", "a", 1);
		const As2Clip b = root.attach_movie("Box", "b", 2);
		REQUIRE(a.exists());
		REQUIRE(b.exists());
		const SwfNodeId ia = a.node()->id;
		const SwfNodeId ib = b.node()->id;
		s.take_changes();
		const auto order_of = [&](SwfNodeId p_id) {
			const std::vector<SwfNodeId> order = s.draw_order();
			return std::find(order.begin(), order.end(), p_id) - order.begin();
		};
		CHECK(order_of(ia) < order_of(ib));
		a.swap_depths(b);
		CHECK(a.depth() == 2);
		CHECK(b.depth() == 1);
		CHECK(order_of(ib) < order_of(ia));
		CHECK((change_of(s.take_changes(), 0) & swf_change::kChildren) != 0);
		a.swap_depths(9);
		CHECK(a.depth() == 9);
		CHECK(b.depth() == 1);
		CHECK(order_of(ib) < order_of(ia));
		b.swap_depths(9);
		CHECK(b.depth() == 9);
		CHECK(a.depth() == 1);

		const SwfNodeId content = need(s, "content").id;
		REQUIRE(s.swap_depths("content", "other").ok());
		CHECK(need(s, "content").depth == 3);
		CHECK(need(s, "other").depth == 1);
		CHECK(need(s, "content").dynamic);
		CHECK(need(s, "other").dynamic);
		REQUIRE(s.advance(3).ok());
		CHECK(need(s, "content").id == content);
		CHECK_FALSE(s.swap_depths("content", "content.fill").ok());
		CHECK_FALSE(s.swap_depths("", 4).ok());
		CHECK_FALSE(s.swap_depths("content", kSwfAs2MaxDepth + 1).ok());
	}

	TEST_CASE("as2 clip: _rotation keeps the scales, and Color.setTransform changes only the fields it is given") {
		const SwfLibrary lib = two_boxes_library();
		SwfScene s = open_scene(lib, "boxes");
		const As2Clip c(&s, "content");
		c.set_xscale(200.0f);
		c.set_rotation(8.0f);
		CHECK(c.rotation() == doctest::Approx(8.0f));
		CHECK(c.xscale() == doctest::Approx(200.0f));
		CHECK(c.yscale() == doctest::Approx(100.0f));
		const SwfMatrix &m = need(s, "content").matrix;
		CHECK(m.a == doctest::Approx(2.0 * std::cos(8.0 * std::numbers::pi / 180.0)));
		CHECK(m.b == doctest::Approx(2.0 * std::sin(8.0 * std::numbers::pi / 180.0)));
		CHECK(m.c == doctest::Approx(-std::sin(8.0 * std::numbers::pi / 180.0)));
		CHECK(m.tx == 100.0f);
		c.set_rotation(-5.0f);
		CHECK(c.rotation() == doctest::Approx(-5.0f));
		c.set_rotation(355.0f);
		CHECK(c.rotation() == doctest::Approx(-5.0f));

		As2ColorTransformPatch dim;
		dim.aa = 100.0;
		dim.ra = 30.0;
		dim.ga = 30.0;
		dim.ba = 30.0;
		dim.ab = 0.0;
		dim.rb = 7.0;
		dim.gb = 11.0;
		dim.bb = 11.0;
		c.set_color_transform(dim);
		const SwfColorXform &x = need(s, "content").color;
		CHECK(x.mul == std::array<float, 4>{ 0.3f, 0.3f, 0.3f, 1.0f });
		CHECK(x.add == std::array<float, 4>{ 7.0f, 11.0f, 11.0f, 0.0f });
		As2ColorTransformPatch alpha_only;
		alpha_only.aa = 50.0;
		c.set_color_transform(alpha_only);
		const As2ColorTransform t = c.color_transform();
		CHECK(t.ra == doctest::Approx(30.0));
		CHECK(t.gb == doctest::Approx(11.0));
		CHECK(t.aa == doctest::Approx(50.0));
		CHECK(c.alpha() == doctest::Approx(50.0f));
	}

	TEST_CASE("swf scene: loadClip img:// replaces a clip's content with the image after the source reports it, and unloadClip clears it") {
		CHECK(swf_image_res_name("img://UI/Art/Persistence\\DogTag/inactivetag_left") == "ui/art/persistence/dogtag/inactivetag_left");
		CHECK(swf_image_res_name("UI/Art/x") == "ui/art/x");
		const SwfLibrary lib = two_boxes_library();
		SwfScene s = open_scene(lib, "boxes");
		As2ClipLoader early;
		early.load_clip(As2Clip(&s, "content"), "img://ui/art/box");
		CHECK(early.poll() == As2ClipLoader::State::Failed);
		auto images = std::make_shared<FakeImages>();
		s.set_image_source(images);
		CHECK(s.image_source() == images.get());
		As2Clip content(&s, "content");
		content.set_xscale(50.0f);
		As2ClipLoader loader;
		loader.load_clip(content, "img://UI/Art/Box");
		CHECK(loader.res_name() == "ui/art/box");
		s.take_changes();
		CHECK(loader.poll() == As2ClipLoader::State::Loading);
		CHECK(loader.poll() == As2ClipLoader::State::Loading);
		CHECK(need(s, "content").children.size() == 1);
		CHECK(loader.poll() == As2ClipLoader::State::Loaded);
		const SwfSceneNode &n = need(s, "content");
		CHECK(n.children.empty());
		CHECK(n.image == "ui/art/box");
		CHECK(n.image_width == 64);
		CHECK(n.image_height == 32);
		CHECK_FALSE(n.playing);
		CHECK(content.width() == doctest::Approx(32.0f));
		CHECK(content.height() == doctest::Approx(32.0f));
		std::vector<SwfSceneChange> ch = s.take_changes();
		CHECK((change_of(ch, n.id) & swf_change::kCharacter) != 0);
		CHECK(s.find_node("content.fill") == nullptr);
		REQUIRE(s.advance(2).ok());
		CHECK(need(s, "content").image == "ui/art/box");
		loader.unload_clip();
		CHECK(need(s, "content").image.empty());
		CHECK(loader.state() == As2ClipLoader::State::Idle);
		CHECK(content.width() == 0.0f);

		As2ClipLoader missing;
		missing.load_clip(As2Clip(&s, "other"), "img://ui/art/missing");
		CHECK(missing.poll() == As2ClipLoader::State::Failed);
		CHECK(missing.error() == "no such res");
		CHECK(need(s, "other").image.empty());
		As2ClipLoader into_shape;
		into_shape.load_clip(As2Clip(&s, "other.fill"), "img://ui/art/box");
		CHECK(into_shape.poll() == As2ClipLoader::State::Failed);
		CHECK_FALSE(s.load_image("other", "ui/art/box", 0, 4).ok());
	}
}

#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_scene.h"
#include "test_support.h"
#include "ui_swf_writer.h"

#include <algorithm>
#include <cmath>
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
}

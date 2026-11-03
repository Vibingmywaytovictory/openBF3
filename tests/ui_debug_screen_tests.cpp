#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/debug_screen.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/util/format.h"
#include "bf3/host/net_stats.h"
#include "bf3/net/protocol.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui_retail;

namespace {

constexpr double kFrame = 1.0 / 30.0;
constexpr std::string_view kMovie = "ui/assets/debugscreen";

struct DebugFixture {
	std::unique_ptr<SwfScene> scene;
	std::unique_ptr<HudScreen> hud;

	void step() {
		hud->advance(kFrame);
		const Status st = scene->advance_seconds(kFrame);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	}
	As2Clip clip(std::string_view p_path) { return hud->clip(p_path); }
};

std::unique_ptr<DebugFixture> open_debug_screen(Retail &r_r, const HudScreenAsset &p_asset) {
	auto f = std::make_unique<DebugFixture>();
	Result<SwfScene> scene = SwfScene::open(*r_r.lib, std::string(kMovie));
	REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? std::string() : scene.error().message));
	f->scene = std::make_unique<SwfScene>(std::move(*scene));
	HudStageRects rects;
	rects.original = f->scene->stage();
	rects.visible = f->scene->stage();
	Result<std::unique_ptr<HudScreen>> hud =
			HudScreen::open(*f->scene, *r_r.lib, p_asset, r_r.fonts.get(), HudClientState{}, rects);
	REQUIRE_MESSAGE(hud.ok(), (hud.ok() ? std::string() : hud.error().message));
	f->hud = std::move(*hud);
	f->hud->initialize(HudModel{});
	return f;
}

std::string field(size_t p_index) {
	return fmt("instance1.DebugField_%02u", unsigned(p_index + 1));
}

std::vector<DebugFieldText> texts(const bf3::host::NetStats &p_stats) {
	std::vector<DebugFieldText> out;
	for (const bf3::host::NetOverlayRow &r : bf3::host::net_overlay_rows(p_stats)) {
		out.push_back({ r.label, r.value });
	}
	return out;
}

}

TEST_SUITE("ui-debug-screen") {
	TEST_CASE("ui debug screen: the retail DebugField AS2 hides every field at onClipLoad, colours it red and sizes it "
			  "to its placement") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<DebugFixture> f = open_debug_screen(*r, hud_movie_screen_asset(kMovie));
		CHECK(f->hud->stats().unknown_class_paths.empty());
		size_t fields = 0, hidden = 0, red = 0, sized = 0;
		for (size_t i = 0; i < 72; ++i) {
			const As2Clip c = f->clip(field(i));
			if (!c.exists()) {
				continue;
			}
			++fields;
			hidden += c.visible() ? 0u : 1u;
			red += c.child("txtLabel").text_color() == std::optional<uint32_t>(0xFF0000) &&
							c.child("txtValue").text_color() == std::optional<uint32_t>(0xFF0000)
					? 1u
					: 0u;
			const As2Clip top = c.child("bg").child("top");
			const As2Clip right = c.child("bg").child("right");
			sized += c.xscale() == doctest::Approx(100.0f) && c.yscale() == doctest::Approx(100.0f) &&
							c.child("txtLabel").width() == doctest::Approx(top.width() - 4.0f).epsilon(0.02) &&
							right.x() == doctest::Approx(top.width() - 1.0f).epsilon(0.001)
					? 1u
					: 0u;
		}
		CHECK(fields == 72);
		CHECK(hidden == 72);
		CHECK(red == 72);
		CHECK(sized == 72);
		CHECK(f->clip(field(0)).child("txtLabel").text() == "NO DATA");
		CHECK(f->clip(field(0)).child("bg").child("top").width() == doctest::Approx(200.0f).epsilon(0.01));
	}

	TEST_CASE("ui debug screen: the net overlay's fields read the NetStats of the same frame for 300 frames") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		bf3::host::NetStats stats;
		std::vector<DebugFieldText> rows = texts(stats);
		REQUIRE(rows.size() == 15);
		std::unique_ptr<DebugFixture> f = open_debug_screen(*r, debug_screen_asset(rows));
		CHECK(f->hud->stats().unknown_class_paths.empty());
		for (size_t i = 0; i < 72; ++i) {
			CHECK(f->clip(field(i)).visible() == (i < rows.size()));
		}
		CHECK(f->clip(field(0)).child("txtLabel").text() == "DEBUGING IS TURNED OF:");
		CHECK(f->clip(field(0)).child("txtValue").text() == "NO DATA");
		size_t mismatches = 0;
		for (uint32_t frame = 0; frame < 300; ++frame) {
			stats.revision = frame + 1;
			stats.in.total_bytes_per_s = 31000.0f + float(frame * 7);
			stats.out.total_bytes_per_s = 2000.0f + float(frame);
			stats.rtt_ms = float(frame % 90);
			stats.snapshots_per_s = 30.0f - float(frame % 3);
			stats.snapshot_loss = float(frame % 10) / 100.0f;
			stats.ghosts_in_scope = frame % 64;
			stats.server_tick = 6000 + frame * 2;
			stats.in.bytes_per_s[size_t(bf3::net::MessageType::Snapshot)] = 24000.0f + float(frame);
			stats.out.bytes_per_s[size_t(bf3::net::MessageType::CommandPacket)] = 1800.0f + float(frame);
			rows = texts(stats);
			CHECK(debug_screen_show(*f->hud, rows) == rows.size());
			f->step();
			for (size_t i = 0; i < rows.size(); ++i) {
				const As2Clip c = f->clip(field(i));
				mismatches += c.child("txtLabel").text() == rows[i].label + ":" ? 0u : 1u;
				mismatches += c.child("txtValue").text() == rows[i].value ? 0u : 1u;
			}
		}
		CHECK(mismatches == 0);
		CHECK(f->clip(field(2)).child("txtLabel").text() == "RTT:");
		CHECK(f->clip(field(2)).child("txtValue").text() == fmt("%.0f ms", double(299 % 90)));
		CHECK(f->clip(field(5)).child("txtValue").text() == fmt("%u", 299u % 64u));
		CHECK_FALSE(f->clip(field(rows.size())).visible());
	}
}

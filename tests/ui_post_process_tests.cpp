#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/minimap.h"
#include "bf3/core/ui/ui_post_process.h"
#include "test_support.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace {

UiDimmerRamp ramp_250() {
	UiDimmerRamp r;
	r.in.event_id = 1;
	r.in.keys = { { 0.0f, 0.0f }, { 250.0f, 1.0f } };
	r.in.length_ms = 250.0f;
	r.out.event_id = 2;
	r.out.keys = { { 0.0f, 0.0f }, { 250.0f, -1.0f } };
	r.out.length_ms = 250.0f;
	r.visual_environment = "ve";
	return r;
}

struct SubwayLevel {
	bf3::test::Mounted *mount = nullptr;
	std::unique_ptr<EbxLibrary> lib;
};

SubwayLevel *level_or_skip() {
	static SubwayLevel l;
	static bool tried = false;
	if (!tried) {
		tried = true;
		l.mount = bf3::test::shared_mount_or_skip();
		if (l.mount == nullptr) {
			return nullptr;
		}
		Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(l.mount->install, { "Win32/Levels/MP_Subway/MP_Subway" });
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
		l.lib = std::move(*lib);
	} else if (l.mount == nullptr) {
		bf3::test::shared_mount_or_skip();
		return nullptr;
	}
	return &l;
}

}

TEST_SUITE("ui-post-process-unit") {
	TEST_CASE("post process: the glitch power falls at TimeScale per second") {
		UiPostProcessData d;
		d.time_scale = 4.0f;
		CHECK(ui_glitch_power(d, 0.35f, 0.0) == doctest::Approx(0.35f));
		CHECK(ui_glitch_power(d, 0.35f, 0.05) == doctest::Approx(0.15f));
		CHECK(ui_glitch_power(d, 0.35f, 0.0875) == doctest::Approx(0.0f));
		CHECK(ui_glitch_power(d, 0.35f, 1.0) == 0.0f);
		CHECK(ui_glitch_power(d, 0.35f, -1.0) == doctest::Approx(0.35f));
		CHECK(ui_glitch_power(d, -0.35f, 0.0) == 0.0f);
		CHECK(ui_glitch_power(d, std::nanf(""), 0.0) == 0.0f);
		CHECK(ui_glitch_seconds(d, 0.35f) == doctest::Approx(0.0875));
		CHECK(ui_glitch_seconds(d, 0.2f) == doctest::Approx(0.05));
		d.time_scale = 0.0f;
		CHECK(ui_glitch_power(d, 0.35f, 0.0) == 0.0f);
	}

	TEST_CASE("post process: dimmer tracks interpolate and the out track ends the fade") {
		const UiDimmerRamp r = ramp_250();
		CHECK(dimmer_track_value(r.in, -5.0) == 0.0f);
		CHECK(dimmer_track_value(r.in, 125.0) == doctest::Approx(0.5f));
		CHECK(dimmer_track_value(r.in, 400.0) == 1.0f);
		CHECK(dimmer_track_value(r.out, 50.0) == doctest::Approx(-0.2f));
		UiDimmer dim(r);
		CHECK(dim.visibility(0.0) == 0.0f);
		CHECK_FALSE(dim.enabled(0.0));
		dim.fade_out(0.0);
		CHECK(dim.fade_outs() == 0);
		dim.fade_in(1.0);
		CHECK(dim.enabled(1.0));
		CHECK(dim.visibility(1.1) == doctest::Approx(0.4f));
		CHECK(dim.visibility(1.25) == doctest::Approx(1.0f));
		CHECK(dim.visibility(5.0) == 1.0f);
		dim.fade_out(5.0);
		CHECK(dim.visibility(5.1) == doctest::Approx(0.6f));
		CHECK(dim.enabled(5.2));
		CHECK_FALSE(dim.enabled(5.25));
		CHECK(dim.visibility(6.0) == 0.0f);
		dim.fade_in(7.0);
		dim.fade_out(7.1);
		CHECK(dim.visibility(7.2) == doctest::Approx(0.4f));
		CHECK(dim.fade_ins() == 2);
		CHECK(dim.fade_outs() == 2);
	}

	TEST_CASE("post process: the menu grade desaturates before the tint") {
		MenuGrade g;
		g.saturation = { 0.0f, 0.0f, 0.0f };
		g.contrast = { 1.0f, 1.0f, 1.0f };
		g.brightness = { 0.25f, 0.5f, 1.0f };
		const std::array<float, 3> grey = menu_grade_rgb(g, { 0.8f, 0.4f, 0.2f }, 1.0f);
		const float luma = 0.8f * 0.299f + 0.4f * 0.587f + 0.2f * 0.114f;
		CHECK(grey[0] == doctest::Approx(luma * 0.25f));
		CHECK(grey[1] == doctest::Approx(luma * 0.5f));
		CHECK(grey[2] == doctest::Approx(luma));
		const std::array<float, 3> half = menu_grade_rgb(g, { 0.8f, 0.4f, 0.2f }, 0.5f);
		CHECK(half[0] == doctest::Approx(0.4f + luma * 0.125f));
		const std::array<float, 3> none = menu_grade_rgb(g, { 0.8f, 0.4f, 0.2f }, 0.0f);
		CHECK(none[0] == doctest::Approx(0.8f));
		g.contrast = { 2.0f, 2.0f, 2.0f };
		g.brightness = { 1.0f, 1.0f, 1.0f };
		CHECK(menu_grade_rgb(g, { 0.5f, 0.5f, 0.5f }, 1.0f)[0] == doctest::Approx(0.5f));
		CHECK(menu_grade_rgb(g, { 0.9f, 0.9f, 0.9f }, 1.0f)[0] == 1.0f);
		CHECK(menu_grade_rgb(g, { 0.1f, 0.1f, 0.1f }, 1.0f)[0] == 0.0f);
	}

	TEST_CASE("post process: the vignette is zero at the centre and its opacity at the rim") {
		MenuGrade g;
		g.vignette = true;
		g.vignette_scale = { 2.0f, 2.0f };
		g.vignette_opacity = 0.4f;
		g.vignette_exponent = 1.0f;
		CHECK(menu_vignette(g, 0.5f, 0.5f) == 0.0f);
		CHECK(menu_vignette(g, 0.75f, 0.5f) == doctest::Approx(0.2f));
		CHECK(menu_vignette(g, 1.0f, 0.5f) == doctest::Approx(0.4f));
		CHECK(menu_vignette(g, 0.0f, 0.0f) == doctest::Approx(0.4f));
		g.vignette = false;
		CHECK(menu_vignette(g, 0.0f, 0.0f) == 0.0f);
	}
}

TEST_SUITE("ui-post-process") {
	TEST_CASE("post process: UIPostProcessComponentData from the install") {
		SubwayLevel *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<UiPostProcessData> d = read_ui_post_process(*l->lib);
		REQUIRE_MESSAGE(d.ok(), (d.ok() ? std::string() : d.error().message));
		CHECK(d->enabled);
		CHECK(d->r_shift == doctest::Approx(0.01f));
		CHECK(d->g_shift == 0.0f);
		CHECK(d->b_shift == 0.0f);
		CHECK(d->distort == doctest::Approx(0.025f));
		CHECK(d->x_power == doctest::Approx(0.5f));
		CHECK(d->y_power == 0.0f);
		CHECK(d->time_scale == doctest::Approx(4.0f));
		CHECK(d->highlight == doctest::Approx(2.0f));
		CHECK(d->highlight_threshold == doctest::Approx(0.5f));
		CHECK(d->noise_texture == "ui/static/postprocess/tvnoise");
		REQUIRE(d->data_sources.size() == 1);
		CHECK(d->data_sources[0] == "Power");
		Result<L8Image> noise = load_l8_texture(l->lib->mount(0), d->noise_texture);
		REQUIRE_MESSAGE(noise.ok(), (noise.ok() ? std::string() : noise.error().message));
		CHECK(noise->width == 256);
		CHECK(noise->height == 256);
		size_t distinct = 0;
		std::array<bool, 256> seen{};
		for (const uint8_t t : noise->texels) {
			if (!seen[t]) {
				seen[t] = true;
				++distinct;
			}
		}
		CHECK(distinct > 64);
		std::printf("ui.post_process data: RShift %.3f Distort %.3f XPower %.2f TimeScale %.1f Highlight %.1f/%.1f noise %s %ux%u L8, %zu levels\n",
				double(d->r_shift), double(d->distort), double(d->x_power), double(d->time_scale), double(d->highlight), double(d->highlight_threshold),
				d->noise_texture.c_str(), noise->width, noise->height, distinct);
	}

	TEST_CASE("post process: the menu visual environment from the install") {
		SubwayLevel *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<MenuGrade> g = read_menu_visual_environment(*l->lib);
		REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
		CHECK(g->correction);
		CHECK(g->brightness[0] == doctest::Approx(0.338f));
		CHECK(g->brightness[1] == doctest::Approx(0.572f));
		CHECK(g->brightness[2] == doctest::Approx(0.75f));
		CHECK(g->contrast[0] == doctest::Approx(1.2f));
		CHECK(g->saturation[0] == 0.0f);
		CHECK(g->vignette);
		CHECK(g->vignette_opacity == doctest::Approx(0.4f));
		CHECK(g->vignette_scale[0] == doctest::Approx(2.0f));
		CHECK(g->vignette_exponent == doctest::Approx(1.0f));
		CHECK(g->dof);
		CHECK(g->blur_deviation == doctest::Approx(3.0f));
		CHECK(g->blur_add == doctest::Approx(3.0f));
		CHECK(g->focus_distance == doctest::Approx(1.0f));
		CHECK(g->far_distance_scale == doctest::Approx(3.9f));
		const std::array<float, 3> mid = menu_grade_rgb(*g, { 0.5f, 0.5f, 0.5f }, 1.0f);
		CHECK(mid[2] > mid[1]);
		CHECK(mid[1] > mid[0]);
	}

	TEST_CASE("post process: the ingame dimmer ramps from the install") {
		SubwayLevel *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<UiDimmerRamp> r = read_ingame_dimmer(*l->lib);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
		CHECK(r->visual_environment == kMenuVisualEnvironmentPartition);
		CHECK(r->in.event_id == -1778993948);
		CHECK(r->out.event_id == 1422740397);
		CHECK(r->in.length_ms == 250.0f);
		CHECK(r->out.length_ms == 250.0f);
		CHECK(dimmer_track_value(r->in, 250.0) == 1.0f);
		CHECK(dimmer_track_value(r->out, 250.0) == -1.0f);
		UiDimmer dim(*r);
		dim.fade_in(0.0);
		CHECK(dim.visibility(0.125) == doctest::Approx(0.5f));
		dim.fade_out(1.0);
		CHECK_FALSE(dim.enabled(1.25));
	}

	TEST_CASE("post process: the graphs' PostProcessEffect powers") {
		SubwayLevel *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		const std::array<std::pair<std::string_view, size_t>, 3> graphs{ {
				{ "ui/flow/graph/spawn/spawngraphpc", 2 },
				{ "ui/flow/graph/spawn/spawnbuttongraph", 1 },
				{ "ui/flow/graph/ingame/mp/ingamemenumpgraph", 3 },
		} };
		for (const auto &[graph, count] : graphs) {
			Result<std::vector<PostProcessAction>> a = read_post_process_actions(*l->lib, graph);
			REQUIRE_MESSAGE(a.ok(), (a.ok() ? std::string() : a.error().message));
			CHECK_MESSAGE(a->size() == count, std::string(graph));
			for (const PostProcessAction &p : *a) {
				CHECK(p.name == "PostProcessEffect");
				CHECK(p.power == doctest::Approx(0.35f));
			}
		}
		Result<std::vector<PostProcessAction>> eor = read_post_process_actions(*l->lib, "ui/flow/graph/endofround/preendofroundgraph");
		REQUIRE_MESSAGE(eor.ok(), (eor.ok() ? std::string() : eor.error().message));
		bool scoreboard = false;
		for (const PostProcessAction &p : *eor) {
			scoreboard = scoreboard || std::fabs(p.power - 0.2f) < 1e-6f;
		}
		CHECK(scoreboard);
	}

	TEST_CASE("post process: install unchanged") {
		SubwayLevel *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		const SnapshotDiff diff = diff_snapshots(l->mount->before, l->mount->reader.snapshot());
		std::printf("ui.post_process install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}

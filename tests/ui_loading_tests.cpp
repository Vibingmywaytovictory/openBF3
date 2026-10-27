#include <doctest/doctest.h>

#include "bf3/core/level/level.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/loading_screen.h"
#include "bf3/core/ui/swf_image.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/text_db.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <memory>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui_retail;

namespace {

constexpr double kFrame = 1.0 / 30.0;
constexpr uint64_t kSeed = 1;

struct LoadingFixture {
	level::LevelInfo info;
	std::unique_ptr<LoadingScreenData> data;
	std::unique_ptr<SwfScene> scene;
	std::unique_ptr<HudScreen> hud;
	const TextDatabase *text = nullptr;

	std::string sid(std::string_view p_sid) const {
		const std::optional<std::string_view> t = text->find_sid(p_sid);
		return t ? std::string(*t) : std::string(p_sid);
	}
	void step() {
		hud->advance(kFrame);
		const Status st = scene->advance_seconds(kFrame);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	}
	std::string text_of(std::string_view p_path) { return hud->clip(p_path).text(); }
	bool visible(std::string_view p_path) { return hud->clip(p_path).visible(); }
};

std::unique_ptr<LoadingFixture> open_metro_loading(Retail &r_r) {
	auto f = std::make_unique<LoadingFixture>();
	Result<LevelMount> mount = level::mount_level_description(*r_r.mount->install, "MP_Subway");
	REQUIRE_MESSAGE(mount.ok(), (mount.ok() ? std::string() : mount.error().message));
	Result<level::LevelInfo> info = level::level_info(*mount, "MP_Subway");
	REQUIRE_MESSAGE(info.ok(), (info.ok() ? std::string() : info.error().message));
	f->info = *info;
	Result<std::vector<std::string>> hints = level::level_hints(*mount, info->loading.hint_asset);
	REQUIRE_MESSAGE(hints.ok(), (hints.ok() ? std::string() : hints.error().message));
	f->data = std::make_unique<LoadingScreenData>(
			loading_screen_source(*info, "TeamDeathMatch0", *hints, "win32", kSeed));
	f->text = &retail_text_db(r_r);
	const std::string movie = loading_screen_movie(info->loading.movie);
	Result<SwfScene> scene = SwfScene::open(*r_r.lib, movie);
	REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? std::string() : scene.error().message));
	f->scene = std::make_unique<SwfScene>(std::move(*scene));
	f->scene->set_image_source(std::make_shared<SwfMountImages>(std::move(*mount)));
	HudStageRects rects;
	rects.original = f->scene->stage();
	rects.visible = f->scene->stage();
	Result<std::unique_ptr<HudScreen>> hud = HudScreen::open(*f->scene, *r_r.lib, hud_movie_screen_asset(movie),
			r_r.fonts.get(), retail_text_client(*f->text), rects, f->data.get());
	REQUIRE_MESSAGE(hud.ok(), (hud.ok() ? std::string() : hud.error().message));
	f->hud = std::move(*hud);
	f->hud->initialize(HudModel{});
	return f;
}

}

TEST_SUITE("ui-loading") {
	TEST_CASE("ui loading: Metro TDM's loading screen reads the level, the mode and the hints") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<LoadingFixture> f = open_metro_loading(*r);
		CHECK(loading_screen_movie(f->info.loading.movie) == "ui/assets/loadingscreen");
		CHECK(loading_screen_game_mode("TeamDeathMatch0") == "team deathmatch");
		CHECK(loading_screen_game_mode("TeamDeathMatchC0") == "team deathmatch cq");
		CHECK(loading_screen_game_mode("ConquestLarge0") == "conquest");
		CHECK(f->data->source().game_type == "multiplayer");
		CHECK(f->hud->controller(kHudScreenInstance) != nullptr);
		CHECK(f->hud->stats().unknown_class_paths.empty());
		const std::string root(kHudScreenInstance);
		CHECK(f->text_of(root + ".header") == "TEAM DEATHMATCH");
		CHECK(f->text_of(root + ".header") == f->sid("ID_M_LOAD_TEAM_DEATHMATCH"));
		CHECK(f->text_of(root + ".subheader") == f->sid("ID_M_LEVELNAME_MP_SUBWAY"));
		CHECK(f->text_of(root + ".upperText") == f->sid("ID_M_TEAM_DEATHMATCH_DESC"));
		CHECK(f->text_of(root + ".upperText").starts_with("In Team Deathmatch"));
		const As2Clip upper = f->hud->clip(root + ".upperText");
		const float upper_height = upper.text_layout_bounds().height();
		const float top = f->hud->clip(root + ".bg.bgTop").height();
		CHECK(upper_height > 0.0f);
		CHECK(upper.height() == doctest::Approx(upper_height));
		CHECK(upper_height < upper.local_bounds().height());
		CHECK(top >= upper.y() + upper_height + 10.0f - 0.01f);
		CHECK(top < upper.y() + upper.local_bounds().height() + 10.0f);
		CHECK_FALSE(f->visible(root + ".rspServerInfo"));
		CHECK_FALSE(f->visible(root + ".mcJoinSquad"));
		CHECK(f->data->get_data("joinSquadHasShowed").truthy());
		CHECK(f->data->hints_served() == 1);
		CHECK(f->visible(root + ".hint"));
		CHECK(f->text_of(root + ".hint") == f->sid(f->data->last_hint()));
		CHECK(f->data->last_hint().starts_with("ID_M_HINT_"));
		CHECK(f->hud->intervals() == 1);
	}

	TEST_CASE("ui loading: the level image loads through img:// and fills the visible rect") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<LoadingFixture> f = open_metro_loading(*r);
		const std::string holder = std::string(kHudScreenInstance) + ".bgHolder";
		f->step();
		const SwfSceneNode *n = f->scene->find_node(holder);
		REQUIRE(n != nullptr);
		CHECK(n->image == "levels/mp_subway/ui/mp15_loading");
		CHECK(n->image_width == 1024);
		CHECK(n->image_height == 512);
		f->step();
		const As2Clip clip = f->hud->clip(holder);
		CHECK(clip.width() == doctest::Approx(1280.0).epsilon(0.001));
		CHECK(clip.height() == doctest::Approx(720.0).epsilon(0.001));
		HudStageRects wide = f->hud->stage_rects();
		wide.visible = SwfRect{ -160.0f, 1440.0f, 0.0f, 720.0f };
		f->hud->set_stage_rects(wide);
		f->step();
		CHECK(f->hud->clip(holder).width() == doctest::Approx(1600.0).epsilon(0.001));
	}

	TEST_CASE("ui loading: the hint changes every 13 s") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<LoadingFixture> f = open_metro_loading(*r);
		const std::string hint = std::string(kHudScreenInstance) + ".hint";
		const std::string first = f->text_of(hint);
		const std::string first_sid = f->data->last_hint();
		double t = 0.0;
		while (t + kFrame < 12.9) {
			f->step();
			t += kFrame;
		}
		CHECK(f->data->hints_served() == 1);
		CHECK(f->text_of(hint) == first);
		while (t < 13.1) {
			f->step();
			t += kFrame;
		}
		CHECK(f->data->hints_served() == 2);
		CHECK(f->data->last_hint() != first_sid);
		CHECK(f->text_of(hint) == f->sid(f->data->last_hint()));
		CHECK(f->text_of(hint) != first);
		while (t < 26.1) {
			f->step();
			t += kFrame;
		}
		CHECK(f->data->hints_served() == 3);
	}

	TEST_CASE("ui loading: LoadingMessage shows LOADING, then the level name at frame 68, alternating") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<LoadingFixture> f = open_metro_loading(*r);
		const std::string path = loading_message_path(*f->hud);
		REQUIRE_FALSE(path.empty());
		const std::string name = f->sid("ID_M_LEVELNAME_MP_SUBWAY");
		const std::string loading = f->sid("ID_M_LOADING");
		CHECK(loading == "LOADING");
		std::vector<std::string> per_frame{ f->text_of(path + ".text.msg.txt") };
		std::vector<int> changed_at;
		for (int i = 1; i < 30 * 10; ++i) {
			f->step();
			per_frame.push_back(f->text_of(path + ".text.msg.txt"));
			if (i > 1 && per_frame[size_t(i)] != per_frame[size_t(i - 1)]) {
				changed_at.push_back(i);
			}
			CHECK(f->text_of(path + ".text.msg.txtBg") == per_frame.back());
		}
		MESSAGE("LoadingMessage frame 0 '" << per_frame[0] << "', frame 1 '" << per_frame[1] << "', changes at "
										   << (changed_at.size() > 0 ? changed_at[0] : -1) << " "
										   << (changed_at.size() > 1 ? changed_at[1] : -1));
		CHECK(per_frame[1] == loading);
		CHECK(per_frame[67] == loading);
		CHECK(per_frame[68] == name);
		REQUIRE(changed_at.size() >= 3);
		CHECK(changed_at[0] == 68);
		for (size_t i = 1; i < changed_at.size(); ++i) {
			CHECK(changed_at[i] - changed_at[i - 1] == 58);
		}
		for (size_t i = 1; i < changed_at.size(); ++i) {
			CHECK(per_frame[size_t(changed_at[i])] == (i % 2 == 1 ? loading : name));
		}
	}
}

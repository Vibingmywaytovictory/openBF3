#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_image.h"
#include "bf3/core/ui/swf_scene.h"
#include "image_manager.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <tuple>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui_retail;

namespace {

constexpr std::string_view kDogTagLeft = "UI/Art/Persistence/DogTag/inactivetag_left";
constexpr std::string_view kDogTagRight = "UI/Art/Persistence/DogTag/inactivetag_right";
constexpr std::string_view kWeaponLineArt = "UI/Art/Persistence/Weapons/m16a4";
constexpr std::string_view kWeaponImage = "instance1.KillCam_01.killed.weaponImage";
constexpr std::string_view kRankImage = "instance1.KillCam_01.killed.rankImage";

std::vector<SwfNodeId> order_of(const SwfScene &p_scene) {
	return p_scene.draw_order();
}

size_t index_in(const std::vector<SwfNodeId> &p_order, SwfNodeId p_id) {
	return size_t(std::find(p_order.begin(), p_order.end(), p_id) - p_order.begin());
}

}

TEST_SUITE("ui-scene2") {
	TEST_CASE("swf image: Win32/UI TextureAssets give img:// images their sizes") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		for (const auto &[url, w, h] : { std::tuple{ kDogTagLeft, 256u, 256u }, std::tuple{ kDogTagRight, 256u, 256u }, std::tuple{ kWeaponLineArt, 256u, 64u } }) {
			const SwfImageInfo info = r->images->request(swf_image_res_name(url));
			CHECK_MESSAGE(info.status == SwfImageStatus::Ready, (std::string(url) + ": " + info.error));
			CHECK(info.width == w);
			CHECK(info.height == h);
		}
		const SwfImageInfo none = r->images->request("ui/art/persistence/weapons/no_such_weapon");
		CHECK(none.status == SwfImageStatus::Missing);
		CHECK_FALSE(none.error.empty());
	}

	TEST_CASE("ImageManager: killscreen's weaponImage streams the line-art into imgHolder, resizes, centres, queues and reports") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kKillScreenPartition);
		auto *weapon = dynamic_cast<ImageManagerWidget *>(s.hud->controller(kWeaponImage));
		REQUIRE(weapon != nullptr);
		HudWidgetHost &host = *s.hud->host(kWeaponImage);
		const SwfSceneNode &self = s.at(kWeaponImage);
		CHECK(self.matrix.a == doctest::Approx(1.0f));
		CHECK(self.matrix.d == doctest::Approx(1.0f));
		REQUIRE(weapon->holder().exists());
		CHECK(weapon->holder().path() == std::string(kWeaponImage) + ".imgHolder");
		const As2Clip size = host.clip("sizeClip");
		CHECK_FALSE(size.visible());
		CHECK(size.width() == doctest::Approx(256.0f).epsilon(0.001));
		CHECK(size.height() == doctest::Approx(64.0f).epsilon(0.001));

		std::vector<std::string> calls;
		weapon->register_callback([&](std::string_view p_message, ImageManagerWidget &) { calls.emplace_back(p_message); });
		weapon->update(host, "ImageUrl", As2Value::of(std::string(kWeaponLineArt)));
		CHECK(calls == std::vector<std::string>{ "LoadStarted" });
		CHECK(weapon->currently_loading());
		CHECK(host.clip("spinnerMc").visible());
		s.hud->advance(1.0 / 30.0);
		CHECK(calls == std::vector<std::string>{ "LoadStarted", "ImageLoaded" });
		CHECK(weapon->is_loaded());
		CHECK_FALSE(host.clip("spinnerMc").visible());
		const SwfSceneNode &holder = s.at(std::string(kWeaponImage) + ".imgHolder");
		CHECK(holder.image == "ui/art/persistence/weapons/m16a4");
		CHECK(holder.image_width == 256);
		CHECK(holder.image_height == 64);
		CHECK(weapon->holder().width() == doctest::Approx(256.0f));
		CHECK(weapon->holder().x() == 0.0f);

		weapon->update_image_url(host, kWeaponLineArt);
		CHECK(calls.size() == 2);

		weapon->resize_to_target = true;
		weapon->center_image = true;
		weapon->update_image_url(host, kDogTagLeft);
		weapon->update_image_url(host, kDogTagRight);
		CHECK(calls == std::vector<std::string>{ "LoadStarted", "ImageLoaded", "UnLoadImage", "LoadStarted" });
		CHECK(s.at(std::string(kWeaponImage) + ".imgHolder").image.empty());
		s.hud->advance(1.0 / 30.0);
		CHECK(calls == std::vector<std::string>{ "LoadStarted", "ImageLoaded", "UnLoadImage", "LoadStarted", "UnLoadImage", "LoadStarted", "ImageLoaded" });
		s.hud->advance(1.0 / 30.0);
		CHECK(calls.size() == 8);
		CHECK(calls.back() == "ImageLoaded");
		CHECK(s.at(std::string(kWeaponImage) + ".imgHolder").image == "ui/art/persistence/dogtag/inactivetag_right");
		CHECK(weapon->holder().width() == doctest::Approx(64.0f));
		CHECK(weapon->holder().height() == doctest::Approx(64.0f));
		CHECK(weapon->holder().x() == doctest::Approx(size.width() * 0.5f - 32.0f));
		CHECK(weapon->holder().y() == doctest::Approx(size.height() * 0.5f - 32.0f));
		CHECK(weapon->holder().x() == doctest::Approx(96.0f).epsilon(0.001));

		auto *rank = dynamic_cast<ImageManagerWidget *>(s.hud->controller(kRankImage));
		REQUIRE(rank != nullptr);
		std::vector<std::string> rank_calls;
		rank->register_callback([&](std::string_view p_message, ImageManagerWidget &) { rank_calls.emplace_back(p_message); });
		rank->update_image_url(*s.hud->host(kRankImage), "UI/Art/Persistence/Weapons/no_such_weapon");
		s.hud->advance(1.0 / 30.0);
		CHECK(rank_calls == std::vector<std::string>{ "LoadStarted", "LoadError2" });
		CHECK_FALSE(rank->is_loaded());
		CHECK_FALSE(rank->currently_loading());
		rank->update_image_url(*s.hud->host(kRankImage), "no_slash");
		CHECK(rank_calls.size() == 2);
		CHECK_FALSE(rank->currently_loading());
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}

	TEST_CASE("DogTagPair on the retail movie: the tags sit at 19,-65 rot 8 and 190,-45 rot -5, and swapDepths raises the advanced tag") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		Result<SwfScene> opened = SwfScene::open(*r->lib, "ui/assets/dogtagpair");
		REQUIRE(opened.ok());
		SwfScene &s = *opened;
		const As2Clip holder(&s, "widget.holder");
		REQUIRE(holder.exists());
		holder.child("sizeClip").set_visible(false);
		const As2Clip advanced = holder.attach_movie("DogTag", "advancedTag", holder.next_highest_depth());
		const As2Clip basic = holder.attach_movie("DogTag", "basicTag", holder.next_highest_depth());
		REQUIRE(advanced.exists());
		REQUIRE(basic.exists());
		CHECK(advanced.depth() == 0);
		CHECK(basic.depth() == 1);
		basic.set_x(19.0f);
		basic.set_y(-65.0f);
		basic.set_rotation(8.0f);
		advanced.set_x(190.0f);
		advanced.set_y(-45.0f);
		advanced.set_rotation(-5.0f);
		std::vector<SwfNodeId> order = order_of(s);
		CHECK(index_in(order, advanced.node()->id) < index_in(order, basic.node()->id));
		advanced.swap_depths(basic);
		order = order_of(s);
		CHECK(advanced.depth() == 1);
		CHECK(basic.depth() == 0);
		CHECK(index_in(order, basic.node()->id) < index_in(order, advanced.node()->id));
		CHECK(basic.x() == 19.0f);
		CHECK(basic.y() == -65.0f);
		CHECK(basic.rotation() == doctest::Approx(8.0f));
		CHECK(basic.xscale() == doctest::Approx(100.0f));
		CHECK(advanced.x() == 190.0f);
		CHECK(advanced.y() == -45.0f);
		CHECK(advanced.rotation() == doctest::Approx(-5.0f));
		As2ColorTransformPatch inactive;
		inactive.aa = 100.0;
		inactive.ra = inactive.ga = inactive.ba = 30.0;
		inactive.ab = 0.0;
		inactive.rb = 7.0;
		inactive.gb = inactive.bb = 11.0;
		basic.set_color_transform(inactive);
		CHECK(basic.node()->color.mul[0] == doctest::Approx(0.3f));
		CHECK(basic.node()->color.add[2] == 11.0f);
		s.take_changes();
		const SwfSceneNode &b = *basic.node();
		CHECK(b.world.a == doctest::Approx(std::cos(8.0 * std::numbers::pi / 180.0)));
		CHECK(b.world.tx == doctest::Approx(19.0f));
	}
}

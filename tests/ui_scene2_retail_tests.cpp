#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_image.h"
#include "bf3/core/ui/swf_scene.h"
#include "image_manager.h"
#include "scroll_text.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

SwfRect world_rect(const SwfRect &p_local, const SwfMatrix &p_world) {
	SwfRect r{ std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest() };
	for (const auto &[x, y] : { std::pair{ p_local.x_min, p_local.y_min }, std::pair{ p_local.x_max, p_local.y_min }, std::pair{ p_local.x_min, p_local.y_max }, std::pair{ p_local.x_max, p_local.y_max } }) {
		const float wx = p_world.a * x + p_world.c * y + p_world.tx;
		const float wy = p_world.b * x + p_world.d * y + p_world.ty;
		r.x_min = std::min(r.x_min, wx);
		r.x_max = std::max(r.x_max, wx);
		r.y_min = std::min(r.y_min, wy);
		r.y_max = std::max(r.y_max, wy);
	}
	return r;
}

SwfRect world_rect(const SwfScene &p_scene, const SwfSceneNode &p_node) {
	return world_rect(swf_node_local_bounds(p_scene, p_node), p_node.world);
}

bool masked_by(const SwfSceneNode &p_node, SwfNodeId p_mask) {
	return std::find(p_node.masks.begin(), p_node.masks.end(), p_mask) != p_node.masks.end();
}

void advance_for(HudScreen &r_hud, double p_seconds) {
	constexpr double kStep = 1.0 / 32.0;
	for (double t = 0.0; t + kStep * 0.5 < p_seconds; t += kStep) {
		r_hud.advance(kStep);
	}
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

	TEST_CASE("ScrollText on the spawn screen's kitbar: the runtime mask clips kitItem text, and ScrollTextTimer scrolls it out and back") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kSpawnScreenPcPartition);
		const std::string item = "instance1.KitBar_01.kitItem_0";
		auto *kit = dynamic_cast<ScrollTextWidget *>(s.hud->controller(item));
		REQUIRE(kit != nullptr);
		for (int i = 1; i < 5; ++i) {
			CHECK(dynamic_cast<ScrollTextWidget *>(s.hud->controller("instance1.KitBar_01.kitItem_" + std::to_string(i))) != nullptr);
		}
		const As2Clip txt = kit->field();
		REQUIRE(txt.exists());
		CHECK(txt.path() == item + ".txt");
		const float field_w = txt.width();
		const float field_h = txt.height();
		CHECK(field_w > 0.0f);
		CHECK(kit->mask_width() == field_w);
		const SwfSceneNode &mask = s.at(item + ".mask");
		REQUIRE(mask.drawing != nullptr);
		CHECK(kit->mask().local_bounds() == SwfRect{ 0.0f, field_w, 0.0f, field_h });
		CHECK(s.at(item).script_mask == mask.id);
		s.scene->take_changes();
		CHECK(s.at(item + ".mask").is_mask);
		CHECK(masked_by(s.at(item + ".txt"), mask.id));
		CHECK_FALSE(kit->registered());

		kit->set_text("ASSAULT");
		CHECK_FALSE(kit->registered());
		CHECK(kit->scroll_distance() < 0.0f);
		CHECK(txt.width() == doctest::Approx(field_w));
		CHECK(txt.x() == 0.0f);

		const std::string long_name = "ASSAULT RIFLE WITH AN UNDERSLUNG GRENADE LAUNCHER";
		kit->set_text(long_name);
		const float d = txt.text_width() - field_w;
		REQUIRE(d > 0.0f);
		CHECK(kit->registered());
		CHECK(kit->scroll_distance() == doctest::Approx(d));
		CHECK(kit->text_width() == field_w);
		CHECK(txt.node()->text_options.auto_size == SwfTextAutoSize::Left);
		CHECK(txt.text_layout_bounds().width() > field_w);
		CHECK(kit->preferred_width() == doctest::Approx(txt.text_layout_bounds().width()));
		REQUIRE(kit->timer() != nullptr);
		CHECK(kit->timer()->listeners() == 1);
		CHECK(kit->timer()->phase() == ScrollTextTimer::Phase::BeginDelay);
		s.scene->take_changes();
		const SwfRect clip_rect = world_rect(*s.scene, s.at(item + ".mask"));
		CHECK(clip_rect.width() == doctest::Approx(field_w));
		const auto text_rect = [&] { return world_rect(txt.text_layout_bounds(), s.at(item + ".txt").world); };
		CHECK(text_rect().x_max > clip_rect.x_max + d * 0.5f);
		CHECK(masked_by(s.at(item + ".txt"), s.at(item + ".mask").id));

		advance_for(*s.hud, ScrollTextTimer::kBeginDelay);
		CHECK(kit->timer()->phase() == ScrollTextTimer::Phase::ScrollForward);
		CHECK(kit->timer()->tweening());
		CHECK(txt.x() == 0.0f);
		advance_for(*s.hud, 1.0);
		CHECK(kit->timer()->value() == doctest::Approx(0.5));
		CHECK(txt.x() == doctest::Approx(-(d + 5.0f) * 0.5f).epsilon(0.001));

		const std::string other = "instance1.KitBar_01.kitItem_1";
		auto *kit1 = dynamic_cast<ScrollTextWidget *>(s.hud->controller(other));
		REQUIRE(kit1 != nullptr);
		kit1->set_text(long_name + " AND A TACTICAL LIGHT");
		CHECK(kit1->timer() == kit->timer());
		CHECK(kit->timer()->listeners() == 2);
		CHECK(kit1->field().x() == doctest::Approx(-(kit1->scroll_distance() + 5.0f) * 0.5f).epsilon(0.001));

		advance_for(*s.hud, 1.0);
		CHECK(kit->timer()->phase() == ScrollTextTimer::Phase::EndDelay);
		CHECK_FALSE(kit->timer()->tweening());
		CHECK(txt.x() == doctest::Approx(-(d + 5.0f)).epsilon(0.001));
		s.scene->take_changes();
		CHECK(text_rect().x_min < clip_rect.x_min - d * 0.5f);
		advance_for(*s.hud, ScrollTextTimer::kEndDelay);
		CHECK(kit->timer()->phase() == ScrollTextTimer::Phase::ScrollBack);
		CHECK(txt.x() == doctest::Approx(-(d + 5.0f)).epsilon(0.001));
		advance_for(*s.hud, ScrollTextTimer::kScrollBackTime * 0.5);
		CHECK(txt.x() == doctest::Approx(-(d + 5.0f) * 0.5f).epsilon(0.001));
		advance_for(*s.hud, ScrollTextTimer::kScrollBackTime * 0.5);
		CHECK(kit->timer()->phase() == ScrollTextTimer::Phase::BeginDelay);
		CHECK(txt.x() == 0.0f);
		CHECK(kit1->field().x() == 0.0f);

		kit->set_width(field_w * 0.5f);
		CHECK(kit->mask().width() == doctest::Approx(field_w * 0.5f));
		CHECK(kit->scroll_distance() == doctest::Approx(txt.text_width() - field_w * 0.5f));
		kit->set_text("AK");
		CHECK_FALSE(kit->registered());
		CHECK(kit->timer()->listeners() == 1);
		CHECK(txt.node()->text_options.auto_size == SwfTextAutoSize::None);
		CHECK(txt.width() == doctest::Approx(field_w * 0.5f));
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}

	TEST_CASE("List viewport mask on the spawn screen's List_01: setupMask's listMask clips the rows below the start height") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kSpawnScreenPcPartition);
		const As2Clip list = s.hud->clip("instance1.List_01");
		REQUIRE(list.exists());
		const float start_w = std::round(list.width());
		const float start_h = std::round(list.height());
		list.set_xscale(100.0f);
		list.set_yscale(100.0f);
		list.set_x(std::round(list.x()));
		list.set_y(std::round(list.y()));
		const As2Clip size = list.child("listSize");
		size.set_width(start_w);
		size.set_height(start_h);
		size.set_alpha(50.0f);
		size.set_visible(false);
		const As2Clip holder = list.create_empty_movie_clip("ListHolder", list.next_highest_depth());
		const As2Clip list_mask = list.attach_movie("listMask", "listMask", list.next_highest_depth());
		REQUIRE(holder.exists());
		REQUIRE(list_mask.exists());
		list_mask.set_x(-5.0f);
		list_mask.set_width(start_w + 10.0f);
		list_mask.set_height(start_h);
		list_mask.set_alpha(0.0f);
		holder.set_mask(list_mask);
		std::vector<As2Clip> rows;
		float y = 0.0f;
		for (int i = 0; y < start_h * 2.0f && i < 64; ++i) {
			const As2Clip row = holder.attach_movie("teamSpawn", "row" + std::to_string(i), holder.next_highest_depth());
			REQUIRE(row.exists());
			row.set_y(y);
			REQUIRE(row.height() > 0.0f);
			y += row.height();
			rows.push_back(row);
		}
		s.scene->take_changes();
		const SwfSceneNode &m = *list_mask.node();
		CHECK(m.is_mask);
		CHECK(s.at(holder.path()).script_mask == m.id);
		const SwfRect view = world_rect(*s.scene, m);
		CHECK(view.width() == doctest::Approx(start_w + 10.0f));
		CHECK(view.height() == doctest::Approx(start_h));
		uint32_t inside = 0;
		uint32_t outside = 0;
		for (const As2Clip &row : rows) {
			const SwfSceneNode &n = *row.node();
			CHECK(masked_by(n, m.id));
			const SwfRect rr = world_rect(*s.scene, n);
			if (rr.y_max <= view.y_max + 0.5f) {
				inside++;
			} else if (rr.y_min >= view.y_max - 0.5f) {
				outside++;
			}
		}
		CHECK(inside > 0);
		CHECK(outside > 0);
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
}

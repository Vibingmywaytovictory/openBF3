#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/ui_input.h"
#include "dog_tag.h"
#include "image_manager.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <cmath>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui_retail;

namespace {

constexpr std::string_view kKillTagPair = "instance1.KillCam_01.killed.kill_tag_pair";
constexpr std::string_view kSuicideTagPair = "instance1.KillCam_01.suicide.tag_pair";
constexpr std::string_view kDefaultLeft = "ui/art/persistence/dogtag/defaulttag_left";
constexpr std::string_view kDefaultRight = "ui/art/persistence/dogtag/defaulttag_right";

void advance_frames(HudScreen &r_hud, int p_frames) {
	for (int i = 0; i < p_frames; ++i) {
		r_hud.advance(1.0 / 30.0);
	}
}

As2Value tag_data(std::string_view p_image) {
	As2Value v = As2Value::object();
	v.set("Image", As2Value::of(std::string(p_image)));
	return v;
}

}

TEST_SUITE("ui-kill-card") {
	TEST_CASE(
			"DogTagPair on the kill card: the constructor attaches the advanced then the basic DogTag into holder, "
			"each DogTag its ImageManager at 256 x 256; the movie's f_static makes the pair static, both tags light") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kKillScreenPartition);
		const std::vector<std::string> unknown{ "instance1.KillCam_01 Widget.ScreenSpecific.KillCam" };
		CHECK(s.hud->stats().unknown_class_paths == unknown);
		for (const std::string_view path : { kKillTagPair, kSuicideTagPair }) {
			auto *pair = dynamic_cast<DogTagPairWidget *>(s.hud->controller(path));
			REQUIRE_MESSAGE(pair != nullptr, std::string(path));
			CHECK(pair->advanced_tag().path() == std::string(path) + ".holder.advancedTag");
			CHECK(pair->basic_tag().path() == std::string(path) + ".holder.basicTag");
			CHECK(pair->advanced_tag().depth() < pair->basic_tag().depth());
			CHECK_FALSE(s.at(std::string(path) + ".holder.sizeClip").visible);
			REQUIRE(pair->basic() != nullptr);
			REQUIRE(pair->advanced() != nullptr);
			CHECK(pair->basic()->type().empty());
			CHECK_FALSE(pair->basic()->image_manager_clip().exists());
		}
		auto &pair = *dynamic_cast<DogTagPairWidget *>(s.hud->controller(kKillTagPair));
		REQUIRE(s.hud->host(kKillTagPair)->construct_param("f_static") != nullptr);
		pair.initialize_pair(As2Value::undefined());
		advance_frames(*s.hud, 1);
		CHECK(pair.is_static());
		CHECK(pair.selected() == "basic");
		const As2Clip &basic = pair.basic_tag();
		const As2Clip &advanced = pair.advanced_tag();
		CHECK(basic.x() == DogTagPairWidget::kBasicX);
		CHECK(basic.y() == DogTagPairWidget::kBasicY);
		CHECK(basic.rotation() == doctest::Approx(DogTagPairWidget::kBasicRotation));
		CHECK(advanced.x() == DogTagPairWidget::kAdvancedX);
		CHECK(advanced.y() == DogTagPairWidget::kAdvancedY);
		CHECK(advanced.rotation() == doctest::Approx(DogTagPairWidget::kAdvancedRotation));
		CHECK(advanced.depth() < basic.depth());
		CHECK(basic.color_transform().ra == doctest::Approx(100.0));
		CHECK(basic.color_transform().rb == doctest::Approx(0.0));
		CHECK(advanced.color_transform().ra == doctest::Approx(100.0));
		CHECK(advanced.color_transform().rb == doctest::Approx(0.0));
		for (const DogTagWidget *tag : { pair.basic(), pair.advanced() }) {
			REQUIRE(tag->image_manager() != nullptr);
			CHECK(tag->image_manager_clip().node()->name == "ImageManager");
			CHECK(tag->image_manager()->resize_to_target_width);
			CHECK_FALSE(tag->image_manager()->center_image);
			const As2Clip size(s.scene.get(), tag->image_manager_clip().path() + ".sizeClip");
			CHECK(tag->image_manager_clip().xscale() == doctest::Approx(100.0f));
			CHECK(size.width() == doctest::Approx(DogTagWidget::kTagWidth));
			CHECK(size.height() == doctest::Approx(DogTagWidget::kTagHeight));
		}
		CHECK(pair.basic()->type() == "basic");
		CHECK(pair.advanced()->type() == "advanced");
		CHECK(s.hud->stats().unknown_class_paths == unknown);
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}

	TEST_CASE("DogTagPair on the kill card: the default metal tags load, the basic tag writes the player name into a "
			  "DogTagText at 49,116, and the pair reports loaded once both images are in") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kKillScreenPartition);
		auto &pair = *dynamic_cast<DogTagPairWidget *>(s.hud->controller(kKillTagPair));
		std::vector<bool> reports;
		pair.register_callback([&](bool p_loaded) { reports.push_back(p_loaded); });
		pair.initialize_pair(As2Value::undefined());
		As2Value basic = tag_data(kDefaultLeft);
		basic.set("PlayerName", As2Value::of("BOT_Capt. Zorin"));
		pair.update_basic_data(basic);
		CHECK_FALSE(pair.basic_tag().visible());
		pair.update_advanced_data(tag_data(kDefaultRight));
		CHECK_FALSE(pair.is_loaded());
		advance_frames(*s.hud, 2);
		CHECK(pair.is_loaded());
		CHECK(reports == std::vector<bool>{ true });
		CHECK(pair.basic_tag().visible());
		CHECK(pair.advanced_tag().visible());
		const std::string basic_holder = pair.basic_tag().path() + ".ImageManager.imgHolder";
		const std::string advanced_holder = pair.advanced_tag().path() + ".ImageManager.imgHolder";
		CHECK(s.at(basic_holder).image == kDefaultLeft);
		CHECK(s.at(advanced_holder).image == kDefaultRight);
		const As2Clip &manager = pair.basic()->image_manager_clip();
		const As2Clip image(s.scene.get(), basic_holder);
		CHECK(image.width() * manager.xscale() / 100.0f == doctest::Approx(DogTagWidget::kTagWidth));
		CHECK(image.height() * manager.yscale() / 100.0f == doctest::Approx(DogTagWidget::kTagHeight));
		CHECK(pair.basic_tag().parent().parent().visible());

		const As2Clip &name = pair.basic()->name_field();
		REQUIRE(name.exists());
		CHECK(name.parent().path() == pair.basic_tag().path());
		CHECK(name.x() == DogTagWidget::kGamertagX);
		CHECK(name.y() == DogTagWidget::kGamertagY);
		CHECK(name.visible());
		auto *text = dynamic_cast<DogTagTextWidget *>(s.hud->controller(name.path()));
		REQUIRE(text != nullptr);
		CHECK(text->text() == "BOT_Capt. Zorin");
		CHECK_FALSE(text->inverted);
		for (const char *layer : { "bg_text", "inner_glow_text", "bottom_drop_text", "top_drop_text" }) {
			const SwfSceneNode &field = s.at(name.path() + "." + layer);
			CHECK_MESSAGE(field.text == "BOT_Capt. Zorin", layer);
			CHECK(As2Clip(s.scene.get(), name.path() + "." + layer).width() ==
					doctest::Approx(DogTagTextWidget::kTextFieldWidth));
		}
		CHECK_FALSE(pair.basic()->clan_field().exists());
		CHECK_FALSE(pair.advanced()->stats_field().exists());
		CHECK_FALSE(pair.advanced()->tag_name_field().exists());
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}

	TEST_CASE("DogTag data: an empty Image falls back to the inactive tag, a stat of 0 stays hidden, InvertText swaps "
			  "the field for the Inverted linkage, and an absent name hides the field") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kKillScreenPartition);
		auto &pair = *dynamic_cast<DogTagPairWidget *>(s.hud->controller(kSuicideTagPair));
		pair.initialize_pair(As2Value::undefined());
		As2Value advanced = tag_data("");
		advanced.set("StatSID", As2Value::of("ID_M_SCORE"));
		advanced.set("YourStatValue", As2Value::of("0"));
		pair.update_advanced_data(advanced);
		advance_frames(*s.hud, 1);
		CHECK(s.at(pair.advanced_tag().path() + ".ImageManager.imgHolder").image ==
				"ui/art/persistence/dogtag/inactivetag_right");
		const As2Clip &stat_name = pair.advanced()->tag_name_field();
		REQUIRE(stat_name.exists());
		CHECK(stat_name.x() == DogTagWidget::kStatNameX);
		CHECK(stat_name.y() == DogTagWidget::kStatNameY);
		CHECK(s.at(stat_name.path() + ".bg_text").text == "ID_M_SCORE");
		CHECK(s.at(stat_name.path()).symbol == "DogTagTextRight");
		CHECK_FALSE(pair.advanced()->stats_field().exists());
		advanced.set("YourStatValue", As2Value::of("1234"));
		pair.update_advanced_data(advanced);
		REQUIRE(pair.advanced()->stats_field().exists());
		CHECK(s.at(pair.advanced()->stats_field().path() + ".top_drop_text").text == "1234");

		As2Value basic = tag_data(kDefaultLeft);
		basic.set("PlayerName", As2Value::of("Maggy"));
		pair.update_basic_data(basic);
		const std::string plain = pair.basic()->name_field().path();
		CHECK(s.at(plain).symbol == "DogTagText");
		basic.set("InvertText", As2Value::of(true));
		pair.update_basic_data(basic);
		REQUIRE(pair.basic()->name_field().exists());
		CHECK(s.at(pair.basic()->name_field().path()).symbol == "DogTagTextInverted");
		CHECK(dynamic_cast<DogTagTextWidget *>(s.hud->controller(pair.basic()->name_field().path()))->inverted);
		basic.set("PlayerName", As2Value::of(""));
		pair.update_basic_data(basic);
		CHECK_FALSE(pair.basic()->name_field().visible());
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}

	TEST_CASE("DogTagPair switchActiveTag: the selected tag goes light and on top, the other dark; a static pair "
			  "ignores TabLeft and TabRight") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kKillScreenPartition);
		auto &pair = *dynamic_cast<DogTagPairWidget *>(s.hud->controller(kKillTagPair));
		pair.initialize_pair(As2Value::undefined());
		HudWidgetHost &host = *s.hud->host(kKillTagPair);
		CHECK_FALSE(pair.input_concept(host, int32_t(UiInputConcept::TabLeft), true));
		CHECK_FALSE(pair.input_concept(host, int32_t(UiInputConcept::TabRight), true));
		CHECK(pair.selected() == "basic");
		pair.switch_active_tag();
		CHECK(pair.selected() == "advanced");
		CHECK(pair.basic_tag().depth() < pair.advanced_tag().depth());
		CHECK(pair.basic_tag().color_transform().ra == doctest::Approx(30.0));
		CHECK(pair.advanced_tag().color_transform().ra == doctest::Approx(100.0));
		pair.switch_active_tag();
		CHECK(pair.selected() == "basic");
		CHECK(pair.advanced_tag().depth() < pair.basic_tag().depth());
		CHECK(pair.advanced_tag().color_transform().ra == doctest::Approx(30.0));
	}
}

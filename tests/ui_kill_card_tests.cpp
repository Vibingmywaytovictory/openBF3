#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/ui_input.h"
#include "dog_tag.h"
#include "image_manager.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <array>
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
constexpr std::string_view kKillCam = "instance1.KillCam_01";
constexpr std::string_view kKiller = "BOT_Capt. Zorin";
constexpr std::string_view kWeaponImage = "UI/Art/Persistence/Weapons/m16a4";
constexpr uint32_t kWeaponHash = 0xA1u;

void advance_frames(HudScreen &r_hud, int p_frames) {
	for (int i = 0; i < p_frames; ++i) {
		r_hud.advance(1.0 / 30.0);
	}
}

void set_text(std::array<char, kHudTextBytes> &r_out, std::string_view p_text) {
	r_out.fill('\0');
	std::copy_n(p_text.data(), std::min(p_text.size(), r_out.size() - 1), r_out.data());
}

HudModel footage_card() {
	HudModel m{};
	HudKillCard &k = m.kill_card;
	k.serial = 1;
	k.visible = true;
	k.killer = 1;
	k.killer_kit = 1;
	std::copy_n(kKiller.data(), kKiller.size(), k.killer_name.data());
	k.killer_health = 83.4f;
	k.player_killer_kills = 0;
	k.killer_player_kills = 2;
	k.killer_weapon_name = kWeaponHash;
	set_text(k.killer_weapon_image_path, kWeaponImage);
	set_text(k.killer_basic_dog_tag, kDefaultLeft);
	set_text(k.killer_advanced_dog_tag, kDefaultRight);
	return m;
}

HudClientState card_client(Retail &r_r) {
	HudClientState c = retail_text_client(retail_text_db(r_r));
	c.localise_hash = [](uint32_t p_hash) { return p_hash == kWeaponHash ? std::string("AEK-971") : std::string(); };
	return c;
}

std::string clip(std::string_view p_path) {
	return std::string(kKillCam) + "." + std::string(p_path);
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
		CHECK(s.hud->stats().unknown_class_paths.empty());
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
		CHECK(s.hud->stats().unknown_class_paths.empty());
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

	TEST_CASE("KillCam from the 18.57 footage's card: KILLED BY and the killer's name after its header, the weapon "
			  "after its 0 service stars, the kit's name and icon, HEALTH 83%, ME 0 - 2 FOE right-aligned at 1154, no "
			  "PREMIUM strip, and the killer's metal dogtags with the name on the basic tag") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kKillScreenPartition, card_client(*r));
		CHECK(s.hud->stats().unknown_class_paths.empty());
		REQUIRE(s.hud->controller(kKillCam) != nullptr);
		s.hud->initialize(footage_card());
		advance_frames(*s.hud, 2);
		HudWidgetHost &host = *s.hud->host(kKillCam);
		const auto at = [&](std::string_view p_path) { return host.clip(p_path); };
		CHECK(at("killed").visible());
		CHECK_FALSE(at("suicide").visible());
		CHECK(at("killed.killedBy").visible());
		CHECK_FALSE(at("killed.killedByNemesis").visible());
		const As2Clip hdr = at("killed.killedBy.hdr");
		CHECK(s.at(clip("killed.killedBy.name")).text == kKiller);
		CHECK(at("killed.killedBy.name").x() == doctest::Approx(hdr.x() + hdr.text_width() + 10.0f));
		CHECK(s.at(clip("killed.weapon")).text == "AEK-971");
		CHECK(at("killed.weapon").x() == doctest::Approx(at("killed.weaponStars").x() + 55.0f));
		CHECK(s.at(clip("killed.weaponStars.txt")).text == "0");
		CHECK(s.at(clip("killed.kit.txt")).text == "ENGINEER");
		CHECK(at("killed.kitIcon").visible());
		CHECK(s.at(clip("killed.health")).text == "83%");
		const As2Clip health_hdr = at("killed.healthHdr");
		CHECK(at("killed.health").x() == doctest::Approx(health_hdr.x() + health_hdr.text_width() + 10.0f));
		const As2Clip holder = at("killed.MeHimScoreHolder");
		const As2Clip me = at("killed.MeHimScoreHolder.meHdr");
		const As2Clip score = at("killed.MeHimScoreHolder.score");
		const As2Clip him = at("killed.MeHimScoreHolder.himHdr");
		CHECK(s.at(clip("killed.MeHimScoreHolder.score")).text == "0 - 2");
		const auto field_width = [&](std::string_view p_path) {
			return swf_node_text_bounds(s.at(clip(p_path))).width();
		};
		CHECK(field_width("killed.MeHimScoreHolder.meHdr") == std::floor(me.text_width() + 5.0f + 0.5f));
		CHECK(field_width("killed.MeHimScoreHolder.score") == std::floor(score.text_width() + 20.0f + 0.5f));
		CHECK(field_width("killed.MeHimScoreHolder.himHdr") == std::floor(him.text_width() + 5.0f + 0.5f));
		CHECK(score.x() == doctest::Approx(me.width() - 3.0f));
		CHECK(him.x() == doctest::Approx(score.x() + score.width()));
		CHECK(holder.x() == doctest::Approx(std::floor(1154.0f - (holder.width() + 10.0f) + 0.5f)));
		CHECK_FALSE(at("killed.bfpro").visible());
		for (const char *c : { "custIcon1", "custIcon2", "custIcon3" }) {
			CHECK_FALSE_MESSAGE(at(std::string("killed.") + c).visible(), c);
		}
		for (const char *c : { "cust1", "cust2", "cust3" }) {
			CHECK_MESSAGE(s.at(clip(std::string("killed.") + c + ".txt")).text.empty(), c);
		}
		CHECK(s.at(clip("killed.seat")).text.empty());
		CHECK(s.at(clip("killed.weaponImage.imgHolder")).image == "ui/art/persistence/weapons/m16a4");
		auto &pair = *dynamic_cast<DogTagPairWidget *>(s.hud->controller(kKillTagPair));
		CHECK(pair.is_loaded());
		CHECK(pair.is_static());
		CHECK(s.at(pair.basic_tag().path() + ".ImageManager.imgHolder").image == kDefaultLeft);
		CHECK(s.at(pair.advanced_tag().path() + ".ImageManager.imgHolder").image == kDefaultRight);
		REQUIRE(pair.basic()->name_field().exists());
		CHECK(s.at(pair.basic()->name_field().path() + ".top_drop_text").text == kKiller);
		CHECK(pair.basic_tag().color_transform().ra == doctest::Approx(100.0));
		CHECK(pair.advanced_tag().color_transform().ra == doctest::Approx(100.0));
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}

	TEST_CASE("KillCam refresh: a new score re-lays the ME - FOE holder and a new kit re-labels the icon; a suicide "
			  "shows the suicide panel instead of the killer's, and bad luck writes BAD LUCK into it") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kKillScreenPartition, card_client(*r));
		HudModel m = footage_card();
		s.hud->initialize(m);
		const As2Clip holder = s.hud->host(kKillCam)->clip("killed.MeHimScoreHolder");
		const As2Clip kit_icon = s.hud->host(kKillCam)->clip("killed.kitIcon");
		const uint32_t engineer = kit_icon.current_frame();
		m.kill_card.player_killer_kills = 12;
		m.kill_card.killer_player_kills = 3;
		m.kill_card.killer_kit = 3;
		s.hud->refresh(m);
		CHECK(s.at(clip("killed.MeHimScoreHolder.score")).text == "12 - 3");
		CHECK(holder.x() == doctest::Approx(std::floor(1154.0f - (holder.width() + 10.0f) + 0.5f)));
		CHECK(s.at(clip("killed.kit.txt")).text == "RECON");
		CHECK(kit_icon.current_frame() != engineer);
		const As2Clip suicide(s.scene.get(), clip("suicide"));
		const std::string suicide_text = s.at(clip("suicide.text")).text;
		m.kill_card.suicide = true;
		s.hud->refresh(m);
		CHECK(suicide.visible());
		CHECK_FALSE(As2Clip(s.scene.get(), clip("killed")).visible());
		CHECK(s.at(clip("suicide.text")).text == suicide_text);
		m.kill_card.is_bad_luck = true;
		s.hud->refresh(m);
		CHECK(s.at(clip("suicide.text")).text ==
				retail_text_db(*r).find_sid("ID_M_BAD_LUCK").value_or("ID_M_BAD_LUCK"));
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
}

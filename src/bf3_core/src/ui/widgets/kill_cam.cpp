#include "dog_tag.h"
#include "hud_widget_support.h"
#include "image_manager.h"
#include "kit_icons.h"
#include "scroll_text.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace bf3::core::ui {

namespace {

constexpr float kKillCamGap = 10.0f;
constexpr double kKillCamScoreRight = 1154.0;
constexpr float kKillCamWeaponAfterStars = 55.0f;
constexpr double kKillCamSuicideByAdmin = 2.0;
constexpr std::string_view kKillCamByAdminSid = "ID_H_MSG_KILLS_BYADMIN";
constexpr std::string_view kKillCamBadLuckSid = "ID_M_BAD_LUCK";

class KillCamClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		sources_.attach(p_node, false, HudSourceFeed::Unlisted::FeedWhenNoneListed);
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		killed_ = r_host.clip("killed");
		kit_icon_ = killed_.child("kitIcon");
		weapon_ = killed_.child("weapon");
		seat_ = killed_.child("seat");
		for (size_t i = 0; i < cust_.size(); ++i) {
			cust_[i] = killed_.child("cust" + std::to_string(i + 1));
			cust_icon_[i] = killed_.child("custIcon" + std::to_string(i + 1));
		}
		is_pro_ = killed_.child("bfpro");
		suicide_ = r_host.clip("suicide");
		killed_by_ = killed_.child("killedBy");
		killed_by_name_ = killed_by_.child("name");
		killed_by_hdr_ = killed_by_.child("hdr");
		nemesis_ = killed_.child("killedByNemesis");
		nemesis_name_ = nemesis_.child("name");
		nemesis_nemesis_ = nemesis_.child("nemesis");
		nemesis_hdr_ = nemesis_.child("hdr");
		rank_ = killed_.child("rank");
		health_ = killed_.child("health");
		kit_ = killed_.child("kit");
		score_holder_ = killed_.child("MeHimScoreHolder");
		score_ = score_holder_.child("score");
		weapon_image_ = killed_.child("weaponImage");
		rank_image_ = killed_.child("rankImage");
		weapon_stars_ = killed_.child("weaponStars");
		suicide_pair_ = suicide_.child("tag_pair");
		killed_pair_ = killed_.child("kill_tag_pair");
		if (ImageManagerWidget *m = manager(r_host, rank_image_)) {
			m->resize_to_target = true;
			m->center_image = true;
		}
		const As2Clip health_hdr = killed_.child("healthHdr");
		health_.set_x(health_hdr.x() + health_hdr.text_width() + kKillCamGap);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		killed_.set_visible(false);
		suicide_.set_visible(false);
		kit_icon_.set_visible(false);
		weapon_.set_text("");
		seat_.set_text("");
		for (const As2Clip &c : cust_) {
			set_scroll_text(r_host, c, "");
		}
		for (const As2Clip &c : cust_icon_) {
			c.set_visible(false);
		}
		is_pro_.set_visible(false);
		for (DogTagPairWidget *pair : pairs(r_host)) {
			pair->initialize_pair(As2Value::undefined());
		}
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "DogtagBasic" || p_key == "DogtagAdvanced") {
			for (DogTagPairWidget *pair : pairs(r_host)) {
				if (p_key == "DogtagBasic") {
					pair->update_basic_data(p_value);
				} else {
					pair->update_advanced_data(p_value);
				}
			}
		} else if (p_key == "Suicide") {
			update_suicide(r_host, p_value);
		} else if (p_key == "BadLuck") {
			bad_luck_ = p_value.defined() && p_value.truthy();
			if (bad_luck_) {
				suicide_.child("text").set_text(r_host.translate(kKillCamBadLuckSid));
			}
		} else if (p_key == "IsPro") {
			is_pro_value_ = p_value.defined() && p_value.truthy();
			is_pro_.set_visible(is_pro_value_);
			if (is_pro_value_) {
				is_pro_.goto_and_play("_start");
			}
		} else if (p_key == "Name") {
			update_name(p_value);
		} else if (p_key == "Rank") {
			if (!p_value.defined()) {
				rank_.set_visible(false);
			} else if (rank_.exists()) {
				goto_and_stop_number(rank_, as2_number(p_value));
			}
		} else if (p_key == "Health") {
			health_.set_text(p_value.defined() ? p_value.to_string() + "%" : std::string());
		} else if (p_key == "Kit") {
			update_kit(r_host, p_value);
		} else if (p_key == "MyScore" || p_key == "HisScore") {
			if (!p_value.defined()) {
				score_.set_text("-");
				return;
			}
			(p_key == "MyScore" ? my_score_ : his_score_) = p_value.to_string();
			score_.set_text(my_score_ + " - " + his_score_);
			update_score_pos();
		} else if (p_key == "Weapon") {
			weapon_.set_text(as2_text(p_value));
		} else if (p_key == "Cust1" || p_key == "Cust2" || p_key == "Cust3") {
			update_cust_icon(r_host, size_t(p_key[4] - '1'), p_value);
		} else if (p_key == "Cust1Sid" || p_key == "Cust2Sid" || p_key == "Cust3Sid") {
			set_scroll_text(r_host, cust_[size_t(p_key[4] - '1')], as2_text(p_value));
		} else if (p_key == "IsNemesis") {
			nemesis_value_ = p_value.defined() && p_value.to_string() == "true";
			if (p_value.defined()) {
				killed_by_.set_visible(!nemesis_value_);
				nemesis_.set_visible(nemesis_value_);
			}
		} else if (p_key == "VehicleSeat") {
			seat_.set_text(as2_text(p_value));
		} else if (p_key == "WeaponImage" || p_key == "RankImage") {
			if (!p_value.defined() || p_value.to_string().empty()) {
				return;
			}
			load_image(r_host, p_key == "WeaponImage" ? weapon_image_ : rank_image_, p_value);
		} else if (p_key == "WeaponServiceStars") {
			if (!p_value.defined()) {
				weapon_stars_.set_visible(false);
				return;
			}
			if (ServiceStarsWidget *stars = weapon_stars_.exists()
							? controller_at<ServiceStarsWidget>(r_host, weapon_stars_)
							: nullptr) {
				stars->set_stars(as2_parse_int(p_value));
			}
			weapon_.set_x(weapon_stars_.x() + kKillCamWeaponAfterStars);
		} else if (p_key == "KitServiceStars") {
			if (!p_value.defined()) {
				return;
			}
			if (KitIconsWidget *kit = kit_icon_.exists() ? controller_at<KitIconsWidget>(r_host, kit_icon_) : nullptr) {
				kit->set_stars(as2_parse_int(p_value));
			}
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		const HudKillCard &k = p_model.kill_card;
		const auto feed = [&](std::string_view p_key, As2Value p_value) {
			sources_.feed(r_out, p_key, std::move(p_value));
		};
		const std::string killer = hud_string(k.killer_name);
		const auto localised = [&](uint32_t p_hash) {
			return p_hash != 0 && p_client.localise_hash ? p_client.localise_hash(p_hash) : std::string();
		};
		feed("Suicide", As2Value::of(k.suicide ? 1 : 0));
		feed("BadLuck", As2Value::of(k.is_bad_luck));
		feed("IsPro", As2Value::of(k.is_pro));
		feed("Name", As2Value::of(killer));
		feed("Rank", As2Value::of(k.killer_rank));
		feed("Health", As2Value::of(as2_round(double(k.killer_health))));
		if (k.killer_kit >= 0 && size_t(k.killer_kit) < kGameUtilsKits.size()) {
			feed("Kit", As2Value::of(std::string(kGameUtilsKits[size_t(k.killer_kit)].sid)));
		}
		feed("MyScore", As2Value::of(k.player_killer_kills));
		feed("HisScore", As2Value::of(k.killer_player_kills));
		feed("Weapon", As2Value::of(localised(k.killer_weapon_name)));
		feed("WeaponType", As2Value::of(k.killer_weapon_type));
		feed("WeaponImage", As2Value::of(hud_string(k.killer_weapon_image_path)));
		feed("RankImage", As2Value::of(hud_string(k.killer_rank_image_path)));
		feed("DogtagImage", As2Value::of(hud_string(k.killer_dogtag_image_path)));
		feed("WeaponServiceStars", As2Value::of(k.killer_weapon_service_stars));
		feed("KitServiceStars", As2Value::of(k.killer_kit_service_stars));
		As2Value basic = As2Value::object();
		basic.set("Image", As2Value::of(hud_string(k.killer_basic_dog_tag)));
		basic.set("PlayerName", As2Value::of(killer));
		feed("DogtagBasic", std::move(basic));
		As2Value advanced = As2Value::object();
		advanced.set("Image", As2Value::of(hud_string(k.killer_advanced_dog_tag)));
		feed("DogtagAdvanced", std::move(advanced));
	}

private:
	static ImageManagerWidget *manager(HudWidgetHost &r_host, const As2Clip &p_clip) {
		return p_clip.exists() ? controller_at<ImageManagerWidget>(r_host, p_clip) : nullptr;
	}

	static void load_image(HudWidgetHost &r_host, const As2Clip &p_clip, const As2Value &p_url) {
		ImageManagerWidget *m = manager(r_host, p_clip);
		HudWidgetHost *mh = m != nullptr ? r_host.screen().host(p_clip.path()) : nullptr;
		if (mh != nullptr) {
			m->update(*mh, "ImageUrl", p_url);
		}
	}

	static void set_scroll_text(HudWidgetHost &r_host, const As2Clip &p_clip, std::string_view p_text) {
		if (ScrollTextWidget *t = p_clip.exists() ? controller_at<ScrollTextWidget>(r_host, p_clip) : nullptr) {
			t->set_text(r_host.translate(p_text));
		}
	}

	std::vector<DogTagPairWidget *> pairs(HudWidgetHost &r_host) const {
		std::vector<DogTagPairWidget *> out;
		for (const As2Clip &p : { suicide_pair_, killed_pair_ }) {
			if (DogTagPairWidget *pair = p.exists() ? controller_at<DogTagPairWidget>(r_host, p) : nullptr) {
				out.push_back(pair);
			}
		}
		return out;
	}

	void update_suicide(HudWidgetHost &r_host, const As2Value &p_value) {
		if (!p_value.defined()) {
			suicide_.set_visible(false);
			suicide_value_ = 0.0;
			return;
		}
		suicide_value_ = p_value.to_number();
		killed_.set_visible(suicide_value_ == 0.0);
		suicide_.set_visible(!(suicide_value_ == 0.0));
		if (suicide_value_ == kKillCamSuicideByAdmin) {
			suicide_.child("text").set_text(r_host.translate(kKillCamByAdminSid));
		}
	}

	void update_name(const As2Value &p_value) {
		if (!p_value.defined()) {
			killed_by_name_.set_text("");
			nemesis_name_.set_text("");
			return;
		}
		const std::string name = p_value.to_string();
		killed_by_name_.set_text(name);
		nemesis_name_.set_text(name);
		killed_by_.set_visible(!nemesis_value_);
		nemesis_.set_visible(nemesis_value_);
		killed_by_name_.set_x(killed_by_hdr_.x() + killed_by_hdr_.text_width() + kKillCamGap);
		nemesis_nemesis_.set_x(nemesis_hdr_.x() + nemesis_hdr_.text_width() + kKillCamGap);
		nemesis_name_.set_x(nemesis_nemesis_.x() + nemesis_nemesis_.text_width() + kKillCamGap);
	}

	void update_kit(HudWidgetHost &r_host, const As2Value &p_value) {
		if (!p_value.defined()) {
			set_scroll_text(r_host, kit_, "");
			return;
		}
		const std::string kit = p_value.to_string();
		set_scroll_text(r_host, kit_, kit);
		const std::string label = game_utils_kit_frame_label(kit);
		if (!label.empty()) {
			kit_icon_.goto_and_stop(label);
		}
		kit_icon_.set_visible(true);
	}

	void update_cust_icon(HudWidgetHost &r_host, size_t p_index, const As2Value &p_value) {
		const As2Clip &icon = cust_icon_[p_index];
		if (!p_value.defined() || !game_utils_is_image_path(p_value)) {
			icon.set_visible(false);
			return;
		}
		load_image(r_host, icon, p_value);
		icon.set_visible(true);
	}

	void update_score_pos() const {
		const As2Clip me = score_holder_.child("meHdr");
		const As2Clip him = score_holder_.child("himHdr");
		me.set_width(float(as2_round(double(me.text_width()) + 5.0)));
		score_.set_width(float(as2_round(double(score_.text_width()) + 20.0)));
		score_.set_x(me.width() - 3.0f);
		him.set_width(float(as2_round(double(him.text_width()) + 5.0)));
		him.set_x(score_.x() + score_.width());
		score_holder_.set_x(
				float(as2_round(kKillCamScoreRight - (double(score_holder_.width()) + double(kKillCamGap)))));
	}

	HudSourceFeed sources_;
	As2Clip killed_;
	As2Clip kit_icon_;
	As2Clip weapon_;
	As2Clip seat_;
	std::array<As2Clip, 3> cust_;
	std::array<As2Clip, 3> cust_icon_;
	As2Clip is_pro_;
	As2Clip suicide_;
	As2Clip killed_by_;
	As2Clip killed_by_name_;
	As2Clip killed_by_hdr_;
	As2Clip nemesis_;
	As2Clip nemesis_name_;
	As2Clip nemesis_nemesis_;
	As2Clip nemesis_hdr_;
	As2Clip rank_;
	As2Clip health_;
	As2Clip kit_;
	As2Clip score_holder_;
	As2Clip score_;
	As2Clip weapon_image_;
	As2Clip rank_image_;
	As2Clip weapon_stars_;
	As2Clip suicide_pair_;
	As2Clip killed_pair_;
	std::string my_score_;
	std::string his_score_;
	double suicide_value_ = 0.0;
	bool nemesis_value_ = false;
	bool bad_luck_ = false;
	bool is_pro_value_ = false;
};

}

void register_hud_widgets_kill_cam(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.ScreenSpecific.KillCam", [] { return std::make_unique<KillCamClass>(); });
}

}

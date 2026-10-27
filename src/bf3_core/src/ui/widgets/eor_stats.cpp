#include "hud_widget_support.h"
#include "image_manager.h"
#include "scroll_text.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace bf3::core::ui {

namespace {

constexpr double kStatsDimAlpha = 50.0;
constexpr float kStatsNameWidth = 318.0f;
constexpr std::string_view kStatsUnset = "?";
constexpr std::string_view kStatsZero = "-";

std::string format_hundredths(const As2Value &p_value) {
	const double v = std::floor(p_value.to_number() * 100.0 + 0.5) / 100.0;
	if (!std::isfinite(v)) {
		return As2Value::of(v).to_string();
	}
	char text[48];
	std::snprintf(text, sizeof(text), "%.2f", v);
	return text;
}

class StatsClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		sources_.clear();
		for (const HudDataSource &s : p_node.sources) {
			if (!s.category.empty()) {
				sources_[s.name] = s.on_initialize;
			}
		}
	}

	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		const HudEndOfRound &e = p_model.end_of_round;
		const auto feed = [&](std::string_view p_key, As2Value p_value) {
			if (const auto it = sources_.find(p_key); it != sources_.end()) {
				r_out.push_back({ std::string(p_key), std::move(p_value), it->second });
			}
		};
		const auto best = [&](const HudBestWeapon &p_best) {
			As2Value v = As2Value::object();
			v.set("Name",
					As2Value::of(p_best.name_sid_hash != 0 && p_client.localise_hash
									? p_client.localise_hash(p_best.name_sid_hash)
									: std::string()));
			v.set("Kills", As2Value::of(p_best.kills));
			v.set("IsSet", As2Value::of(p_best.kills > 0 ? 1 : 0));
			if (p_best.kills > 0) {
				v.set("Image", As2Value::of(hud_string(p_best.image_path)));
			}
			return v;
		};
		feed("TotalScore", As2Value::of(e.total_score));
		feed("CombatScore", As2Value::of(e.combat_score));
		feed("AwardScore", As2Value::of(0));
		feed("Kills", As2Value::of(e.kills));
		feed("Deaths", As2Value::of(e.deaths));
		feed("KillDeathRatio", As2Value::of(double(e.kill_death_ratio)));
		feed("BestWeaponInfo", best(e.best_weapon_info));
		feed("BestVehicleInfo", best(e.best_vehicle_info));
		feed("KillsPerMinute", As2Value::of(double(e.kills_per_minute)));
		feed("HighestStreak", As2Value::of(e.longest_streak));
		feed("HighestNemesisStreak", As2Value::of(e.longest_nemesis_streak));
		feed("DogtagsTaken",
				e.dogtags_taken > 0 ? As2Value::of(e.dogtags_taken) : As2Value::of(std::string(kStatsZero)));
		feed("LongestHeadshot", As2Value::of(hud_eor_distance(e.longest_headshot)));
		feed("TotalPlaytime", As2Value::of(hud_eor_play_time(e.total_playtime)));
		feed("ScorePerMinute", As2Value::of(double(e.score_per_minute)));
		feed("Name", As2Value::of(hud_string(e.name)));
		feed("IsMatch", As2Value::of(false));
		feed("IsRanked", As2Value::of(e.is_ranked));
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const auto value = [&](std::string_view p_key) {
			const As2Value *v = p_init.value(p_key);
			return v != nullptr ? *v : As2Value();
		};
		const bool end_of_round = param_true(r_host, "endOfRoundMode");
		const As2Value weapon = value("BestWeaponInfo");
		const As2Value vehicle = value("BestVehicleInfo");
		fill_header_row(r_host, "weaponHeader", "ID_M_BEST_WEAPON");
		set_scroll_text(r_host, "weaponNameText",
				weapon.field("Name").defined() ? weapon.field("Name").to_string() : std::string());
		fill_row(r_host, "weaponKills", As2Value::of("ID_M_KILLS"), weapon.field("Kills"));
		fill_header_row(r_host, "VehicleHeader", "ID_M_BEST_VEHICLE");
		set_scroll_text(r_host, "vehicleNameText",
				vehicle.field("Name").defined() ? vehicle.field("Name").to_string() : std::string());
		fill_row(r_host, "VehicleKills", As2Value::of("ID_M_KILLS"), vehicle.field("Kills"));
		const bool weapon_set = weapon.field("IsSet").to_number() > 0.0;
		const bool vehicle_set = vehicle.field("IsSet").to_number() > 0.0;
		for (const std::string_view c : { "weaponHeader", "vehicleNameText", "weaponKills", "weaponBg" }) {
			r_host.clip(c).set_alpha(float(weapon_set ? 100.0 : kStatsDimAlpha));
		}
		for (const std::string_view c : { "VehicleHeader", "vehicleNameText", "VehicleKills", "VehicleBg" }) {
			r_host.clip(c).set_alpha(float(vehicle_set ? 100.0 : kStatsDimAlpha));
		}
		const As2Value weapon_image = weapon_set ? image_or_empty(weapon.field("Image")) : As2Value();
		const As2Value vehicle_image = vehicle_set ? image_or_empty(vehicle.field("Image")) : As2Value();
		setup_image(r_host, "weaponImage", weapon_image);
		setup_image(r_host, "vehicleImage", vehicle_image);
		fill_row(r_host, "kills_field", As2Value::of("ID_M_KILLS"), value("Kills"));
		fill_row(r_host, "deaths_field", As2Value::of("ID_M_DEATHS"), value("Deaths"));
		std::string kd = "ID_M_KD_RATIO";
		if (!end_of_round) {
			kd = r_host.translate(kd) + " *";
		}
		fill_row(r_host, "kdRatio_field", As2Value::of(kd), As2Value::of(format_hundredths(value("KillDeathRatio"))));
		fill_row(r_host, "KPM_field", As2Value::of("ID_M_KDM"),
				As2Value::of(format_hundredths(value("KillsPerMinute"))));
		fill_row(r_host, "HighestStreak_field", As2Value::of("ID_M_HIGHEST_STREAK"), value("HighestStreak"));
		fill_row(r_host, "HighestNemesisStreak_field", As2Value::of("ID_M_HIGHEST_NEMESIS_STREAK"),
				value("HighestNemesisStreak"));
		fill_row(r_host, "DogtagsTaken_field", As2Value::of("ID_M_DOGTAGS_TAKEN"), value("DogtagsTaken"));
		fill_row(r_host, "LongestHeadShot_field", As2Value::of("ID_M_LONGEST_HEADSHOT"), value("LongestHeadshot"));
		if (value("IsMatch").truthy()) {
			const As2Value match = value("MatchStats");
			fill_row(r_host, "totalPlayTime_field", As2Value::of("ID_M_MATCH_PLAY_TIME"), match.field("RoundDuration"));
			fill_row(r_host, "awardScore_field", As2Value::of("ID_M_AWARD_SCORE"), As2Value::of("0"));
			fill_row(r_host, "totalScore_field", As2Value::of("ID_M_MATCH_SCORE"), match.field("RoundScore"));
			fill_row(r_host, "SPM_field", As2Value::of("ID_M_SCORE_PER_MINUTE"),
					As2Value::of(As2Value::of(as2_round(match.field("RoundScorePerMinute").to_number())).to_string()));
			r_host.clip("statsSince_field").set_visible(false);
		} else {
			fill_row(r_host, "totalPlayTime_field", As2Value::of("ID_M_TOTAL_PLAY_TIME"), value("TotalPlaytime"));
			fill_row(r_host, "awardScore_field", As2Value::of("ID_M_AWARD_SCORE"), value("AwardScore"));
			fill_row(r_host, "totalScore_field", As2Value::of("ID_M_TOTAL_SCORE"), value("TotalScore"));
			r_host.clip("statsSince_field").set_visible(!end_of_round);
			std::string spm = r_host.translate("ID_M_SCORE_PER_MINUTE");
			if (!end_of_round) {
				spm += " *";
				fill_row(r_host, "statsSince_field", As2Value::of("ID_M_STATS_SINCE"), value("StatsSince"));
			}
			fill_row(r_host, "SPM_field", As2Value::of(spm),
					As2Value::of(As2Value::of(as2_round(value("ScorePerMinute").to_number())).to_string()));
		}
		r_host.clip("playerNameText").set_text(value("Name").to_string());
		r_host.clip("rankNameText").set_text(value("RankName").to_string());
		if (!value("IsRanked").truthy()) {
			for (const std::string_view c : { "bar", "rankProgressText", "playerNameText", "rankNameText",
						 "CurrentRank_img", "NextRank_img", "barBackground" }) {
				r_host.clip(c).set_visible(false);
			}
		}
		align_texts(r_host);
		r_host.self().set_visible(true);
	}

private:
	static As2Value image_or_empty(const As2Value &p_image) { return p_image.defined() ? p_image : As2Value::of(""); }

	static void fill_row(
			HudWidgetHost &r_host, std::string_view p_row, const As2Value &p_left, const As2Value &p_right) {
		const As2Clip row = r_host.clip(p_row);
		row.child("leftText")
				.set_text(p_left.defined() ? r_host.translate(p_left.to_string()) : std::string(kStatsUnset));
		row.child("rightText").set_text(p_right.defined() ? p_right.to_string() : std::string(kStatsUnset));
	}

	static void fill_header_row(HudWidgetHost &r_host, std::string_view p_row, std::string_view p_header) {
		r_host.clip(p_row).child("leftText").set_text(r_host.translate(p_header));
	}

	static ScrollTextWidget *scroll_text(HudWidgetHost &r_host, std::string_view p_clip) {
		const As2Clip c = r_host.clip(p_clip);
		return c.exists() ? controller_at<ScrollTextWidget>(r_host, c) : nullptr;
	}

	static void set_scroll_text(HudWidgetHost &r_host, std::string_view p_clip, std::string_view p_text) {
		if (ScrollTextWidget *t = scroll_text(r_host, p_clip)) {
			t->set_text(p_text);
		}
	}

	static void setup_image(HudWidgetHost &r_host, std::string_view p_clip, const As2Value &p_url) {
		if (!p_url.defined()) {
			return;
		}
		const As2Clip c = r_host.clip(p_clip);
		ImageManagerWidget *m = c.exists() ? controller_at<ImageManagerWidget>(r_host, c) : nullptr;
		HudWidgetHost *mh = m != nullptr ? r_host.screen().host(c.path()) : nullptr;
		if (mh == nullptr) {
			return;
		}
		m->initialize(*mh, HudWidgetInit{});
		m->center_image = true;
		m->update(*mh, "ImageUrl", p_url);
	}

	static void align_texts(HudWidgetHost &r_host) {
		for (const auto &[header, name] :
				{ std::pair<std::string_view, std::string_view>{ "weaponHeader", "weaponNameText" },
						std::pair<std::string_view, std::string_view>{ "VehicleHeader", "vehicleNameText" } }) {
			if (ScrollTextWidget *t = scroll_text(r_host, name)) {
				t->set_width(kStatsNameWidth - r_host.clip(header).child("leftText").text_width());
			}
		}
	}

	std::map<std::string, bool, std::less<>> sources_;
};

}

std::string hud_eor_play_time(float p_seconds) {
	const int64_t s = std::isfinite(p_seconds) && p_seconds > 0.0f ? int64_t(p_seconds) : 0;
	char text[48];
	std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", static_cast<long long>(s / 3600),
			static_cast<long long>(s / 60 % 60), static_cast<long long>(s % 60));
	return text;
}

std::string hud_eor_distance(float p_metres) {
	if (!(p_metres > 0.0f) || !std::isfinite(p_metres)) {
		return std::string(kStatsZero);
	}
	return As2Value::of(as2_round(double(p_metres))).to_string() + "m";
}

void register_hud_widgets_eor_stats(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Stats.Stats", [] { return std::make_unique<StatsClass>(); });
}

}

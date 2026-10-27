#include "hud_widget_support.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace bf3::core::ui {

namespace {

constexpr uint32_t kEorFriendlyTextColour = 7586815u;
constexpr uint32_t kEorEnemyTextColour = 9843250u;
constexpr float kEorValueGap = 6.0f;
constexpr float kEorPairGap = 12.0f;
constexpr double kEorStageCentre = 640.0;
constexpr std::string_view kEorGameModeCtf = "CTF0";
constexpr std::string_view kEorTemplatePrefix = "ID_M_EOR_TEMPLATE_";
constexpr std::string_view kEorSidPrefix = "ID_M_EOR_";

struct EorKey {
	std::string sid;
	std::string arg;
};

std::string eor_format(const HudWidgetHost &p_host, std::string_view p_sid, std::string_view p_arg) {
	std::string text = p_host.translate(p_sid);
	const size_t at = text.find("{0:s}");
	if (at != std::string::npos) {
		text.replace(at, 5, p_arg);
	}
	return text;
}

class EorHeaderValuePairClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override { host_ = &r_host; }

	void setup_texts(const EorKey &p_key, const As2Value &p_value, bool p_active) const {
		const As2Clip key = host_->clip("keyText");
		const As2Clip value = host_->clip("valueText");
		key.set_auto_size(true);
		value.set_auto_size(true);
		key.set_text(eor_format(*host_, p_key.sid, p_key.arg) + ":");
		value.set_text(p_value.to_string());
		value.set_x(as2_autosized_width(key) + kEorValueGap);
		const uint32_t colour = p_active ? kEorFriendlyTextColour : kEorEnemyTextColour;
		set_color(key, colour);
		set_color(value, colour);
		host_->self().set_visible(true);
	}

private:
	static void set_color(const As2Clip &p_target, uint32_t p_colour) {
		p_target.set_text_color(p_colour);
		p_target.set_filters({ as2_glow_filter(p_colour, 100.0, 5.0f, 5.0f, 1.0f, 3) });
	}

	HudWidgetHost *host_ = nullptr;
};

class EorHeaderClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		sources_.clear();
		for (const HudDataSource &s : p_node.sources) {
			if (!s.category.empty()) {
				sources_[s.name] = s.on_initialize;
			}
		}
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.self().set_visible(false);
		text_holder_ = r_host.clip("textHolder");
		duration_desc_ = text_holder_.child("durationDescText");
		duration_ = text_holder_.child("durationText");
		duration_desc_.set_auto_size(true);
		duration_.set_auto_size(true);
	}

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		const HudEndOfRound &e = p_model.end_of_round;
		const auto feed = [&](std::string_view p_key, As2Value p_value) {
			if (const auto it = sources_.find(p_key); it != sources_.end()) {
				r_out.push_back({ std::string(p_key), std::move(p_value), it->second });
			}
		};
		feed("GameMode", As2Value::of(hud_string(p_model.settings.current_game_mode)));
		feed("IsWinner", As2Value::of(hud_eor_place(e)));
		As2Value results = As2Value::array();
		for (const int32_t kills : e.team_results) {
			As2Value r = As2Value::object();
			r.set("TeamName", As2Value::of(""));
			r.set("Value", As2Value::of(kills));
			results.push(std::move(r));
		}
		feed("TeamResults", std::move(results));
		feed("TeamNumber", As2Value::of(e.player_team_index - 1));
		feed("RoundDuration", As2Value::of(hud_eor_round_duration(e.round_duration)));
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const auto value = [&](std::string_view p_key) {
			const As2Value *v = p_init.value(p_key);
			return v != nullptr ? *v : As2Value();
		};
		const As2Value mode = value("GameMode");
		const As2Value is_winner = value("IsWinner");
		const bool won = as2_loose_equals(is_winner, As2Value::of(0));
		const As2Clip header = r_host.clip("headerText");
		if (as2_game_mode_is(mode, "SquadDeathMatch") || as2_game_mode_is(mode, "SquadRush")) {
			header.set_text(r_host.translate(won ? "ID_M_EOR_SQUADWON" : "ID_M_EOR_SQUADLOST"));
		} else if (as2_game_mode_is(mode, "GunMaster")) {
			const double place = is_winner.to_number() + 1.0;
			if (place == 1.0) {
				header.set_text(r_host.translate("ID_M_EOR_YOUWON"));
			} else if (place == 2.0) {
				header.set_text(r_host.translate("ID_M_EOR_FINISHEDSECOND"));
			} else if (place == 3.0) {
				header.set_text(r_host.translate("ID_M_EOR_FINISHEDTHIRD"));
			} else if (place == 4.0 || place == 5.0) {
				header.set_text(r_host.translate("ID_M_EOR_FINISHEDTOPFIVE"));
			} else {
				header.set_text(eor_format(r_host, "ID_M_EOR_FINISHEDAS", As2Value::of(place).to_string()));
			}
		} else {
			header.set_text(r_host.translate(won ? "ID_M_EOR_TEAMWON" : "ID_M_EOR_TEAMLOST"));
		}
		const As2Value results = value("TeamResults");
		const As2Value team = value("TeamNumber");
		const auto team_name = [&](size_t p_index) { return results.at(p_index).field("TeamName"); };
		if (as2_game_mode_is(mode, "Conquest") || as2_game_mode_is(mode, "Domination") ||
				as2_loose_equals(mode, As2Value::of(std::string(kEorGameModeCtf))) ||
				as2_game_mode_is(mode, "Scavenger")) {
			set_round_score(r_host,
					{ match_stats_sid(team_name(0), "US", "TICKETS"), match_stats_sid(team_name(1), "RU", "TICKETS") },
					results, team);
		} else if (as2_game_mode_is(mode, "Rush") || as2_game_mode_is(mode, "SquadRush")) {
			set_round_score(r_host,
					{ match_stats_sid(team_name(0), "ATTACKERS", "TICKETS"), EorKey{ "ID_M_EOR_MCOMS_REMAINING", {} } },
					results, team);
		} else if (as2_game_mode_is(mode, "TeamDeathMatch")) {
			set_round_score(r_host,
					{ match_stats_sid(team_name(0), "US", "KILLS"), match_stats_sid(team_name(1), "RU", "KILLS") },
					results, team);
		} else if (as2_game_mode_is(mode, "SquadDeathMatch")) {
			set_round_score(r_host,
					{ EorKey{ "ID_H_SDM_TEAM_0_SHORT", {} }, EorKey{ "ID_H_SDM_TEAM_1_SHORT", {} },
							EorKey{ "ID_H_SDM_TEAM_2_SHORT", {} }, EorKey{ "ID_H_SDM_TEAM_3_SHORT", {} } },
					results, team);
		}
		duration_.set_x(duration_desc_.x() + as2_autosized_width(duration_desc_) + kEorValueGap);
		const As2Value duration = value("RoundDuration");
		if (duration.defined() && !as2_loose_equals(duration, As2Value::of("")) &&
				!as2_loose_equals(duration, As2Value::of(0))) {
			duration_.set_text(duration.to_string());
			duration_desc_.set_text(r_host.translate("ID_M_EOR_ROUNDDURATION"));
		} else {
			duration_.set_visible(false);
			duration_desc_.set_visible(false);
		}
		text_holder_.set_x(float(as2_round(kEorStageCentre - double(holder_width()) / 2.0)));
		r_host.self().set_visible(true);
	}

	const std::vector<As2Clip> &pairs() const { return pairs_; }
	const As2Clip &text_holder() const { return text_holder_; }

private:
	static EorKey match_stats_sid(const As2Value &p_name, std::string_view p_team, std::string_view p_suffix) {
		if (p_name.defined() && !as2_loose_equals(p_name, As2Value::of(""))) {
			return EorKey{ std::string(kEorTemplatePrefix) + std::string(p_suffix), p_name.to_string() };
		}
		return EorKey{ std::string(kEorSidPrefix) + std::string(p_team) + "_" + std::string(p_suffix), {} };
	}

	float holder_width() const {
		float right = 0.0f;
		const auto extend = [&](const As2Clip &p_clip, float p_origin) {
			right = std::max(right, p_origin + p_clip.x() + as2_autosized_width(p_clip));
		};
		extend(duration_desc_, 0.0f);
		extend(duration_, 0.0f);
		for (const As2Clip &p : pairs_) {
			extend(p.child("keyText"), p.x());
			extend(p.child("valueText"), p.x());
		}
		const float left = std::min(duration_desc_.x(), duration_.x());
		return right - left;
	}

	void set_round_score(HudWidgetHost &r_host, const std::vector<EorKey> &p_keys, const As2Value &p_values,
			const As2Value &p_team) {
		if (!p_values.defined()) {
			return;
		}
		float x = holder_width() + kEorValueGap;
		for (size_t i = 0; i < p_keys.size(); ++i) {
			const As2Clip pair = r_host.attach_movie(text_holder_, "EorHeaderValuePair", "values_" + std::to_string(i),
					text_holder_.next_highest_depth());
			if (const EorHeaderValuePairClass *c =
							pair.exists() ? controller_at<EorHeaderValuePairClass>(r_host, pair) : nullptr) {
				c->setup_texts(
						p_keys[i], p_values.at(i).field("Value"), as2_loose_equals(p_team, As2Value::of(int32_t(i))));
			}
			pair.set_x(x);
			pairs_.push_back(pair);
			float w = 0.0f;
			for (const std::string_view f : { "keyText", "valueText" }) {
				w = std::max(w, pair.child(f).x() + as2_autosized_width(pair.child(f)));
			}
			x += w + kEorPairGap;
		}
	}

	std::map<std::string, bool, std::less<>> sources_;
	As2Clip text_holder_;
	As2Clip duration_desc_;
	As2Clip duration_;
	std::vector<As2Clip> pairs_;
};

}

int32_t hud_eor_place(const HudEndOfRound &p_eor) {
	return p_eor.local_player_in_winning_team ? 0 : 1;
}

std::string hud_eor_round_duration(float p_seconds) {
	if (!(p_seconds > 0.0f)) {
		return std::string();
	}
	const int64_t s = int64_t(p_seconds);
	char text[32];
	std::snprintf(text, sizeof(text), "%02lld:%02lld", static_cast<long long>(s / 60), static_cast<long long>(s % 60));
	return text;
}

int32_t hud_eor_seconds_left(float p_seconds) {
	return p_seconds > 0.0f && std::isfinite(p_seconds) ? int32_t(std::floor(p_seconds)) : 0;
}

void register_hud_widgets_eor_header(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Eor.EorHeader", [] { return std::make_unique<EorHeaderClass>(); });
	r_registry.add("Widget.Eor.EorHeaderValuePair", [] { return std::make_unique<EorHeaderValuePairClass>(); });
}

}

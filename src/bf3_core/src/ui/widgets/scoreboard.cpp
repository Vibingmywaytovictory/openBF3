#include "list.h"

namespace bf3::core::ui {

namespace {

constexpr uint32_t kRowTextGlow = 0x73C3FFu;
constexpr uint32_t kWhite = 0xFFFFFFu;
constexpr std::string_view kReadySid = "ID_M_SCOREBOARD_READY";

bool same(const As2Value &p_a, const As2Value &p_b) {
	if (p_a.kind == As2Value::Kind::Undefined || p_b.kind == As2Value::Kind::Undefined) {
		return p_a.kind == p_b.kind;
	}
	if (p_a.kind == As2Value::Kind::String && p_b.kind == As2Value::Kind::String) {
		return p_a.text == p_b.text;
	}
	return p_a.to_number() == p_b.to_number();
}

std::string dash_if_undefined(const As2Value &p_v, std::string_view p_dash) {
	const std::string s = p_v.to_string();
	return s == "undefined" ? std::string(p_dash) : s;
}

class ScoreBoardRowClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		register_state_callback(r_host.path());
		if (hud_platform_win32(r_host.client())) {
			return;
		}
		const float ping = r_host.clip("PingLabel").width();
		for (const std::string_view name : { "KillsLabel", "DeathsLabel", "ScoreLabel" }) {
			const As2Clip c = r_host.clip(name);
			c.set_x(c.x() + ping);
		}
		r_host.clip("PingLabel").set_visible(false);
	}

	void set_data(const As2Value &p_data) override {
		const As2Value &da = p_data.field("useDirectAccess");
		if (da.kind == As2Value::Kind::Bool ? da.boolean : same(da, As2Value::of(true))) {
			set_text("Label", "-");
			set_text("NumLabel", As2Value::of(row_index() + 1.0).to_string());
			set_text("KillsLabel", "-");
			set_text("DeathsLabel", "-");
			set_text("ScoreLabel", "-");
			host_->clip("voipIcon").goto_and_stop(0);
			set_text("PingLabel", "");
			set_local_squad(As2Value::of(0));
			set_ready_state(As2Value::of(false));
			setup_direct_access();
		} else {
			set_text("Label", p_data.field("Label").to_string());
			set_text("NumLabel", As2Value::of(row_index()).to_string());
			set_text("KillsLabel", dash_if_undefined(p_data.field("Kills"), "-"));
			set_text("DeathsLabel", dash_if_undefined(p_data.field("Deaths"), "-"));
			set_text("ScoreLabel", dash_if_undefined(p_data.field("Score"), "-"));
			const As2Value &voip = p_data.field("Voip");
			if (voip.kind == As2Value::Kind::String) {
				host_->clip("voipIcon").goto_and_stop(std::string_view(voip.text));
			} else {
				goto_and_stop_number(host_->clip("voipIcon"), voip.to_number());
			}
			set_text("PingLabel", dash_if_undefined(p_data.field("Ping"), ""));
			set_ready_state(p_data.field("MatchReady"));
			set_local_squad(As2Value::of(p_data.field("LocalSquad").to_number()));
		}
		set_mouse_enabled(true);
		RowBaseClass::set_data(p_data);
	}

	As2Value label() const override { return As2Value::of(host_->clip("Label").text()); }

	bool local_player() const { return local_player_.truthy(); }

protected:
	void on_state(const std::string &p_state) override {
		if (local_player_.truthy()) {
			host_->clip("bg").goto_and_stop(std::string_view(p_state + "Player"));
		} else if (local_squad_.truthy()) {
			host_->clip("bg").goto_and_stop(std::string_view(p_state + "Squad"));
		}
		const bool inverted = p_state == "selected" || p_state == "overSelected" || p_state == "press" ||
				p_state == "pressSelected" || local_player_.truthy();
		for (const std::string_view name :
				{ "NumLabel", "Label", "KillsLabel", "DeathsLabel", "ScoreLabel", "PingLabel", "ReadyLabel" }) {
			set_text_color(host_->clip(name), inverted ? 0u : kWhite);
		}
		set_ready_state(match_ready_);
	}

	void member(std::string_view p_name, const As2Value &p_value) override {
		if (p_name == "isLocalSquad") {
			set_local_squad(p_value);
		} else if (p_name == "isLocalPlayer") {
			if (!same(p_value, local_player_)) {
				local_player_ = p_value;
				state_changed();
			}
		} else if (p_name == "isMatchReady") {
			match_ready_ = p_value;
			state_changed();
		} else {
			RowBaseClass::member(p_name, p_value);
		}
	}

private:
	void set_text(std::string_view p_name, std::string_view p_text) const { host_->clip(p_name).set_text(p_text); }

	void set_local_squad(const As2Value &p_value) {
		if (same(p_value, local_squad_)) {
			return;
		}
		local_squad_ = p_value;
		state_changed();
	}

	static void set_text_color(const As2Clip &p_label, uint32_t p_color) {
		if (!p_label.exists()) {
			return;
		}
		p_label.set_text_color(p_color);
		const bool black = p_color == 0u;
		p_label.set_filters({ as2_glow_filter(black ? 0u : kRowTextGlow, 1.0, 8.0f, 8.0f, black ? 0.75f : 1.0f, 1) });
	}

	void set_ready_state(const As2Value &p_state) const {
		const As2Clip ready = host_->clip("ReadyLabel");
		if (p_state.defined() && p_state.truthy()) {
			ready.set_visible(true);
			ready.set_text(host_->translate(kReadySid));
		} else {
			ready.set_visible(false);
		}
	}

	void setup_direct_access() {
		const std::string &self = host_->path();
		const auto at = [&](std::string_view p_name) { return host_->clip(p_name).path(); };
		register_direct_access_array({
				ListAccessItem{ "Label", "", UiDirectAccessOp::Text, at("Label") },
				ListAccessItem{ "Rank", "", UiDirectAccessOp::GotoAndStop, at("rankIcon") },
				ListAccessItem{ "Voip", "", UiDirectAccessOp::GotoAndStop, at("voipIcon") },
				ListAccessItem{ "Kills", "", UiDirectAccessOp::Text, at("KillsLabel") },
				ListAccessItem{ "Deaths", "", UiDirectAccessOp::Text, at("DeathsLabel") },
				ListAccessItem{ "Score", "", UiDirectAccessOp::Text, at("ScoreLabel") },
				ListAccessItem{ "Ping", "", UiDirectAccessOp::Text, at("PingLabel") },
				ListAccessItem{ "LocalSquad", "isLocalSquad", UiDirectAccessOp::Member, self },
				ListAccessItem{ "LocalPlayer", "isLocalPlayer", UiDirectAccessOp::Member, self },
				ListAccessItem{ "MatchReady", "isMatchReady", UiDirectAccessOp::Member, self },
		});
	}

	As2Value local_squad_ = As2Value::of(false);
	As2Value local_player_ = As2Value::of(false);
	As2Value match_ready_ = As2Value::of(false);
};

}

void register_hud_widgets_scoreboard(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.Rows.ScoreBoardRow", [] { return std::make_unique<ScoreBoardRowClass>(); });
}

}

#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class SquadListClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		for (int32_t i = 0; i < kRows; ++i) {
			row(r_host, i).set_visible(false);
		}
		r_host.renderer(r_host.self(), "t2");
		r_host.self().set_visible(false);
		const std::optional<uint32_t> slot0 = nametag_colour_slot0_as_authored_txt(r_host);
		if (!slot0) {
			return;
		}
		for (int32_t i = 0; i < kRows; ++i) {
			const As2Clip r = row(r_host, i);
			r.child("label").child("txt").set_text_color(*slot0);
			for (const char *icon : { "kit", "leader", "voip", "booster" }) {
				r.child(icon).set_rgb(*slot0);
			}
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != "SquadList") {
			return;
		}
		bool any_booster = false;
		for (int32_t i = 0; i < kRows; ++i) {
			const As2Value &m = p_value.at(size_t(i));
			const As2Clip r = row(r_host, i);
			if (!(m.truthy() && p_value.size() > 1)) {
				r.set_visible(false);
				continue;
			}
			const As2Clip label = r.child("label");
			const std::string text = m.field("Label").to_string();
			label.child("txt").set_text(text);
			label.child("txtBg").set_text(text);
			if (m.field("Dead").truthy()) {
				r.set_alpha(50.0f);
				r.child("kit").goto_and_stop(kUiHudIconDead);
			} else {
				r.set_alpha(100.0f);
				r.child("kit").goto_and_stop(int32_t(m.field("Kit").to_number()));
			}
			if (m.field("SquadLeader").truthy()) {
				label.set_x(70.0f);
				r.child("leader").set_x(60.0f);
				r.child("leader").goto_and_stop(kUiHudIconSquadLeader);
				r.child("leader").set_visible(true);
			} else {
				label.set_x(48.0f);
				r.child("leader").set_visible(false);
			}
			r.child("voip").goto_and_stop(int32_t(m.field("Voip").to_number()));
			r.child("voip").set_x(label.x() + label.child("txt").text_width() + 15.0f);
			const double booster = m.field("BoosterIcon").to_number();
			if (booster != 0.0) {
				r.child("booster").set_visible(true);
				any_booster = true;
			}
			r.child("booster").goto_and_stop(int32_t(booster));
			if (r.child("booster").child("icon").exists()) {
				r.child("booster").child("icon").goto_and_stop(2);
			}
			r.set_visible(true);
		}
		if (any_booster) {
			return;
		}
		for (int32_t i = 0; i < kRows; ++i) {
			const As2Clip r = row(r_host, i);
			const As2Clip label = r.child("label");
			if (r.child("leader").visible()) {
				label.set_x(50.0f);
				r.child("leader").set_x(40.0f);
			} else {
				label.set_x(28.0f);
			}
			r.child("voip").set_x(label.x() + label.child("txt").text_width() + 15.0f);
		}
	}

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		const HudSquadMemberList &members = p_model.squad.local_squad_members;
		As2Value list = As2Value::array();
		for (size_t i = 0; i < std::min<size_t>(members.count, kHudSquadSize); ++i) {
			const uint8_t slot = members.index[i];
			if (slot >= kHudMaxPlayers) {
				continue;
			}
			const HudPlayer &p = p_model.players[slot];
			As2Value m = As2Value::object();
			m.set("Label", As2Value::of(hud_player_name(p_model, slot)));
			m.set("Dead", As2Value::of(!p.alive));
			m.set("Kit", As2Value::of(int32_t(p.kit_icon)));
			m.set("SquadLeader", As2Value::of(slot == members.leader));
			m.set("Voip", As2Value::of(int32_t(0)));
			m.set("BoosterIcon", As2Value::of(int32_t(p.booster_icon)));
			list.push(std::move(m));
		}
		r_out.push_back({ "SquadList", std::move(list) });
	}

private:
	static constexpr int32_t kRows = 4;

	static As2Clip row(HudWidgetHost &r_host, int32_t p_index) { return r_host.clip("squadMember" + std::to_string(p_index)); }

	static std::optional<uint32_t> nametag_colour_slot0_as_authored_txt(HudWidgetHost &r_host) {
		return row(r_host, 0).child("label").child("txt").text_color();
	}
};

}

void register_hud_widgets_squad_list(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.SquadList", [] { return std::make_unique<SquadListClass>(); });
}

}

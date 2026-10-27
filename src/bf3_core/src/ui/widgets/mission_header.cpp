#include "hud_widget_support.h"
#include "scroll_text.h"

#include "bf3/core/ui/ui_graph.h"

namespace bf3::core::ui {

namespace {

enum class HeaderText : uint8_t {
	GameModeName,
	LevelName,
	FriendlyTeamName,
};

struct HeaderSource {
	std::string_view category;
	int32_t key = 0;
	HeaderText text = HeaderText::GameModeName;
};

constexpr std::array<HeaderSource, 3> kHeaderSources{ {
		{ "uisettingscomp", 1098058770, HeaderText::GameModeName },
		{ "uilevelcomp", -1926457542, HeaderText::LevelName },
		{ "uiteamcomp", -909259276, HeaderText::FriendlyTeamName },
} };

constexpr double kMissionHeaderWidth = 385.0;

uint32_t header_text_hash(HeaderText p_text, const HudModel &p_model) {
	switch (p_text) {
		case HeaderText::GameModeName:
			return p_model.settings.current_game_mode_name;
		case HeaderText::LevelName:
			return p_model.level.current_level;
		case HeaderText::FriendlyTeamName:
			return p_model.team.friendly_team_name;
	}
	return 0;
}

As2Value header_value(const std::string &p_static, const HudDataSource &p_source, const HudModel &p_model,
		const HudClientState &p_client) {
	if (!p_static.empty()) {
		const std::optional<std::string> t = p_client.translate ? p_client.translate(p_static) : std::nullopt;
		return As2Value::of(t.value_or(p_static));
	}
	const std::string leaf = ui_data_category_leaf(p_source.category);
	for (const HeaderSource &s : kHeaderSources) {
		if (s.category != leaf || s.key != p_source.key) {
			continue;
		}
		const uint32_t hash = header_text_hash(s.text, p_model);
		if (hash == 0 || !p_client.localise_hash) {
			return As2Value();
		}
		return As2Value::of(p_client.localise_hash(hash));
	}
	return As2Value();
}

class MissionHeaderMpClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { binding_ = p_node.page_header; }

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		if (binding_) {
			hud_page_header_data(*binding_, p_model, p_client, r_out);
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const std::string *w = p_init.property("m_width");
		const double width = w != nullptr ? As2Value::of(*w).to_number() : std::nan("");
		width_ = std::isnan(width) ? width_ : width;
		for (const std::string_view name : { "text1", "text2" }) {
			if (ScrollTextWidget *t = controller_at<ScrollTextWidget>(r_host, r_host.clip(name))) {
				t->set_text("");
				t->field().set_auto_size(true);
				t->set_width(float(width_));
			}
		}
		if (const As2Clip icon = r_host.clip("icon"); icon.exists()) {
			icon.set_visible(false);
		}
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		std::string_view target;
		if (p_key == "Header") {
			target = "text1";
		} else if (p_key == "SubHeader") {
			target = "text2";
		} else {
			return;
		}
		if (ScrollTextWidget *t = controller_at<ScrollTextWidget>(r_host, r_host.clip(target))) {
			t->set_text(p_value.defined() ? p_value.to_string() : std::string());
		}
	}

private:
	std::optional<HudPageHeaderBinding> binding_;
	double width_ = kMissionHeaderWidth;
};

}

void hud_page_header_data(const HudPageHeaderBinding &p_binding, const HudModel &p_model,
		const HudClientState &p_client, HudData &r_out) {
	const auto put = [&](std::string_view p_key, const std::string &p_static, const HudDataSource &p_source) {
		if (p_static.empty() && !p_source.key && p_source.name.empty()) {
			return;
		}
		r_out.push_back(HudDataEntry{
				std::string(p_key), header_value(p_static, p_source, p_model, p_client), p_source.on_initialize });
	};
	put("Header", p_binding.static_header, p_binding.header);
	put("SubHeader", p_binding.static_sub_header, p_binding.sub_header);
	put("Icon", p_binding.static_icon, p_binding.icon);
}

void register_hud_widgets_mission_header(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.MissionHeader.MissionHeaderMP", [] { return std::make_unique<MissionHeaderMpClass>(); });
}

}

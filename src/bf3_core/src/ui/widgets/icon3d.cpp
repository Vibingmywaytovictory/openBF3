#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class Icon3DClass : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		created_ = false;
		r_host.self().stop();
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path != r_host.path()) {
			return;
		}
		if (p_fs.calls("createIconComplete")) {
			r_host.self().stop();
		}
		if (p_fs.calls("removeIconComplete")) {
			r_host.self().stop();
			r_host.self().set_visible(false);
			r_host.remove_movie_clip(r_host.self());
		}
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "IsClose") {
			const bool old_snap = !(looking_at_ || close_);
			const bool new_snap = !(looking_at_ || p_value.truthy());
			close_ = p_value.truthy();
			update_snapping(r_host, old_snap, new_snap);
			update_is_close(r_host, close_);
		} else if (p_key == "LookingAt") {
			const bool old_snap = !(looking_at_ || close_);
			const bool new_snap = !(close_ || p_value.truthy());
			looking_at_ = p_value.truthy();
			update_snapping(r_host, old_snap, new_snap);
		} else if (p_key == "Snap") {
			update_snap_data(r_host, p_value);
		} else if (p_key == "Icons") {
			As2Value icons = As2Value::array();
			icons.push(p_value);
			update_icons(r_host, icons);
		} else {
			update_key(r_host, p_key, p_value);
		}
	}

	void create3d_icon(HudWidgetHost &r_host) {
		r_host.self().set_visible(true);
		if (!created_) {
			created_ = true;
			r_host.self().goto_and_play("create");
		}
	}

	void remove_icon(HudWidgetHost &r_host) {
		created_ = false;
		r_host.self().goto_and_play("remove");
	}

	bool created() const { return created_; }

protected:
	virtual void update_snap_data(HudWidgetHost &r_host, const As2Value &p_value) {
		const bool snap = !(looking_at_ || close_);
		update_snapping(r_host, snap, snap);
		snap_ = p_value.truthy();
	}
	virtual void update_snapping(HudWidgetHost &, bool, bool) {}
	virtual void update_is_close(HudWidgetHost &, bool) {}
	virtual void update_icons(HudWidgetHost &, const As2Value &) {}
	virtual void update_key(HudWidgetHost &, std::string_view, const As2Value &) {}

	bool snap_ = false;
	bool looking_at_ = false;
	bool first_update_ = true;
	bool close_ = false;
	bool created_ = false;
};

class SupportIconTagClass final : public Icon3DClass {
public:
	void construct(HudWidgetHost &r_host) override {
		Icon3DClass::construct(r_host);
		r_host.self().set_visible(false);
		r_host.self().stop();
	}

protected:
	void update_icons(HudWidgetHost &r_host, const As2Value &p_icons) override {
		if (p_icons.size() > 0) {
			icon_ = as2_number(p_icons.at(0));
			update_tag(r_host);
		}
	}

	void update_key(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Team") {
			team_ = as2_number(p_value);
			update_tag(r_host);
		}
	}

private:
	void update_tag(HudWidgetHost &r_host) {
		const As2Clip tag = r_host.clip("tag");
		if (std::isfinite(icon_) && icon_ != 0.0) {
			goto_and_stop_number(tag, icon_);
		}
		if (std::isfinite(team_) && team_ != 0.0) {
			goto_and_stop_number(tag.child("icon"), team_);
		}
	}

	double icon_ = 0.0;
	double team_ = 0.0;
};

class MapmarkertagClass final : public Icon3DClass {
public:
	void construct(HudWidgetHost &r_host) override {
		Icon3DClass::construct(r_host);
		const As2Clip tag = r_host.clip("tag");
		r_host.self().set_visible(false);
		for (const char *c : { "label", "txtBg", "txtBgAdd", "iconBg", "iconBgAdd" }) {
			tag.child(c).set_visible(false);
		}
		r_host.self().stop();
	}

protected:
	void update_key(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Label") {
			label_ = p_value.to_string();
			has_label_ = !label_.empty();
			update_tag(r_host);
		} else if (p_key == "Spotted") {
			const bool spotted = as2_int(p_value) != kUiSpottedInfoNone;
			if (spotted_ != spotted) {
				r_host.self().goto_and_play(p_value.truthy() ? "spotted" : "create");
			}
			spotted_ = spotted;
		}
	}

	void update_icons(HudWidgetHost &r_host, const As2Value &p_icons) override {
		const As2Clip tag = r_host.clip("tag");
		if (p_icons.size() > 0) {
			const double n = as2_number(p_icons.at(0));
			goto_and_stop_number(tag.child("icon"), n);
			blue_bg_ = std::find(kUiHudIconsOnRedMarker.begin(), kUiHudIconsOnRedMarker.end(), n) == kUiHudIconsOnRedMarker.end();
			tag.child("iconBg").set_visible(true);
			tag.child("iconBgAdd").set_visible(true);
			if (tag.child("icon").child("icon").exists()) {
				tag.child("icon").child("icon").goto_and_stop(4);
			}
		} else {
			blue_bg_ = true;
			tag.child("iconBg").set_visible(false);
			tag.child("iconBgAdd").set_visible(false);
		}
		update_tag(r_host);
	}

	void update_snapping(HudWidgetHost &r_host, bool p_old, bool p_new) override {
		const As2Clip tag = r_host.clip("tag");
		if (first_update_) {
			if (p_new) {
				tag.goto_and_stop(blue_bg_ ? "hiddenBlue" : "hiddenRed");
			}
			first_update_ = false;
		} else if (p_new != p_old) {
			if (p_new) {
				tag.set_visible(true);
			}
			if (p_new || !has_label_) {
				tag.goto_and_play(blue_bg_ ? "hideBlue" : "hideRed");
			} else {
				tag.goto_and_play(blue_bg_ ? "showBlue" : "showRed");
			}
		}
		snapped_ = p_new;
		update_tag(r_host);
	}

private:
	void update_tag(HudWidgetHost &r_host) {
		const As2Clip tag = r_host.clip("tag");
		const As2Clip label = tag.child("label");
		label.child("nameTxt").set_text(label_);
		label.child("nameTxtBg").set_text(label_);
		for (const char *c : { "label", "txtBg", "txtBgAdd" }) {
			tag.child(c).set_visible(has_label_);
		}
		if (has_label_) {
			label.goto_and_stop(blue_bg_ ? "blue" : "red");
		}
		if (snapped_ || !has_label_) {
			tag.goto_and_stop(blue_bg_ ? "hiddenBlue" : "hiddenRed");
		} else {
			tag.goto_and_play(blue_bg_ ? "showBlue" : "showRed");
		}
	}

	std::string label_;
	bool spotted_ = false;
	bool has_label_ = false;
	bool snapped_ = false;
	bool blue_bg_ = true;
};

class AlerttagClass final : public Icon3DClass {
public:
	void construct(HudWidgetHost &r_host) override {
		Icon3DClass::construct(r_host);
		r_host.self().set_visible(false);
		r_host.self().stop();
	}

protected:
	void update_snap_data(HudWidgetHost &r_host, const As2Value &p_value) override {
		if (snap_ != p_value.truthy()) {
			r_host.clip("grIcon").goto_and_play(p_value.truthy() ? "snapped" : "normal");
		}
		snap_ = p_value.truthy();
	}
};

class Icon3DManagerClass final : public HudWidgetController, public HudIcon3DManagerApi {
public:
	struct Spec {
		HudTagComponent component = HudTagComponent::Mapmarker;
		std::string_view tag_template;
		std::string_view linkage;
	};
	explicit Icon3DManagerClass(Spec p_spec) :
			spec_(p_spec) {}

	bool aligns_in_safe_rect() const override { return false; }

	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		if (p_node.widget_path_query) {
			query_ = *p_node.widget_path_query;
		}
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		if (!spec_.tag_template.empty()) {
			r_host.clip(spec_.tag_template).set_visible(false);
		}
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const As2Value *key = p_init.value("WidgetPathKey");
		published_key_ = key != nullptr ? key->to_number() : std::nan("");
		published_path_ = "_level0." + r_host.path();
		const As2Value *name = p_init.value("ComponentName");
		component_name_ = name != nullptr ? name->to_string() : std::string();
		r_host.self().set_visible(true);
	}

	void bind(const HudModel &, const HudClientState &, HudData &r_out) const override {
		if (!query_) {
			return;
		}
		r_out.push_back({ "WidgetPathKey", As2Value::of(double(query_->key)) });
		r_out.push_back({ "ComponentName", As2Value::of(query_->category_short_name) });
	}

	void drive(HudWidgetHost &r_host, const HudModel &, const HudClientState &p_client) override {
		std::vector<uint32_t> seen;
		if (p_client.world_tags != nullptr) {
			for (const HudWorldTag &t : p_client.world_tags->tags) {
				if (t.component == spec_.component) {
					seen.push_back(t.id);
					place(r_host, t);
				}
			}
		}
		for (auto it = live_.begin(); it != live_.end();) {
			HudWidgetHost *h = r_host.screen().host(it->second.path);
			Icon3DClass *tag = h != nullptr ? dynamic_cast<Icon3DClass *>(r_host.screen().controller(it->second.path)) : nullptr;
			if (tag == nullptr) {
				it = live_.erase(it);
				continue;
			}
			if (tag->created() && std::find(seen.begin(), seen.end(), it->first) == seen.end()) {
				tag->remove_icon(*h);
			}
			++it;
		}
	}

	HudTagComponent tag_component() const override { return spec_.component; }
	const std::string &published_path() const override { return published_path_; }
	double published_key() const override { return published_key_; }
	const std::string &component_name() const override { return component_name_; }
	std::vector<std::string> tag_paths() const override {
		std::vector<std::string> out;
		for (const auto &[id, l] : live_) {
			out.push_back(l.path);
		}
		return out;
	}

private:
	struct Live {
		std::string path;
		std::map<std::string, As2Value, std::less<>> sent;
	};

	void place(HudWidgetHost &r_host, const HudWorldTag &p_tag) {
		auto it = live_.find(p_tag.id);
		if (it == live_.end()) {
			const As2Clip made = r_host.attach_movie(r_host.self(), spec_.linkage, "tag" + std::to_string(p_tag.id), r_host.self().next_highest_depth());
			if (!made.exists()) {
				return;
			}
			it = live_.emplace(p_tag.id, Live{ made.path(), {} }).first;
		}
		Live &l = it->second;
		HudWidgetHost *h = r_host.screen().host(l.path);
		Icon3DClass *tag = h != nullptr ? dynamic_cast<Icon3DClass *>(r_host.screen().controller(l.path)) : nullptr;
		if (tag == nullptr) {
			return;
		}
		h->self().set_x(p_tag.x - r_host.self().x());
		h->self().set_y(p_tag.y - r_host.self().y());
		if (!tag->created()) {
			tag->create3d_icon(*h);
		}
		for (const HudDataEntry &e : p_tag.data) {
			if (!e.value.defined()) {
				continue;
			}
			auto s = l.sent.find(e.key);
			if (s != l.sent.end() && s->second == e.value) {
				continue;
			}
			l.sent[e.key] = e.value;
			tag->update(*h, e.key, e.value);
		}
	}

	Spec spec_;
	std::optional<HudDataSource> query_;
	std::string published_path_;
	double published_key_ = std::nan("");
	std::string component_name_;
	std::map<uint32_t, Live> live_;
};

class InteractionManagerClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override { r_host.self().set_visible(true); }
};

}

void register_hud_widgets_icon3d(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Icon3D.TeamSupport.SupportIconManager", [] { return std::make_unique<Icon3DManagerClass>(Icon3DManagerClass::Spec{ HudTagComponent::TeamSupport, "TeamSupportIcon", "TeamSupportIcon" }); });
	r_registry.add("Widget.Icon3D.TeamSupport.SupportIconTag", [] { return std::make_unique<SupportIconTagClass>(); });
	r_registry.add("Widget.Icon3D.Alerts.AlertManager", [] { return std::make_unique<Icon3DManagerClass>(Icon3DManagerClass::Spec{ HudTagComponent::Alert, {}, "Alert" }); });
	r_registry.add("Widget.Icon3D.Alerts.Alerttag", [] { return std::make_unique<AlerttagClass>(); });
	r_registry.add("Widget.Icon3D.Interaction.InteractionManager", [] { return std::make_unique<InteractionManagerClass>(); });
	r_registry.add("Widget.Icon3D.Mapmarker.MapmarkerManager", [] { return std::make_unique<Icon3DManagerClass>(Icon3DManagerClass::Spec{ HudTagComponent::Mapmarker, "Marker", "Marker" }); });
	r_registry.add("Widget.Icon3D.Mapmarker.Mapmarkertag", [] { return std::make_unique<MapmarkertagClass>(); });
}

}

#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class AmmoClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip ammo = r_host.clip("Ammo");
		ammo.child("reload.btn").goto_and_stop(r_host.client().platform);
		r_host.renderer(r_host.self(), "t2");
		r_host.tilt(r_host.self(), -15.0f, 15.0f, kHudPerspectiveFov);
		for (const char *name : { "auto", "burst", "single" }) {
			ammo.child("fmd").child(name).child("gfx").stop();
			ammo.child("fmd").child(name).child("bg").stop();
		}
		update(r_host, "GrenadeAmmo", As2Value::of(0));
		update(r_host, "HasAlternativeProjectiles", As2Value::of(false));
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const As2Clip ammo = r_host.clip("Ammo");
		setup_refresh_data(*this, r_host, p_init.data);
		HudBackgroundWidgetApi *bg = controller_at<HudBackgroundWidgetApi>(r_host, ammo.child("bgAmmo"));
		if (bg != nullptr) {
			bg->initialize_without_data();
		}
		const As2Value *progress = r_host.construct_param("m_showWeaponProgress");
		show_progress_ = progress != nullptr && progress->truthy();
		if (bg != nullptr) {
			bg->set_height(show_progress_ ? kProgressPanelHeight : kPanelHeight);
		}
		ammo.child("weaponProgress").set_visible(show_progress_);
		ammo.goto_and_stop("us");
		if (p_init.value("Visible") == nullptr && p_init.value("VisibleAccess") == nullptr) {
			const As2Value *hc = p_init.value("IsHardcore");
			r_host.self().set_visible(!(hc != nullptr && hc->truthy()));
		}
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		const As2Clip ammo = r_host.clip("Ammo");
		if (p_key == "TotalAmmo") {
			total_ = p_value.defined() ? p_value.to_number() : -1.0;
			update_total(ammo);
		} else if (p_key == "ClipAmmo") {
			clip_ = p_value.to_number();
			update_clip(ammo);
		} else if (p_key == "GrenadeAmmo") {
			const double v = p_value.to_number();
			ammo.child("grtxt.txtDisplay").set_text(v < 0.0 || !show_ammo_ ? std::string("---") : p_value.to_string());
		} else if (p_key == "ShowAmmo") {
			show_ammo_ = string_true(p_value) || !p_value.defined();
			update_clip(ammo);
			update_total(ammo);
		} else if (p_key == "LowAmmo") {
			if (string_true(p_value)) {
				ammo.child("reload").goto_and_play("reload_us");
				ammo.child("clip").goto_and_play("reload_us");
			} else {
				ammo.child("reload").goto_and_stop(0);
				ammo.child("clip").goto_and_stop(0);
			}
		} else if (p_key == "ReloadButton") {
			if (!p_value.truthy()) {
				return;
			}
			const As2Clip icon = ammo.child("reload.btn.icon");
			if (r_host.client().platform == "win32") {
				const As2Clip label = icon.child("label");
				label.set_auto_size(true);
				label.set_text(p_value.to_string());
				const As2Clip bgc = icon.child("button.bg");
				float w = label.text_width() + bgc.x() + 4.0f;
				bgc.set_width(w);
				if (w < 23.0f) {
					bgc.set_width(23.0f);
				}
				const As2Clip left = icon.child("button.left");
				icon.child("button.right").set_x(left.x() + left.width() + bgc.x() + bgc.width() - 10.0f);
			} else {
				icon.goto_and_stop(p_value.to_string());
			}
		} else if (p_key == "FiringMode") {
			const double v = p_value.to_number();
			if (v != current_mode_) {
				current_mode_ = v;
				const std::string name = mode_name(v);
				for (const char *n : { "auto", "burst", "single" }) {
					const As2Clip c = ammo.child("fmd").child(n);
					if (name == n) {
						c.goto_and_play("active");
					} else {
						c.goto_and_stop("inactive");
					}
				}
			}
		} else if (p_key == "FiringModeTypes") {
			modes_ = p_value.list;
			for (const char *n : { "auto", "burst", "single" }) {
				ammo.child("fmd").child(n).set_visible(false);
			}
			for (const double id : modes_) {
				const std::string n = mode_name(id);
				if (n.empty()) {
					continue;
				}
				const As2Clip c = ammo.child("fmd").child(n);
				c.child("gfx").goto_and_stop(n);
				c.child("bg").goto_and_stop(n);
				c.set_visible(true);
			}
		} else if (p_key == "HasAlternativeProjectiles") {
			const bool b = p_value.truthy();
			ammo.child("fmd").set_visible(!b);
			ammo.child("atp").set_visible(b);
		} else if (p_key == "WeaponProgress") {
			const double p = p_value.to_number();
			if (!show_progress_) {
				return;
			}
			const As2Clip wp = ammo.child("weaponProgress");
			if (p == -1.0) {
				wp.set_alpha(0.0f);
			} else {
				wp.child("progress").goto_and_stop(int32_t(as2_round(p * 100.0)));
				wp.set_alpha(100.0f);
			}
		} else if (p_key == "Visible") {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		const HudWeapon &w = p_model.weapon;
		r_out.push_back({ "ClipAmmo", As2Value::of(w.active_weapon_ammo) });
		r_out.push_back({ "TotalAmmo", As2Value::of(w.active_weapon_ammo_in_bag) });
		r_out.push_back({ "GrenadeAmmo", As2Value::of(w.secondary_weapon_ammo) });
		r_out.push_back({ "LowAmmo", As2Value::of(w.active_weapon_critical_ammo) });
		r_out.push_back({ "ShowAmmo", As2Value::of(w.active_weapon_show_ammo) });
		r_out.push_back({ "FiringMode", As2Value::of(double(w.firing_mode)) });
		std::vector<double> modes;
		for (int32_t id = 0; id < 32; ++id) {
			if ((w.firing_mode_types & (1u << uint32_t(id))) != 0) {
				modes.push_back(double(id));
			}
		}
		r_out.push_back({ "FiringModeTypes", As2Value::numbers(std::move(modes)) });
		r_out.push_back({ "Visible", As2Value::of(hud_soldier_widgets_visible(p_model, p_client)) });
		if (!p_client.reload_button.empty()) {
			r_out.push_back({ "ReloadButton", As2Value::of(p_client.reload_button) });
		}
		r_out.push_back({ "WeaponProgress", As2Value::of(double(w.active_weapon_progress)), false });
		r_out.push_back({ "IsHardcore", As2Value::of(p_client.hardcore) });
		r_out.push_back({ "ProjectileIndex", As2Value::of(w.projectile_index), false });
		r_out.push_back({ "HasAlternativeProjectiles", As2Value::of(w.has_alternative_projectiles), false });
	}

private:
	static std::string mode_name(double p_id) {
		if (!(p_id >= 0.0) || p_id >= double(kAs2FiringModes.size()) || p_id != std::floor(p_id)) {
			return {};
		}
		return std::string(kAs2FiringModes[size_t(p_id)]);
	}

	void update_total(const As2Clip &p_ammo) const {
		const std::string s = total_ < 0.0 || !show_ammo_ ? std::string("---") : as2_number_string(total_);
		p_ammo.child("amholder.amtxtGlow.txtDisplay").set_text(s);
		p_ammo.child("amholder.amtxt.txtDisplay").set_text(s);
	}

	void update_clip(const As2Clip &p_ammo) const {
		p_ammo.child("clip.cltxt.txtDisplay").set_text(clip_ < 0.0 || !show_ammo_ ? std::string("---") : as2_number_string(clip_));
	}

	static constexpr double kProgressPanelHeight = 90.0;
	static constexpr double kPanelHeight = 80.0;
	bool show_ammo_ = true;
	bool show_progress_ = false;
	double clip_ = 0.0;
	double total_ = 0.0;
	double current_mode_ = -1.0;
	std::vector<double> modes_;
};

}

void register_hud_widgets_ammo(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.Ammo", [] { return std::make_unique<AmmoClass>(); });
}

}

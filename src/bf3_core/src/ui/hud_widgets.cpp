#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace bf3::core::ui {

namespace {

bool string_true(const As2Value &p_v) {
	return to_lower_ascii(p_v.to_string()) == "true";
}

double as2_round(double p_v) {
	return std::floor(p_v + 0.5);
}

std::string param_string(const HudWidgetHost &p_host, std::string_view p_name) {
	const As2Value *v = p_host.construct_param(p_name);
	return v != nullptr ? v->to_string() : std::string("undefined");
}

void setup_refresh_data(HudWidgetController &r_ctl, HudWidgetHost &r_host, const HudData &p_data) {
	for (const HudDataEntry &e : p_data) {
		if (e.value.defined()) {
			r_ctl.update(r_host, e.key, e.value);
		}
	}
}

template <class T>
T *controller_at(HudWidgetHost &r_host, const As2Clip &p_clip) {
	return dynamic_cast<T *>(r_host.screen().controller(p_clip.path()));
}

class HitAnimation {
public:
	void watch(const As2Clip &p_holder) {
		const uint32_t f = p_holder.current_frame();
		const As2Clip controller = p_holder.child("HitController");
		const uint32_t cf = controller.current_frame();
		if (f != last_holder_ && (f == kLevelLowFrame || f == kLevelHighFrame)) {
			controller.play();
		}
		if (cf != last_controller_ && cf == kControllerPlayFrame) {
			p_holder.play();
		}
		last_holder_ = p_holder.current_frame();
		last_controller_ = controller.current_frame();
	}

private:
	static constexpr uint32_t kLevelLowFrame = 2;
	static constexpr uint32_t kLevelHighFrame = 13;
	static constexpr uint32_t kControllerPlayFrame = 2;
	uint32_t last_holder_ = 0;
	uint32_t last_controller_ = 0;
};

constexpr std::array<int32_t, 4> kHitLevelFrames{ 0, 2, 2, 13 };

class ScreenClass final : public HudWidgetController {
public:
	bool screen_initialized() const override { return false; }
};

class HudBackgroundClass final : public HudWidgetController, public HudBackgroundApi {
public:
	bool screen_initialized() const override { return false; }

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		const As2Clip render = r_host.clip("renderHolder");
		const As2Clip guide = render.child("guide_mc");
		if (guide.exists()) {
			guide.set_visible(false);
		}
		start_x_ = render.x();
		start_y_ = render.y();
		start_w_ = r_host.self().width();
		start_h_ = r_host.self().height();
		horizontal_ = param_string(r_host, "alignHorizontal");
		vertical_ = param_string(r_host, "alignVertical");
		set_width(start_w_, "");
		set_height(start_h_, "");
		render.goto_and_play("us");
	}

	void on_clip_load(HudWidgetHost &r_host) override { r_host.renderer(r_host.clip("renderHolder"), kBgDrawRenderer); }

	void set_width(double p_value, std::string_view p_dir) override {
		const As2Clip render = host_->clip("renderHolder");
		const double fade = p_value / kHorizontalFadePercentage;
		render.set_width(float(as2_round(p_value + fade)));
		const std::string dir = p_dir.empty() ? std::string("left") : std::string(p_dir);
		if (dir == "left") {
			render.set_x(float(-fade / 2.0));
		} else if (dir == "right") {
			render.set_x(float(start_w_ - render.width() + fade / 2.0));
		} else if (dir == "center") {
			render.set_x(float(start_w_ / 2.0 - render.width() / 2.0));
		}
	}

	void set_height(double p_value, std::string_view p_dir) override {
		const As2Clip render = host_->clip("renderHolder");
		const double fade = p_value / kVerticalFadePercentage;
		render.set_height(float(as2_round(p_value + fade)));
		const std::string dir = p_dir.empty() ? std::string("top") : std::string(p_dir);
		if (dir == "top") {
			render.set_y(float(-fade / 2.0));
		} else if (dir == "bottom") {
			render.set_y(float(start_h_ - render.height() + fade / 2.0));
		} else if (dir == "center") {
			render.set_y(float(start_h_ / 2.0 - render.height() / 2.0));
		}
	}

	double start_width() const override { return start_w_; }
	double start_height() const override { return start_h_; }

private:
	HudWidgetHost *host_ = nullptr;
	double start_x_ = 0.0;
	double start_y_ = 0.0;
	double start_w_ = 0.0;
	double start_h_ = 0.0;
	std::string horizontal_;
	std::string vertical_;
};

class HudBackgroundWidgetClass final : public HudWidgetController, public HudBackgroundWidgetApi {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		start_w_ = r_host.self().width();
		start_h_ = r_host.self().height();
		start_y_ = r_host.self().y();
		r_host.self().set_visible(false);
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		if (const As2Value *shake = r_host.construct_param("m_enableShake"); shake != nullptr && shake->truthy()) {
			r_host.renderer(r_host.self(), "t2");
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override { run_initialize(r_host, &p_init); }

	void initialize_without_data() override { run_initialize(*host_, nullptr); }

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Visible") {
			if (!hardcore_) {
				r_host.self().set_visible(p_value.truthy());
			}
		} else if (p_key == "ShowAirRadar") {
			set_height(p_value.truthy() ? kAirRadarHeight : height_);
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "Visible", As2Value::of(p_client.minimap_small) });
		r_out.push_back({ "HideForHardcore", As2Value::of(p_client.hardcore) });
		r_out.push_back({ "ShowAirRadar", As2Value::of(p_model.minimap.air_radar_mask_enabled || p_client.show_air_radar) });
	}

	void set_width(double p_value) override {
		if (p_value != 0.0 && !std::isnan(p_value)) {
			if (HudBackgroundApi *bg = background()) {
				bg->set_width(p_value, "");
			}
		}
	}

	void set_height(double p_value) override {
		if (p_value != 0.0 && !std::isnan(p_value)) {
			if (HudBackgroundApi *bg = background()) {
				bg->set_height(p_value, "");
			}
		}
	}

	double width_value() const override { return width_; }
	double height_value() const override { return height_; }
	bool hardcore() const override { return hardcore_; }
	std::string tilt_mode() const override { return tilt_; }

private:
	HudBackgroundApi *background() { return controller_at<HudBackgroundClass>(*host_, host_->clip("Background")); }

	void run_initialize(HudWidgetHost &r_host, const HudWidgetInit *p_init) {
		const std::string *tilt = p_init != nullptr ? p_init->property("m_enableTilt") : nullptr;
		tilt_ = tilt != nullptr ? *tilt : std::string("undefined");
		if (tilt_ != "none") {
			if (tilt_ == "left") {
				r_host.tilt(r_host.self(), -15.0f, -15.0f, kHudPerspectiveFov);
			} else if (tilt_ == "right") {
				r_host.tilt(r_host.self(), -15.0f, 15.0f, kHudPerspectiveFov);
			}
		}
		const std::string *w = p_init != nullptr ? p_init->property("m_width") : nullptr;
		const std::string *h = p_init != nullptr ? p_init->property("m_height") : nullptr;
		const double wn = w != nullptr ? As2Value::of(*w).to_number() : std::nan("");
		const double hn = h != nullptr ? As2Value::of(*h).to_number() : std::nan("");
		width_ = std::isnan(wn) ? start_w_ : wn;
		height_ = std::isnan(hn) ? start_h_ : hn;
		const As2Value *hide = p_init != nullptr ? p_init->value("HideForHardcore") : nullptr;
		hardcore_ = hide != nullptr ? hide->truthy() : false;
		if (hardcore_) {
			height_ = start_h_ - kMinimapSize;
			r_host.self().set_y(r_host.self().y() + float(kMinimapSize));
		}
		r_host.self().set_xscale(100.0f);
		r_host.self().set_yscale(100.0f);
		set_width(width_);
		set_height(height_);
		if (p_init != nullptr) {
			setup_refresh_data(*this, r_host, p_init->data);
		}
		r_host.self().set_visible(true);
	}

	static constexpr double kAirRadarHeight = 80.0;
	static constexpr double kMinimapSize = 170.0;
	HudWidgetHost *host_ = nullptr;
	double start_w_ = 0.0;
	double start_h_ = 0.0;
	double start_y_ = 0.0;
	double width_ = 0.0;
	double height_ = 0.0;
	bool hardcore_ = false;
	std::string tilt_;
};

HudBackgroundWidgetApi *background_widget(HudWidgetHost &r_host, const As2Clip &p_clip) {
	return controller_at<HudBackgroundWidgetClass>(r_host, p_clip);
}

class HealthClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip holder = r_host.clip("Health.healthHolder");
		r_host.renderer(holder.child("EKGRenderTarget"), kSoldierRenderer);
		r_host.renderer(holder, "t2");
		r_host.tilt(holder, -15.0f, 15.0f, kHudPerspectiveFov);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const As2Clip holder = r_host.clip("Health.healthHolder");
		const std::string *prop = p_init.property("HealthData");
		As2Value health = prop != nullptr && !prop->empty() ? As2Value::of(*prop) : As2Value::undefined();
		if (!health.defined()) {
			if (const As2Value *v = p_init.value("Health")) {
				health = *v;
			}
		}
		set_health(r_host, health);
		if (HudBackgroundWidgetApi *bg = background_widget(r_host, holder.child("bgHealth"))) {
			bg->initialize_without_data();
		}
		holder.goto_and_stop("us");
		const As2Value *visible = p_init.value("Visible");
		r_host.self().set_visible(visible != nullptr ? visible->truthy() : true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Health") {
			set_health(r_host, p_value);
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "Health", As2Value::of(double(p_model.soldier.health)) });
		r_out.push_back({ "Visible", As2Value::of(!p_client.hardcore) });
	}

private:
	static void set_health(HudWidgetHost &r_host, const As2Value &p_value) {
		double n = as2_round(p_value.to_number());
		if (n < 0.0 || !p_value.defined()) {
			n = 0.0;
		}
		r_host.clip("Health.healthHolder.amtxt.txtDisplay").set_text(as2_number_string(n));
	}
};

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
		HudBackgroundWidgetApi *bg = background_widget(r_host, ammo.child("bgAmmo"));
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
		r_out.push_back({ "Visible", As2Value::of(!p_client.hardcore) });
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

class TicketCounterClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.renderer(r_host.self(), "t2");
		r_host.tilt(r_host.self(), -15.0f, -15.0f, kHudPerspectiveFov);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		mode_ = param_string(r_host, "m_gamemode");
		setup_gamemodes(r_host);
		setup_refresh_data(*this, r_host, p_init.data);
		if (const As2Value *hc = p_init.value("IsHardcore"); hc != nullptr && hc->truthy()) {
			r_host.self().set_y(r_host.self().y() + float(kMinimapSize));
		}
		layout_gamemodes(r_host);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Team1TDMKills") {
			team1_ = p_value;
			if (player_team_ && *player_team_ == 0.0) {
				friendly(r_host, p_value);
			} else {
				enemy(r_host, p_value);
			}
		} else if (p_key == "Team2TDMKills") {
			team2_ = p_value;
			if (player_team_ && *player_team_ == 1.0) {
				friendly(r_host, p_value);
			} else {
				enemy(r_host, p_value);
			}
		} else if (p_key == "PlayerTeam") {
			const double t = p_value.to_number() - 1.0;
			if (!player_team_ || *player_team_ != t) {
				player_team_ = t;
				if (mode_ == "tdm") {
					update(r_host, "Team1TDMKills", team1_);
					update(r_host, "Team2TDMKills", team2_);
				}
			}
		} else if (p_key == "FriendlyTDMTeamName" || p_key == "FriendlyTeamName") {
			const std::string s = p_value.to_string();
			if (s != friendly_name_) {
				friendly_name_ = s;
				r_host.clip(friendly_row_).child("team").set_text(s);
			}
		} else if (p_key == "EnemyTDMTeamName" || p_key == "EnemyTeamName") {
			const std::string s = p_value.to_string();
			if (s != enemy_name_) {
				enemy_name_ = s;
				r_host.clip(enemy_row_).child("team").set_text(s);
			}
		} else if (p_key == "FriendlyTickets") {
			friendly(r_host, p_value);
		} else if (p_key == "EnemyTickets") {
			enemy(r_host, p_value);
		} else if (p_key == "MaxTickets") {
			if (!max_tickets_ || !(*max_tickets_ == p_value)) {
				max_tickets_ = p_value;
				setup_gamemodes(r_host);
			}
		}
	}

	void drive(HudWidgetHost &r_host, const HudModel &, const HudClientState &p_client) override { r_host.self().set_visible(p_client.minimap_small); }

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		const HudTeam &t = p_model.team;
		const HudKillCounter &k = p_model.kill_counter;
		if (p_client.localise_hash) {
			if (t.friendly_team_name != 0) {
				r_out.push_back({ "FriendlyTDMTeamName", As2Value::of(p_client.localise_hash(t.friendly_team_name)) });
			}
			if (t.enemy_team1_name != 0) {
				r_out.push_back({ "EnemyTDMTeamName", As2Value::of(p_client.localise_hash(t.enemy_team1_name)) });
			}
		}
		r_out.push_back({ "Team1TDMKills", As2Value::of(k.team1_kills) });
		r_out.push_back({ "Team2TDMKills", As2Value::of(k.team2_kills) });
		if (k.player_team > 0) {
			r_out.push_back({ "PlayerTeam", As2Value::of(k.player_team) });
		}
		if (k.team1_max_kills > 0) {
			r_out.push_back({ "MaxTickets", As2Value::of(k.team1_max_kills) });
		}
		r_out.push_back({ "IsHardcore", As2Value::of(p_client.hardcore) });
	}

	const std::string &friendly_row() const { return friendly_row_; }
	const std::string &enemy_row() const { return enemy_row_; }

private:
	void show_teams(HudWidgetHost &r_host, int32_t p_n) {
		for (int32_t i = 0; i < 4; ++i) {
			if (i >= p_n) {
				r_host.clip("tickets" + std::to_string(i)).set_visible(false);
			}
		}
	}

	void setup_gamemodes(HudWidgetHost &r_host) {
		if (mode_ != "tdm") {
			return;
		}
		show_teams(r_host, 2);
		friendly_row_ = "tickets1";
		enemy_row_ = "tickets0";
		const As2Clip enemy_row = r_host.clip(enemy_row_);
		enemy_row.goto_and_stop("enemy");
		row_frame_script(enemy_row, 2);
		row_frame_script(r_host.clip(friendly_row_), 1);
		r_host.clip(friendly_row_).child("team").set_text(friendly_name_);
		enemy_row.child("team").set_text(enemy_name_);
		update(r_host, "Team1TDMKills", team1_);
		update(r_host, "Team2TDMKills", team2_);
	}

	static void row_frame_script(const As2Clip &p_row, int32_t p_frame) {
		const As2Clip meter = p_row.child("meter");
		for (const char *n : { "meter", "leftEdge", "rightEdge", "bg" }) {
			meter.child(n).goto_and_stop(p_frame);
		}
	}

	void layout_gamemodes(HudWidgetHost &r_host) {
		if (mode_ != "tdm") {
			return;
		}
		const As2Clip first = r_host.clip(friendly_row_);
		const As2Clip second = r_host.clip(enemy_row_);
		const float name_w = std::max(first.child("team").text_width(), second.child("team").text_width());
		const float tick_w = std::max(first.child("tickets").text_width(), second.child("tickets").text_width());
		const float right = std::clamp(kNameFieldOffset + name_w + kMinPadding + tick_w, kMinRightBound, kMaxRightBound);
		const float bar_x = std::clamp(right - kBarOverlap, kBarRightEnd - kBarNormalWidth, 100000.0f);
		for (const As2Clip &row : { first, second }) {
			const As2Clip tickets = row.child("tickets");
			tickets.set_x(right - tick_w - kTicketMargin);
			tickets.set_width(tick_w + kTicketMargin);
		}
		for (const As2Clip &row : { first, second }) {
			const As2Clip meter = row.child("meter");
			meter.set_xscale((kBarRightEnd - bar_x) / kBarNormalWidth * 100.0f);
			meter.set_x(bar_x);
			update_tickets(row, As2Value::of(As2Value::of(row.child("tickets").text()).to_number()));
		}
	}

	void friendly(HudWidgetHost &r_host, const As2Value &p_v) {
		if (!friendly_row_.empty()) {
			update_tickets(r_host.clip(friendly_row_), p_v);
		}
	}

	void enemy(HudWidgetHost &r_host, const As2Value &p_v) {
		if (!enemy_row_.empty()) {
			update_tickets(r_host.clip(enemy_row_), p_v);
		}
	}

	void update_tickets(const As2Clip &p_row, const As2Value &p_v) const {
		p_row.child("tickets").set_text(p_v.to_string());
		const As2Clip m = p_row.child("meter");
		const double max = max_tickets_ ? max_tickets_->to_number() : std::nan("");
		const double pct = std::max(std::min(100.0, as2_round(p_v.to_number() / max * 100.0)), 0.0);
		if (!std::isnan(as2_round(p_v.to_number() / max * 100.0))) {
			m.child("meter").set_xscale(float(pct));
		}
		m.child("rightEdge").set_x(m.child("meter").x() + m.child("meter").width());
		if (p_v.to_number() == 0.0) {
			m.child("leftEdge").set_visible(false);
			m.child("rightEdge").set_visible(false);
		}
	}

	static constexpr float kMinimapSize = 170.0f;
	static constexpr float kNameFieldOffset = 8.0f;
	static constexpr float kMinRightBound = 80.0f;
	static constexpr float kMaxRightBound = 105.0f;
	static constexpr float kBarNormalWidth = 90.0f;
	static constexpr float kBarRightEnd = 170.0f;
	static constexpr float kMinPadding = 4.0f;
	static constexpr float kBarOverlap = 4.0f;
	static constexpr float kTicketMargin = 20.0f;
	std::string mode_;
	std::string friendly_row_;
	std::string enemy_row_;
	std::string friendly_name_ = "YOU";
	std::string enemy_name_ = "THEM";
	As2Value team1_ = As2Value::of(0);
	As2Value team2_ = As2Value::of(0);
	std::optional<double> player_team_;
	std::optional<As2Value> max_tickets_;
};

class CompassClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		type_ = param_string(r_host, "m_compassType");
		mp_ = type_ == "multiplayerCompass";
		const std::string linkage = mp_ || type_ == "singlePlayerCompass" ? std::string("mc_singlePlayerCompass") : "mc_" + type_ + "_us";
		heading_ = r_host.attach_movie(r_host.self(), linkage, type_, r_host.self().next_highest_depth());
		heading_.set_x(0.0f);
		heading_.set_y(0.0f);
		if (linkage == "mc_singlePlayerCompass") {
			r_host.renderer(heading_, "t2");
			r_host.tilt(heading_, -15.0f, -15.0f, kHudPerspectiveFov);
		}
		objectives_.clear();
		for (int32_t i = 1;; ++i) {
			const As2Clip obj = heading_.child("objective" + std::to_string(i)).child("obj");
			if (!obj.exists()) {
				break;
			}
			objectives_.push_back(obj);
			obj.goto_and_stop(kObjectiveIdleFrame);
		}
		if (mp_) {
			heading_.child("bg").set_visible(false);
			for (const As2Clip &obj : objectives_) {
				obj.set_visible(false);
			}
		}
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		heading_.child("bg").set_visible(false);
		heading_.goto_and_stop("us");
		if (p_init.value("Visible") == nullptr && p_init.value("VisibleAccess") == nullptr) {
			r_host.self().set_visible(true);
		}
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Heading") {
			heading_.child("cps").goto_and_stop(int32_t(std::trunc(p_value.to_number())));
		} else if (p_key == "Visible") {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	void drive(HudWidgetHost &, const HudModel &p_model, const HudClientState &) override {
		heading_.child("cps").goto_and_stop(hud_compass_frame(p_model.soldier.compass_angle) + 1);
	}

	const As2Clip &heading_clip() const { return heading_; }
	const std::vector<As2Clip> &objectives() const { return objectives_; }

private:
	static constexpr int32_t kObjectiveIdleFrame = 180;
	std::vector<As2Clip> objectives_;
	std::string type_;
	bool mp_ = false;
	As2Clip heading_;
};

class MinimapClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.renderer(r_host.clip("map"), kMinimapRenderer);
		r_host.tilt(r_host.self(), -15.0f, -15.0f, kHudPerspectiveFov);
		orig_w_ = r_host.self().width();
		orig_h_ = r_host.self().height();
		orig_x_ = r_host.self().x();
		orig_y_ = r_host.self().y();
		r_host.clip("hitindicator").goto_and_stop("idle");
	}

	void pre_update_screen_size(HudWidgetHost &r_host) override { update_size(r_host, true, true); }

	void update_screen_size(HudWidgetHost &r_host) override {
		orig_w_ = r_host.self().width();
		orig_h_ = r_host.self().height();
		orig_x_ = r_host.self().x();
		orig_y_ = r_host.self().y();
		update_size(r_host, small_, true);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		const As2Value *visible = p_init.value("Visible");
		r_host.self().set_visible(visible != nullptr ? visible->truthy() : true);
		r_host.clip("hitindicator").set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Size") {
			update_size(r_host, p_value.truthy(), false);
		} else if (p_key == "Zoom") {
			zoom_ = p_value.to_number();
		} else if (p_key == "ShowCross") {
			r_host.clip("testCross").set_visible(p_value.truthy());
			r_host.clip("hitindicator").set_visible(p_value.truthy());
		} else if (p_key == "HitIndicatorValue") {
			const As2Clip cross = r_host.clip("testCross");
			if (!cross.visible()) {
				return;
			}
			const As2Clip hit = r_host.clip("hitindicator");
			const double d = p_value.to_number();
			if (!p_value.defined() || d == 0.0) {
				hit.goto_and_stop(0);
			} else if (d >= 0.0 && d < double(kHitLevelFrames.size()) && hit.current_frame() != uint32_t(kHitLevelFrames[size_t(d)])) {
				hit.child("HitController").goto_and_stop(1);
				hit.goto_and_stop(kHitLevelFrames[size_t(d)]);
			}
		} else if (p_key == "ShowAirRadar") {
			if (p_value.truthy()) {
				r_host.clip("tint").set_visible(false);
				r_host.self().set_y(float(orig_y_ + 20.0));
			} else {
				r_host.clip("tint").set_visible(true);
				r_host.self().set_y(float(orig_y_));
			}
		}
	}

	void advance(HudWidgetHost &r_host, double) override { hit_.watch(r_host.clip("hitindicator")); }

	void tween_complete(HudWidgetHost &, uint32_t) override {}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "Size", As2Value::of(p_client.minimap_small) });
		r_out.push_back({ "Zoom", As2Value::of(double(p_client.minimap_zoom)) });
		r_out.push_back({ "ShowCross", As2Value::of(p_client.show_minimap_cross) });
		r_out.push_back({ "Visible", As2Value::of(!p_client.hardcore) });
		r_out.push_back({ "ShowAirRadar", As2Value::of(p_model.minimap.air_radar_mask_enabled || p_client.show_air_radar) });
	}

	double zoom() const { return zoom_; }
	bool small() const { return small_; }

private:
	void update_size(HudWidgetHost &r_host, bool p_small, bool p_always) {
		if (p_small == small_ && !p_always) {
			return;
		}
		const double w = p_small ? orig_w_ : kLargeSize;
		const double h = p_small ? orig_h_ : kLargeSize;
		const double x = p_small ? orig_x_ : orig_x_ - 40.0;
		const double y = p_small ? orig_y_ : orig_y_ + 50.0 + (orig_h_ - kLargeSize);
		r_host.remove_tweens(r_host.self());
		if (!p_always) {
			r_host.clip("tint").goto_and_play(p_small ? "MaxOut" : "MaxIn");
			r_host.add_tween(r_host.self(), HudTweenProperty::Width, float(w), kTweenSeconds, HudTweenEase::EaseOutBack, 1);
			r_host.add_tween(r_host.self(), HudTweenProperty::Height, float(h), kTweenSeconds, HudTweenEase::EaseOutBack);
			r_host.add_tween(r_host.self(), HudTweenProperty::X, float(x), kTweenSeconds, HudTweenEase::EaseOutBack);
			r_host.add_tween(r_host.self(), HudTweenProperty::Y, float(y), kTweenSeconds, HudTweenEase::EaseOutBack);
			small_ = p_small;
		} else {
			r_host.self().set_width(float(w));
			r_host.self().set_height(float(h));
			r_host.self().set_x(float(orig_x_));
			r_host.self().set_y(float(p_small ? orig_y_ : orig_y_ + (orig_h_ - kLargeSize)));
		}
	}

	static constexpr double kLargeSize = 600.0;
	static constexpr double kTweenSeconds = 0.05;
	double orig_w_ = 0.0;
	double orig_h_ = 0.0;
	double orig_x_ = 0.0;
	double orig_y_ = 0.0;
	double zoom_ = 0.0;
	bool small_ = true;
	HitAnimation hit_;
};

class CrosshairClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		for (const char *n : { "crosshair", "circle", "dot", "grenade" }) {
			r_host.clip(n).set_visible(false);
		}
		run_initialize(r_host, nullptr);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override { run_initialize(r_host, &p_init); }

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Visible") {
			r_host.self().set_visible(string_true(p_value));
		} else if (p_key == "BaseValue") {
			base_ = p_value.to_number();
			update_dispersion(r_host, current_ + base_);
		} else if (p_key == "Dispersion") {
			current_ = p_value.to_number();
			update_dispersion(r_host, current_ + base_);
		} else if (p_key == "Zoomed") {
			zoomed_ = p_value.truthy();
			if (zoomed_ != was_zoomed_) {
				const As2Clip c = r_host.clip(type_);
				if (hide_on_zoom_) {
					if (zoomed_) {
						r_host.add_tween(c, HudTweenProperty::Alpha, 0.0f, kFadeOutSeconds, HudTweenEase::Linear);
					} else {
						r_host.remove_tweens(c);
						update_dispersion(r_host, current_ + base_);
						r_host.add_tween(c, HudTweenProperty::Alpha, 100.0f, kFadeInSeconds, HudTweenEase::Linear);
					}
				}
				was_zoomed_ = zoomed_;
			}
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		const HudWeapon &w = p_model.weapon;
		r_out.push_back({ "Visible", As2Value::of(p_model.soldier.is_alive && w.active_weapon_crosshair != 0) });
		r_out.push_back({ "BaseValue", As2Value::of(0.0) });
		r_out.push_back({ "Dispersion", As2Value::of(hud_crosshair_dispersion(w.dispersion, p_client.vertical_fov_deg, p_client.stage_height)) });
		r_out.push_back({ "Zoomed", As2Value::of(w.zoomed) });
	}

	bool hide_on_zoom() const { return hide_on_zoom_; }

private:
	void run_initialize(HudWidgetHost &r_host, const HudWidgetInit *p_init) {
		if (p_init != nullptr) {
			if (const std::string *h = p_init->property("HideOnZoom"); h != nullptr && !h->empty()) {
				hide_on_zoom_ = to_lower_ascii(*h) == "true";
			}
		}
		type_ = param_string(r_host, "m_crosshairType");
		if (type_.empty() || type_ == "undefined") {
			type_ = "crosshair";
		}
		const As2Clip c = r_host.clip(type_);
		c.set_visible(true);
		r_host.renderer(c, "x1");
		if (p_init != nullptr) {
			setup_refresh_data(*this, r_host, p_init->data);
		}
	}

	void update_dispersion(HudWidgetHost &r_host, double p_d) {
		const double r = as2_round(p_d * 100.0);
		if (std::isnan(r)) {
			return;
		}
		const As2Clip c = r_host.clip(type_);
		if (type_ == "crosshair") {
			c.child("up").set_y(float(-r));
			c.child("down").set_y(float(r));
			c.child("left").set_x(float(-r));
			c.child("right").set_x(float(r));
		} else if (type_ == "circle") {
			c.set_xscale(float(64.0 + r / 3.0));
			c.set_yscale(float(64.0 + r / 3.0));
		}
	}

	static constexpr double kFadeOutSeconds = 0.2;
	static constexpr double kFadeInSeconds = 0.1;
	std::string type_ = "crosshair";
	bool hide_on_zoom_ = false;
	double base_ = 0.0;
	double current_ = 0.0;
	bool zoomed_ = false;
	bool was_zoomed_ = false;
};

class HitindicatorClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.self().set_visible(false);
		r_host.clip("hitindicator").goto_and_stop("idle");
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
		r_host.clip("hitindicator").set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "IsInVehicle") {
			in_vehicle_ = p_value.truthy();
		} else if (p_key == "IndicatorValue") {
			if (in_vehicle_) {
				return;
			}
			const As2Clip hit = r_host.clip("hitindicator");
			const double d = p_value.to_number();
			if (!p_value.defined() || d == 0.0) {
				hit.goto_and_stop(0);
				return;
			}
			if (d >= 0.0 && d < double(kHitLevelFrames.size()) && hit.current_frame() != uint32_t(kHitLevelFrames[size_t(d)])) {
				hit.child("HitController").goto_and_stop(1);
				hit.goto_and_stop(kHitLevelFrames[size_t(d)]);
			}
		}
	}

	void advance(HudWidgetHost &r_host, double) override { hit_.watch(r_host.clip("hitindicator")); }

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		const HudWeapon &w = p_model.weapon;
		const double level = w.show_hitindicator ? std::clamp(std::round(double(w.hitindicator_level)), 1.0, 3.0) : 0.0;
		r_out.push_back({ "IndicatorValue", As2Value::of(level) });
		r_out.push_back({ "IsInVehicle", As2Value::of(false) });
	}

private:
	bool in_vehicle_ = false;
	HitAnimation hit_;
};

class LatencyIndicatorClass final : public HudWidgetController {
public:
	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override { setup_refresh_data(*this, r_host, p_init.data); }

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Visible") {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "Visible", As2Value::of(p_client.latency_warning) });
	}
};

class HiddenUntilDataClass final : public HudWidgetController {
public:
	struct Spec {
		std::vector<std::string_view> hide;
		bool shake = false;
		bool tilt = false;
	};
	explicit HiddenUntilDataClass(Spec p_spec) :
			spec_(std::move(p_spec)) {}

	bool screen_initialized() const override { return false; }

	void on_clip_load(HudWidgetHost &r_host) override {
		for (const std::string_view h : spec_.hide) {
			r_host.clip(h).set_visible(false);
		}
		if (spec_.shake) {
			r_host.renderer(r_host.self(), "t2");
		}
		if (spec_.tilt) {
			r_host.tilt(r_host.self(), -15.0f, 15.0f, kHudPerspectiveFov);
		}
		r_host.self().set_visible(false);
	}

private:
	Spec spec_;
};

}

int32_t hud_compass_frame(float p_heading_deg) {
	if (!std::isfinite(p_heading_deg)) {
		return 0;
	}
	const int32_t h = int32_t(std::lround(double(p_heading_deg)));
	return ((h % 360) + 360) % 360;
}

double hud_crosshair_dispersion(float p_spread_deg, float p_vertical_fov_deg, float p_stage_height) {
	if (!(p_spread_deg > 0.0f) || !(p_vertical_fov_deg > 0.0f) || !(p_vertical_fov_deg < 180.0f) || !(p_stage_height > 0.0f)) {
		return 0.0;
	}
	constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
	const double gap = double(p_stage_height) / 2.0 * std::tan(double(p_spread_deg) * kDegToRad) / std::tan(double(p_vertical_fov_deg) * kDegToRad / 2.0);
	return gap / 100.0;
}

bool hud_widget_class_known(std::string_view p_class) {
	return make_hud_widget_controller(p_class) != nullptr;
}

std::unique_ptr<HudWidgetController> make_hud_widget_controller(std::string_view p_class) {
	using Spec = HiddenUntilDataClass::Spec;
	if (p_class == "fb.Base.UIScreen" || p_class == "Screen.Weapon.CrosshairDefault") {
		return std::make_unique<ScreenClass>();
	}
	if (p_class == "Shared.HudBackground") {
		return std::make_unique<HudBackgroundClass>();
	}
	if (p_class == "Widget.Hud.HudBackgroundWidget") {
		return std::make_unique<HudBackgroundWidgetClass>();
	}
	if (p_class == "Widget.Hud.Health") {
		return std::make_unique<HealthClass>();
	}
	if (p_class == "Widget.Hud.Ammo") {
		return std::make_unique<AmmoClass>();
	}
	if (p_class == "Widget.TicketCounter.TicketCounter") {
		return std::make_unique<TicketCounterClass>();
	}
	if (p_class == "Widget.Hud.Compass") {
		return std::make_unique<CompassClass>();
	}
	if (p_class == "Widget.Hud.Minimap") {
		return std::make_unique<MinimapClass>();
	}
	if (p_class == "Widget.Crosshair.Crosshair") {
		return std::make_unique<CrosshairClass>();
	}
	if (p_class == "Widget.Crosshair.Hitindicator") {
		return std::make_unique<HitindicatorClass>();
	}
	if (p_class == "Widget.Misc.LatencyIndicator") {
		return std::make_unique<LatencyIndicatorClass>();
	}
	if (p_class == "Widget.Hud.SquadList") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ { "squadMember0", "squadMember1", "squadMember2", "squadMember3" }, true, false });
	}
	if (p_class == "Widget.Message.HudMessage") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ { "posGuide" }, false, false });
	}
	if (p_class == "Widget.Hud.PassangerList") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ {}, true, false });
	}
	if (p_class == "Widget.Hud.VehicleHealth") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ {}, true, true });
	}
	if (p_class == "Widget.Message.ScoreAggregator") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ {}, false, false });
	}
	if (p_class == "Widget.Icon3D.TeamSupport.SupportIconManager") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ { "TeamSupportIcon" }, false, false });
	}
	if (p_class == "Widget.Message.HudAlertMessage") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ { "alert", "warning", "information" }, false, false });
	}
	if (p_class == "Widget.Icon3D.Mapmarker.MapmarkerManager") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ { "Marker" }, false, false });
	}
	return nullptr;
}

}

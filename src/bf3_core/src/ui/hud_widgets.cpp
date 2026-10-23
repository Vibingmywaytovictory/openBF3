#include "bf3/core/math.h"
#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
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

bool param_true(const HudWidgetHost &p_host, std::string_view p_name) {
	const As2Value *v = p_host.construct_param(p_name);
	if (v == nullptr) {
		return false;
	}
	return v->kind == As2Value::Kind::Bool ? v->boolean : string_true(*v);
}

std::string as2_id(const As2Value &p_id) {
	return p_id.to_string();
}

class HudMessageOwner {
public:
	virtual ~HudMessageOwner() = default;
	virtual void move_row(const std::string &p_id) = 0;
	virtual void remove_row(const std::string &p_id) = 0;
	virtual void removed_row(const std::string &p_id) = 0;
};

class MessageBaseClass : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		r_host.self().goto_and_stop("start");
		message_height_ = r_host.clip("heightClip").height();
		r_host.self().set_alpha(0.0f);
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path != r_host.path()) {
			return;
		}
		if (p_fs.calls("startAnimDone")) {
			start_anim_done();
		}
		if (p_fs.calls("outAnimationDone")) {
			out_animation_done();
		}
	}

	virtual void set_data(const As2Value &p_e) { data_ = p_e; }

	virtual void refresh(const As2Value &p_e) {
		data_ = p_e;
		if (p_e.field("RemoveFlag").to_number() == 1.0) {
			out_anim();
		}
	}

	virtual double get_height() const { return message_height_; }

	virtual void start_anim() {
		host_->self().set_alpha(100.0f);
		host_->self().goto_and_play("startAnim");
	}

	virtual void out_anim() {
		if (owner_ != nullptr) {
			owner_->remove_row(id());
		}
		host_->self().goto_and_play("outAnim");
	}

	void move(double p_y, bool p_instant) {
		double time = kMoveSeconds;
		if (p_instant) {
			time = 0.0;
			started_ = true;
		}
		host_->add_tween(host_->self(), HudTweenProperty::Y, float(p_y), time, HudTweenEase::ExpoOut);
	}

	void start_anim_done() { started_ = true; }

	void out_animation_done() {
		if (owner_ != nullptr) {
			owner_->removed_row(id());
		}
	}

	bool has_started() const { return started_; }
	void set_owner(HudMessageOwner *p_owner) { owner_ = p_owner; }
	std::string id() const { return as2_id(data_.field("Id")); }
	const As2Value &data() const { return data_; }

protected:
	static constexpr double kMoveSeconds = 0.4;
	HudWidgetHost *host_ = nullptr;
	HudMessageOwner *owner_ = nullptr;
	As2Value data_;
	double message_height_ = 0.0;
	bool started_ = false;
};

void set_both_texts(HudWidgetHost &r_host, const As2Clip &p_holder, std::string_view p_text) {
	const std::string shown = r_host.translate(p_text);
	p_holder.child("txt").set_text(shown);
	p_holder.child("txtBg").set_text(shown);
}

void fit_both_texts(const As2Clip &p_holder, float p_offset) {
	const float w = p_holder.child("txt").text_width() + p_offset;
	p_holder.child("txt").set_width(w);
	p_holder.child("txtBg").set_width(w);
}

class KillLogMessageClass final : public MessageBaseClass {
public:
	void set_data(const As2Value &p_e) override {
		MessageBaseClass::set_data(p_e);
		const As2Value &v = p_e.field("Value");
		set_color(v.field("KillerColor").to_number(), "killer");
		set_color(v.field("VictimColor").to_number(), "victim");
		set_message(v.field("Text").to_string());
		set_killer(v.field("KillerName").to_string(), v.field("KillerName").defined());
		set_victim(v.field("VictimName").to_string(), v.field("VictimName").defined());
		setup_icon(v.field("HeadShot").truthy());
		update_text_positions();
		start_anim();
	}

private:
	static constexpr float kTextOffset = 5.0f;

	As2Clip container() const { return host_->clip("container"); }

	void set_color(double p_color, std::string_view p_field) {
		const As2Clip c = container().child(p_field);
		if (p_color == double(kHudRelationFriendly)) {
			c.goto_and_stop("friendly");
		} else if (p_color == double(kHudRelationEnemy)) {
			c.goto_and_stop("enemy");
		} else if (p_color == double(kHudRelationSquad)) {
			c.goto_and_stop("squad");
		}
	}

	void set_message(const std::string &p_text) {
		As2Clip target = container().child("msg");
		if (p_text == kHudKillsIsNoSid) {
			target = container().child("victim");
			switch_message_ = true;
		}
		target.goto_and_stop("neutral");
		set_both_texts(*host_, target, p_text);
		fit_both_texts(target, kTextOffset);
	}

	void set_name(const As2Clip &p_field, const std::string &p_name, bool p_show) {
		p_field.set_visible(p_show);
		set_both_texts(*host_, p_field, p_name);
		fit_both_texts(p_field, kTextOffset);
		if (!p_show) {
			p_field.set_width(0.0f);
		}
	}

	void set_killer(const std::string &p_name, bool p_show) { set_name(container().child("killer"), p_name, p_show); }

	void set_victim(const std::string &p_name, bool p_show) { set_name(container().child(switch_message_ ? "msg" : "victim"), p_name, p_show); }

	void setup_icon(bool p_headshot) {
		const As2Clip icon = container().child("headShotIcon");
		if (p_headshot) {
			icon.goto_and_stop("KitRecon");
			icon.set_visible(true);
		} else {
			icon.set_visible(false);
		}
	}

	void update_text_positions() {
		const As2Clip c = container();
		const As2Clip killer = c.child("killer");
		const As2Clip msg = c.child("msg");
		const As2Clip icon = c.child("headShotIcon");
		const As2Clip victim = c.child("victim");
		msg.set_x(killer.x() + killer.width() + kTextOffset);
		icon.set_x(msg.x() + msg.width() + 10.0f);
		icon.set_y(17.0f);
		victim.set_x(icon.visible() ? icon.x() + 10.0f : msg.x() + msg.width() + kTextOffset);
		host_->self().set_x(-host_->self().width());
	}

	bool switch_message_ = false;
};

class LocalKillLogMessageClass final : public MessageBaseClass {
public:
	void set_data(const As2Value &p_e) override {
		MessageBaseClass::set_data(p_e);
		const As2Value &v = p_e.field("Value");
		set_field("killer", v.field("KillerName"));
		set_field("victim", v.field("VictimName"));
		set_message(v.field("Text").to_string());
		setup_icon(v.field("HeadShot").truthy());
		update_text_positions();
		start_anim();
	}

private:
	static constexpr float kTextOffset = 5.0f;
	static constexpr float kKillerAloneX = 100.0f;

	As2Clip container() const { return host_->clip("container"); }

	void set_field(std::string_view p_field, const As2Value &p_name) {
		const As2Clip f = container().child(p_field);
		f.set_visible(p_name.defined());
		set_both_texts(*host_, f, p_name.to_string());
		fit_both_texts(f, kTextOffset);
	}

	void set_message(const std::string &p_text) {
		const As2Clip msg = container().child("msg");
		msg.goto_and_stop("neutral");
		set_both_texts(*host_, msg, p_text);
		fit_both_texts(msg, kTextOffset);
	}

	void setup_icon(bool p_headshot) {
		const As2Clip icon = container().child("headShotIcon");
		if (p_headshot) {
			icon.goto_and_stop("KitRecon");
			icon.set_visible(true);
		} else {
			icon.set_visible(false);
		}
	}

	void update_text_positions() {
		const As2Clip c = container();
		const As2Clip killer = c.child("killer");
		const As2Clip msg = c.child("msg");
		const As2Clip icon = c.child("headShotIcon");
		const As2Clip victim = c.child("victim");
		if (!victim.visible()) {
			killer.set_x(kKillerAloneX);
		}
		msg.set_x(killer.x() + killer.width() + kTextOffset);
		icon.set_x(msg.x() + msg.width() + 10.0f);
		icon.set_y(17.0f);
		victim.set_x(icon.visible() ? icon.x() + 10.0f : msg.x() + msg.width() + kTextOffset);
		host_->self().set_x(-(victim.x() + victim.width()) * 0.5f);
	}
};

class ScoreMessageClass final : public MessageBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		MessageBaseClass::construct(r_host);
		play_out_after_loop_ = false;
		r_host.self().set_x(kScoreX);
		r_host.self().stop();
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path != r_host.path()) {
			return;
		}
		if (p_fs.calls("playOutAnimAfterLoop")) {
			if (!play_out_after_loop_) {
				r_host.self().goto_and_play("loop");
			}
			return;
		}
		if (p_fs.calls("outAnimComplete")) {
			if (owner_ != nullptr) {
				owner_->removed_row(id());
			}
			return;
		}
		MessageBaseClass::frame_script(r_host, p_fs);
	}

	void set_data(const As2Value &p_e) override {
		MessageBaseClass::set_data(p_e);
		const As2Value &v = p_e.field("Value");
		const std::array<std::string, 1> args{ v.field("Score").to_string() };
		const std::string text = hud_format_text(host_->translate(v.field("Text").to_string()), args);
		const As2Clip m = host_->clip("scoringMessage");
		m.child("txt").set_text(text);
		m.child("txtBg").set_text(text);
		start_anim();
	}

	void start_anim() override {
		host_->self().set_alpha(100.0f);
		host_->self().goto_and_play("start");
	}

	void refresh(const As2Value &p_e) override {
		MessageBaseClass::refresh(p_e);
		host_->clip("cursor_mc").set_visible(p_e.field("TotalMessages") == p_e.field("Index"));
	}

	void out_anim() override {
		play_out_after_loop_ = true;
		if (owner_ != nullptr) {
			owner_->remove_row(id());
			owner_->move_row(id());
		}
	}

private:
	static constexpr float kScoreX = 140.0f;
	bool play_out_after_loop_ = false;
};

class GameMessageClass final : public MessageBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		MessageBaseClass::construct(r_host);
		r_host.self().set_x(-r_host.self().width() / 2.0f);
		r_host.clip("txt.txtDisplay").set_auto_size(SwfTextAutoSize::Center);
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path == r_host.path() + ".txt" && p_fs.calls("setSize")) {
			set_size();
			return;
		}
		if (p_fs.path == r_host.path() && p_fs.calls("outAnimComplete")) {
			out_anim_complete();
			return;
		}
		MessageBaseClass::frame_script(r_host, p_fs);
	}

	void set_data(const As2Value &p_e) override {
		MessageBaseClass::set_data(p_e);
		host_->clip("txt.txtDisplay").set_text(host_->translate(p_e.field("Value").field("Text").at(0).to_string()));
		start_anim();
	}

	void start_anim() override {
		host_->self().goto_and_play("in");
		host_->self().set_alpha(100.0f);
		host_->self().play();
	}

	void out_anim() override {
		host_->self().goto_and_play("out");
		if (owner_ != nullptr) {
			owner_->remove_row(id());
		}
	}

	double get_height() const override { return std::min(double(host_->clip("txt.txtDisplay").text_height()), kMaxHeight); }

private:
	static constexpr double kMaxHeight = 300.0;
	static constexpr double kWidthPad = 60.0;
	static constexpr int32_t kBackgroundShownFrame = 21;

	void set_size() {
		const As2Clip t = host_->clip("txt.txtDisplay");
		const double w = double(t.text_width()) + kWidthPad;
		const double h = std::min(double(t.text_height()), kMaxHeight);
		const As2Clip bg = host_->clip("txt.bg");
		if (HudBackgroundWidgetApi *api = controller_at<HudBackgroundWidgetApi>(*host_, bg)) {
			api->initialize_without_data();
			api->set_width(as2_round(w));
			api->set_height(as2_round(h) + 10.0);
		}
		bg.set_x(float(as2_round(-w / 2.0) + 5.0));
		bg.set_y(-5.0f);
		bg.goto_and_stop(kBackgroundShownFrame);
	}

	void out_anim_complete() {
		if (owner_ != nullptr) {
			owner_->removed_row(id());
		}
		if (host_->self().exists()) {
			host_->self().set_alpha(0.0f);
		}
	}
};

class HudMessageClass final : public HudWidgetController, public HudMessageApi, public HudMessageOwner {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		if (p_node.message_query) {
			source_ = hud_message_source(p_node.message_query->key);
			on_initialize_ = p_node.message_query->on_initialize;
		}
		number_of_rows_ = p_node.number_of_rows;
	}

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		inverted_list_ = param_true(r_host, "invertedList");
		invert_push_ = param_true(r_host, "invertPush");
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		ids_.clear();
		r_host.create_empty_movie_clip(r_host.self(), kHolder, r_host.self().next_highest_depth());
		r_host.clip("posGuide").set_visible(false);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "NumberOfRows") {
			max_row_count_ = p_value.to_number();
		} else if (p_key == "Message") {
			update_message_data(p_value);
		}
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "NumberOfRows", As2Value::of(number_of_rows_) });
		if (p_client.messages != nullptr && source_ != HudMessageSource::None) {
			r_out.push_back({ "Message", p_client.messages->list(source_), on_initialize_ });
		}
	}

	void advance(HudWidgetHost &, double p_seconds) override {
		if (team_kill_.empty()) {
			return;
		}
		team_kill_left_ -= p_seconds;
		if (team_kill_left_ <= 0.0) {
			remove_team_kill_message();
		}
	}

	void move_row(const std::string &) override { refresh_pos(-1.0); }
	void remove_row(const std::string &) override {}

	void removed_row(const std::string &p_id) override {
		host_->remove_movie_clip(holder().child(kRowPrefix + p_id));
		ids_.erase(std::remove(ids_.begin(), ids_.end(), p_id), ids_.end());
		refresh_pos(0.0);
	}

	uint64_t rows_added() const override { return rows_added_; }
	size_t rows() const override { return ids_.size(); }
	double max_row_count() const override { return max_row_count_; }
	HudMessageSource source() const override { return source_; }

private:
	static constexpr std::string_view kHolder = "messageHolder";
	static constexpr std::string_view kTeamKillId = "99999";
	static constexpr std::string_view kTeamKillSid = "ID_H_ALERT_FAILED_FRIENDLY_FIRE";
	static constexpr double kTeamKillWaitSeconds = 5.0;
	static constexpr float kTeamKillY = -240.0f;
	inline static const std::string kRowPrefix = "message_";

	As2Clip holder() const { return host_->clip(kHolder); }

	MessageBaseClass *row(const std::string &p_id) const { return controller_at<MessageBaseClass>(*host_, holder().child(kRowPrefix + p_id)); }

	void update_message_data(const As2Value &p_list) {
		const size_t n = p_list.size();
		for (size_t i = 0; i < n; ++i) {
			As2Value e = p_list.at(i);
			const std::string id = as2_id(e.field("Id"));
			MessageBaseClass *mc = holder().child(kRowPrefix + id).exists() ? row(id) : nullptr;
			e.set("Index", As2Value::of(double(i)));
			e.set("TotalMessages", As2Value::of(double(n) - 1.0));
			if (!holder().child(kRowPrefix + id).exists()) {
				if (e.field("RemoveFlag").to_number() != 1.0) {
					add_row(e);
				}
			} else if (mc != nullptr) {
				mc->refresh(e);
			}
		}
		refresh_pos(0.0);
	}

	void add_row(const As2Value &p_e) {
		const std::string id = as2_id(p_e.field("Id"));
		const std::string type = p_e.field("Type").to_string();
		const As2Clip h = holder();
		const As2Clip mc = host_->attach_movie(h, type, kRowPrefix + id, h.next_highest_depth());
		if (type == "Subtitle") {
			host_->renderer(mc, "t1");
		}
		MessageBaseClass *r = controller_at<MessageBaseClass>(*host_, mc);
		if (r == nullptr) {
			return;
		}
		r->set_data(p_e);
		r->set_owner(this);
		ids_.push_back(id);
		rows_added_++;
		if (p_e.field("Value").field("TeamKill").truthy()) {
			send_team_kill_info();
		}
	}

	void refresh_pos(double p_offset) {
		const size_t n = ids_.size();
		MessageBaseClass *prev = nullptr;
		double y = 0.0;
		bool instant = false;
		if (inverted_list_) {
			for (size_t k = n; k-- > 0;) {
				MessageBaseClass *r = row(ids_[k]);
				if (r == nullptr) {
					continue;
				}
				instant = !r->has_started() && !(n > 1);
				if (invert_push_) {
					y = prev == nullptr ? r->get_height() * p_offset : y + r->get_height();
				} else {
					y = prev == nullptr ? -r->get_height() : y - r->get_height();
				}
				r->move(y, instant);
				prev = r;
			}
			return;
		}
		for (size_t k = 0; k < n; ++k) {
			MessageBaseClass *r = row(ids_[k]);
			if (r == nullptr) {
				continue;
			}
			if (!r->has_started()) {
				instant = true;
			}
			if (invert_push_) {
				y = prev == nullptr ? -r->get_height() : y - r->get_height();
			} else {
				y = prev == nullptr ? r->get_height() * p_offset : y + prev->get_height();
			}
			r->move(y, instant);
			prev = r;
		}
	}

	void send_team_kill_info() {
		if (!team_kill_.empty()) {
			return;
		}
		const As2Clip h = holder();
		const std::string name = kRowPrefix + std::string(kTeamKillId);
		if (h.child(name).exists()) {
			host_->remove_movie_clip(h.child(name));
		}
		const As2Clip mc = host_->attach_movie(h, "Game", name, h.next_highest_depth());
		MessageBaseClass *r = controller_at<MessageBaseClass>(*host_, mc);
		if (r == nullptr) {
			return;
		}
		As2Value e = As2Value::object();
		e.set("Type", As2Value::of("Game"));
		e.set("Id", As2Value::of(std::string(kTeamKillId)));
		As2Value v = As2Value::object();
		v.set("Text", As2Value::array({ As2Value::of(std::string(kTeamKillSid)) }));
		e.set("Value", std::move(v));
		r->set_data(e);
		mc.set_y(kTeamKillY);
		team_kill_ = name;
		team_kill_left_ = kTeamKillWaitSeconds;
	}

	void remove_team_kill_message() {
		if (MessageBaseClass *r = controller_at<MessageBaseClass>(*host_, holder().child(team_kill_))) {
			r->out_anim();
		}
		team_kill_.clear();
	}

	HudWidgetHost *host_ = nullptr;
	HudMessageSource source_ = HudMessageSource::None;
	bool on_initialize_ = true;
	int32_t number_of_rows_ = 0;
	double max_row_count_ = 1.0;
	bool inverted_list_ = false;
	bool invert_push_ = false;
	std::vector<std::string> ids_;
	uint64_t rows_added_ = 0;
	std::string team_kill_;
	double team_kill_left_ = 0.0;
};

class ScoreAggregatorClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		if (const HudDataSource *src = p_node.source(kValue)) {
			on_initialize_ = src->on_initialize;
		}
	}

	void construct(HudWidgetHost &) override {
		ready_ = true;
		prev_value_ = -1.0;
	}

	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path == r_host.path() + ".aggHolder" && p_fs.calls("ready")) {
			ready_ = true;
		}
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != kValue || !p_value.defined()) {
			return;
		}
		const As2Clip holder = r_host.clip("aggHolder");
		const double v = p_value.to_number();
		if (v == 0.0) {
			holder.goto_and_play("out");
			prev_value_ = 0.0;
			return;
		}
		if (ready_ && prev_value_ > 0.0) {
			ready_ = false;
			holder.goto_and_play("start");
		}
		prev_value_ = v;
		const std::string text = p_value.to_string();
		r_host.clip("aggHolder.scoringMessage.txt").set_text(text);
		r_host.clip("aggHolder.scoringMessage.txtBg").set_text(text);
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		if (p_client.messages != nullptr) {
			r_out.push_back({ std::string(kValue), p_client.messages->score_aggregate(), on_initialize_ });
		}
	}

private:
	static constexpr std::string_view kValue = "ScoreAgregateValue";
	bool on_initialize_ = true;
	bool ready_ = true;
	double prev_value_ = -1.0;
};

class DamageIndicatorClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != "Indicator" || !p_value.defined()) {
			return;
		}
		for (size_t i = 0; i < kHudDamageIndicators; ++i) {
			if (p_value.size() < i) {
				continue;
			}
			const As2Value &e = p_value.at(i);
			const As2Clip ind = r_host.clip("indicator" + std::to_string(i));
			const As2Value &angle = e.field("Angle");
			if (angle.defined()) {
				ind.goto_and_stop(int32_t(angle.to_number()));
			}
			if (e.field("New").kind == As2Value::Kind::Bool && e.field("New").boolean) {
				ind.child("arrow").goto_and_play(e.field("Scale").to_number() < kMinorScale ? kMinorHitFrame : kFullHitFrame);
			}
		}
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		if (p_client.damage != nullptr) {
			r_out.push_back({ "Indicator", p_client.damage->indicators() });
		}
	}

private:
	static constexpr double kMinorScale = 50.0;
	static constexpr int32_t kMinorHitFrame = 31;
	static constexpr int32_t kFullHitFrame = 2;
};

class PanelMessageClass final : public HudWidgetController {
public:
	struct Spec {
		float x_rotation = 0.0f;
		float y_rotation = 0.0f;
		std::string_view background;
		std::vector<std::string_view> hide_on_initialize;
		bool shown_on_initialize = false;
	};
	explicit PanelMessageClass(Spec p_spec) :
			spec_(std::move(p_spec)) {}

	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.renderer(r_host.self(), "t2");
		r_host.tilt(r_host.self(), spec_.x_rotation, spec_.y_rotation, kHudPerspectiveFov);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		const As2Clip bg = r_host.clip(spec_.background);
		if (HudBackgroundWidgetApi *api = controller_at<HudBackgroundWidgetApi>(r_host, bg)) {
			api->initialize_without_data();
		}
		bg.set_visible(false);
		for (const std::string_view h : spec_.hide_on_initialize) {
			r_host.clip(h).set_visible(false);
		}
		if (spec_.shown_on_initialize) {
			r_host.self().set_visible(true);
		}
	}

private:
	Spec spec_;
};

class InteractionManagerClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override { r_host.self().set_visible(true); }
};

class HudAlertMessageClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		if (const As2Value *t = r_host.construct_param("m_messageType")) {
			message_type_ = t->to_string();
		}
		always_enabled_ = param_true(r_host, "m_alwaysEnabled");
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		for (const char *c : { "alert", "warning", "information" }) {
			r_host.clip(c).set_visible(false);
		}
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		for (const HudWidgetProperty &prop : p_init.properties) {
			if (prop.name == "m_staticDescription" && !prop.value.empty()) {
				static_description_ = prop.value;
			} else if (prop.name == "m_staticTitle" && !prop.value.empty()) {
				static_title_ = prop.value;
			}
		}
		setup_refresh_data(*this, r_host, p_init.data);
		if (message_type_ != "none" && always_enabled_) {
			visible_ = true;
			setup_message();
		}
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		for (const auto &[key, type] : kVisibilityKeys) {
			if (p_key == key) {
				show(p_value, type);
				return;
			}
		}
		for (const auto &[key, title, timed] : kInformationKeys) {
			if (p_key == key) {
				show_information(p_value, title, timed);
				return;
			}
		}
		if (p_key == "Description") {
			description_text_ = text_or_empty(p_value);
			setup_message();
		} else if (p_key == "Title") {
			title_text_ = text_or_empty(p_value);
			setup_message();
		} else if (p_key == "Timer") {
			const std::string v = p_value.to_string();
			host_->self().set_visible(!(message_type_ == "waiting" && (v == "0" || v == "00:00")));
			if (message_.exists()) {
				message_.child("content").child("timer").set_text(p_value.defined() ? v : std::string());
			}
		}
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (!message_.exists() || p_fs.path != message_.path()) {
			return;
		}
		if (p_fs.calls("onShowComplete")) {
			message_.goto_and_play(slow_loop_ ? "slowloop" : "loop");
		}
		if (p_fs.calls("onHideComplete")) {
			r_host.self().set_visible(false);
			message_.set_visible(false);
		}
	}

	void advance(HudWidgetHost &, double p_seconds) override {
		if (timer_left_ <= 0.0) {
			return;
		}
		timer_left_ -= p_seconds;
		if (timer_left_ <= 0.0) {
			timer_left_ = 0.0;
			visible_ = false;
			setup_message();
		}
	}

private:
	static constexpr float kMinimumWidth = 350.0f;
	static constexpr float kYStartTwoRows = 10.0f;
	static constexpr float kInformationTitleY = 4.0f;
	static constexpr double kHideAlertSeconds = 5.0;
	static constexpr int32_t kBackgroundShownFrame = 21;
	static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kVisibilityKeys{ {
			{ "WarningVisibility", "warning" },
			{ "RevivedVisibility", "revive" },
			{ "WaitingVisibility", "waiting" },
			{ "DeathVisibility", "death" },
			{ "FailedVisibility", "failed" },
			{ "InformationVisibility", "information" },
	} };
	static constexpr std::array<std::tuple<std::string_view, std::string_view, bool>, 4> kInformationKeys{ {
			{ "SquadLeaderVisibility", "ID_H_ALERT_SQUAD_LEADER_PROMOTION", true },
			{ "FlagTransporterVisibility", "ID_H_ALERT_YOU_TRANSPORT_FLAG_CARRIER", false },
			{ "FlagHolderVisibility", "ID_H_ALERT_YOU_HAVE_THE_FLAG", false },
			{ "LastManStandingVisibility", "ID_H_ALERT_LAST_MAN_STANDING", true },
	} };

	static std::string text_or_empty(const As2Value &p_value) {
		const std::string v = p_value.defined() ? p_value.to_string() : std::string();
		return v;
	}

	void show(const As2Value &p_value, std::string_view p_type) {
		if (!p_value.defined()) {
			return;
		}
		visible_ = p_value.truthy();
		if (visible_) {
			message_type_ = std::string(p_type);
		}
		setup_message();
	}

	void show_information(const As2Value &p_value, std::string_view p_title, bool p_timed) {
		if (!p_value.defined()) {
			return;
		}
		visible_ = p_value.truthy();
		if (visible_) {
			message_type_ = "information";
		}
		title_text_ = std::string(p_title);
		description_text_.clear();
		setup_message();
		if (message_.exists()) {
			message_.child("content").child("title").set_y(kInformationTitleY);
		}
		timer_left_ = p_timed ? kHideAlertSeconds : 0.0;
	}

	void set_text(const As2Clip &p_field, std::string_view p_text) const { p_field.set_text(host_->translate(p_text)); }

	void setup_message() {
		timer_left_ = 0.0;
		slow_loop_ = false;
		std::string_view title;
		std::string_view description;
		std::string_view colour;
		if (message_type_ == "warning") {
			message_ = host_->clip("warning");
			title = "ID_H_ALERT_COMBAT_AREA_WARNING";
			description = "ID_H_ALERT_COMBAT_AREA_WARNING_DESCRIPTION";
			colour = "off";
			bracket_colour_ = "red";
		} else if (message_type_ == "revive") {
			message_ = host_->clip("warning");
			title = "ID_H_ALERT_COOP_REVIVE";
			description = "ID_H_ALERT_COOP_REVIVE_DESCRIPTION";
			colour = "us";
			bracket_colour_ = "blue";
		} else if (message_type_ == "waiting") {
			message_ = host_->clip("alert");
			title = "GET READY";
			description = "ROUND STARTS IN";
			colour = "us";
			bracket_colour_ = "blue";
		} else if (message_type_ == "death" || message_type_ == "failed") {
			message_ = host_->clip("warning");
			title = "ID_H_ALERT_YOU_FAILED";
			description = message_type_ == "death" ? "ID_H_ALERT_YOU_DIED" : "";
			colour = "off";
			bracket_colour_ = "red";
			slow_loop_ = true;
			const As2Clip t = message_.child("content").child("title");
			t.set_y(kYStartTwoRows);
			message_.child("content").child("description").set_y(t.y() + t.height());
		} else if (message_type_ == "information") {
			message_ = host_->clip("information");
			colour = "none";
			bracket_colour_ = "blue";
		} else {
			return;
		}
		const As2Clip content = message_.child("content");
		const As2Clip title_field = content.child("title");
		const As2Clip description_field = content.child("description");
		set_text(title_field, title);
		set_text(description_field, description);
		const As2Clip bg = message_.child("bg");
		if (colour != "none") {
			bg.goto_and_stop(kBackgroundShownFrame);
			bg.set_visible(true);
		} else {
			bg.set_visible(false);
		}
		title_field.set_auto_size(SwfTextAutoSize::Center);
		description_field.set_auto_size(SwfTextAutoSize::Center);
		if (!description_text_.empty()) {
			set_text(description_field, description_text_);
		} else if (!static_description_.empty()) {
			set_text(description_field, static_description_);
		}
		if (!title_text_.empty()) {
			set_text(title_field, title_text_);
		} else if (!static_title_.empty()) {
			set_text(title_field, static_title_);
		}
		update_size();
		set_bracket_colour();
		if (visible_) {
			host_->self().set_visible(true);
			message_.set_visible(true);
			message_.goto_and_play("in");
		} else {
			message_.goto_and_play("out");
		}
	}

	void update_size() {
		const As2Clip content = message_.child("content");
		const As2Clip title = content.child("title");
		const As2Clip description = content.child("description");
		const As2Clip icon_left = content.child("iconLeft");
		const As2Clip icon_right = content.child("iconRight");
		float left = 0.0f;
		if (icon_left.exists()) {
			icon_left.set_x(title.x() - 26.0f);
			icon_right.set_x(title.x() + title.width() + 26.0f);
			if (message_type_ != "information") {
				icon_left.set_y(title.y() + 28.0f);
				icon_right.set_y(title.y() + 28.0f);
			}
			left = icon_left.x() - icon_left.width() / 2.0f;
		} else {
			left = title.x();
		}
		left = std::min(left, description.x());
		if (std::fabs(left) < 150.0f) {
			left = -150.0f;
		}
		content.child("bracketLeft").set_x(left - 25.0f);
		content.child("bracketRight").set_x(-left + 25.0f);
		if (message_type_ == "information") {
			set_bracket_size(30.0f);
		}
		const As2Clip bg = message_.child("bg");
		bg.set_xscale(100.0f);
		const double w = double(content.child("bracketRight").x() - content.child("bracketLeft").x()) + 70.0;
		if (HudBackgroundWidgetApi *api = controller_at<HudBackgroundWidgetApi>(*host_, bg)) {
			api->set_width(w);
		}
		bg.set_x(float(-w / 2.0));
	}

	void set_bracket_colour() const {
		for (const char *side : { "bracketLeft", "bracketRight" }) {
			const As2Clip b = message_.child("content").child(side);
			for (const char *part : { "top", "middle", "bottom" }) {
				b.child(part).goto_and_stop(bracket_colour_);
			}
		}
	}

	void set_bracket_size(float p_height) const {
		const As2Clip l = message_.child("content").child("bracketLeft");
		const As2Clip r = message_.child("content").child("bracketRight");
		l.child("middle").set_height(p_height);
		r.child("middle").set_height(p_height);
		const float y = l.child("middle").y() + l.child("middle").height() + l.child("bottom").height();
		l.child("bottom").set_y(y);
		r.child("bottom").set_y(y);
	}

	HudWidgetHost *host_ = nullptr;
	As2Clip message_;
	std::string message_type_;
	std::string bracket_colour_;
	std::string static_description_;
	std::string static_title_;
	std::string title_text_;
	std::string description_text_;
	bool always_enabled_ = false;
	bool visible_ = false;
	bool slow_loop_ = false;
	double timer_left_ = 0.0;
};

class SquadListClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		for (int32_t i = 0; i < kRows; ++i) {
			row(r_host, i).set_visible(false);
		}
		r_host.renderer(r_host.self(), "t2");
		r_host.self().set_visible(false);
		for (int32_t i = 0; i < kRows; ++i) {
			const As2Clip r = row(r_host, i);
			r.child("label").child("txt").set_text_color(kNametagUiColourSlot0);
			for (const char *icon : { "kit", "leader", "voip", "booster" }) {
				r.child(icon).set_rgb(kNametagUiColourSlot0);
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
};

double as2_number(const As2Value &p_v) {
	if (p_v.kind == As2Value::Kind::List || p_v.kind == As2Value::Kind::NumberList) {
		return As2Value::of(p_v.to_string()).to_number();
	}
	return p_v.to_number();
}

int32_t as2_int(const As2Value &p_v) {
	const double n = as2_number(p_v);
	return std::isfinite(n) ? int32_t(std::trunc(n)) : 0;
}

void goto_and_stop_number(const As2Clip &p_clip, double p_frame) {
	if (std::isfinite(p_frame)) {
		p_clip.goto_and_stop(int32_t(p_frame));
	}
}

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
	const double gap = double(p_stage_height) / 2.0 * std::tan(double(p_spread_deg) * kDegToRad) / std::tan(double(p_vertical_fov_deg) * kDegToRad / 2.0);
	return gap / 100.0;
}

double hud_bg_light_ramp_fraction_x() {
	const double fade = 1.0 / kHorizontalFadePercentage;
	return fade / (1.0 + fade);
}

double hud_bg_light_ramp_fraction_y() {
	const double fade = 1.0 / kVerticalFadePercentage;
	return fade / (1.0 + fade);
}

float hud_bg_ramp(const HudBgDrawLook &p_look, float p_inward_of_fade) {
	const float span = p_look.ramp_full_of_fade - p_look.ramp_start_of_fade;
	if (!(span > 0.0f)) {
		return p_inward_of_fade >= p_look.ramp_start_of_fade ? 1.0f : 0.0f;
	}
	return std::clamp((p_inward_of_fade - p_look.ramp_start_of_fade) / span, 0.0f, 1.0f);
}

float hud_ekg_beat_px(const HudEkgLook &p_look, float p_distance_px) {
	const float d = p_distance_px;
	if (!(d >= 0.0f)) {
		return 0.0f;
	}
	if (d < p_look.rise_px) {
		return p_look.peak_px * d / p_look.rise_px;
	}
	const float after_rise = d - p_look.rise_px;
	if (after_rise < p_look.fall_px) {
		return p_look.peak_px - (p_look.peak_px + p_look.undershoot_px) * after_rise / p_look.fall_px;
	}
	const float after_fall = after_rise - p_look.fall_px;
	if (after_fall < p_look.recover_px) {
		return -p_look.undershoot_px + p_look.undershoot_px * after_fall / p_look.recover_px;
	}
	return 0.0f;
}

float hud_ekg_offset_px(const HudEkgLook &p_look, double p_clock_seconds, float p_screen_x) {
	if (!std::isfinite(p_clock_seconds) || !std::isfinite(p_screen_x) || !(p_look.scroll_px_per_second > 0.0f) || !(p_look.period_seconds > 0.0f)) {
		return 0.0f;
	}
	const double emitted = p_clock_seconds - double(p_look.rect_x1 - p_screen_x) / double(p_look.scroll_px_per_second);
	double phase = std::fmod(emitted, double(p_look.period_seconds));
	if (phase < 0.0) {
		phase += double(p_look.period_seconds);
	}
	return hud_ekg_beat_px(p_look, float(phase * double(p_look.scroll_px_per_second)));
}

float hud_ekg_alpha(const HudEkgLook &p_look, float p_screen_x) {
	if (!std::isfinite(p_screen_x) || !(p_look.fade_full_x > p_look.fade_start_x)) {
		return 0.0f;
	}
	return std::clamp((p_screen_x - p_look.fade_start_x) / (p_look.fade_full_x - p_look.fade_start_x), 0.0f, 1.0f);
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
		return std::make_unique<SquadListClass>();
	}
	if (p_class == "Widget.Message.HudMessage") {
		return std::make_unique<HudMessageClass>();
	}
	if (p_class == "Widget.Message.MessageTypes.KillLogMessage") {
		return std::make_unique<KillLogMessageClass>();
	}
	if (p_class == "Widget.Message.MessageTypes.LocalKillLogMessage") {
		return std::make_unique<LocalKillLogMessageClass>();
	}
	if (p_class == "Widget.Message.MessageTypes.ScoreMessage") {
		return std::make_unique<ScoreMessageClass>();
	}
	if (p_class == "Widget.Message.MessageTypes.GameMessage") {
		return std::make_unique<GameMessageClass>();
	}
	if (p_class == "Widget.Hud.PassangerList") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ {}, true, false });
	}
	if (p_class == "Widget.Hud.VehicleHealth") {
		return std::make_unique<HiddenUntilDataClass>(Spec{ {}, true, true });
	}
	if (p_class == "Widget.Message.ScoreAggregator") {
		return std::make_unique<ScoreAggregatorClass>();
	}
	if (p_class == "Widget.Icon3D.TeamSupport.SupportIconManager") {
		return std::make_unique<Icon3DManagerClass>(Icon3DManagerClass::Spec{ HudTagComponent::TeamSupport, "TeamSupportIcon", "TeamSupportIcon" });
	}
	if (p_class == "Widget.Icon3D.TeamSupport.SupportIconTag") {
		return std::make_unique<SupportIconTagClass>();
	}
	if (p_class == "Widget.Message.HudAlertMessage") {
		return std::make_unique<HudAlertMessageClass>();
	}
	if (p_class == "Widget.Hud.DamageIndicator") {
		return std::make_unique<DamageIndicatorClass>();
	}
	if (p_class == "Widget.Message.LogMessage") {
		return std::make_unique<PanelMessageClass>(PanelMessageClass::Spec{ -5.0f, -15.0f, "container.bg", {}, false });
	}
	if (p_class == "Widget.Message.TooltipMessage") {
		return std::make_unique<PanelMessageClass>(PanelMessageClass::Spec{ -5.0f, 15.0f, "container.bg",
				{ "container.textTop.textClip.t1", "container.textTop.textClip.t2", "container.textTop.textClip.t3", "container.textTop.textClip.t4",
						"container.textBottom.textClip.t1", "container.textBottom.textClip.t2", "container.textBottom.textClip.t3",
						"container.textBottom.textClip.t4" },
				false });
	}
	if (p_class == "Widget.Icon3D.Alerts.AlertManager") {
		return std::make_unique<Icon3DManagerClass>(Icon3DManagerClass::Spec{ HudTagComponent::Alert, {}, "Alert" });
	}
	if (p_class == "Widget.Icon3D.Alerts.Alerttag") {
		return std::make_unique<AlerttagClass>();
	}
	if (p_class == "Widget.Icon3D.Interaction.InteractionManager") {
		return std::make_unique<InteractionManagerClass>();
	}
	if (p_class == "Widget.Message.ObjectiveMessage") {
		return std::make_unique<PanelMessageClass>(PanelMessageClass::Spec{ 5.0f, -15.0f, "header.bg", {}, true });
	}
	if (p_class == "Widget.Icon3D.Mapmarker.MapmarkerManager") {
		return std::make_unique<Icon3DManagerClass>(Icon3DManagerClass::Spec{ HudTagComponent::Mapmarker, "Marker", "Marker" });
	}
	if (p_class == "Widget.Icon3D.Mapmarker.Mapmarkertag") {
		return std::make_unique<MapmarkertagClass>();
	}
	return nullptr;
}

}

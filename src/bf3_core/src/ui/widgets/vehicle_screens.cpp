#include "hud_widget_support.h"

#include <cstdio>

// SOURCE: the AS2 classes TurretDisplay, VehicleHudWarnings, VehicleZoomFlir, VehicleWeaponUnlockDisplay,
// VehicleWeaponName, VehicleStaticArt, VehicleGunnerWeaponElevation, VehicleCrosshair (actionscriptlibrary), ported here.
namespace bf3::core::ui {

namespace {

constexpr std::string_view kVehicleCompCategory = "ui/uicomponents/uivehiclecomp";
// SOURCE: notes R5-UI2 E: the UIVehicleComp and UIWeaponComp DataKeys below (Health, DamageStateDisabled, Velocity,
// Entry1/2Rotation and Inverted, the weapon pitches, ActiveWeaponName, InVehicle, ShortName, Reloading, Heat, Ammo).
constexpr int32_t kVehicleHealthKey = int32_t(0x49ba4cf0u);
constexpr int32_t kDamageStateDisabledKey = int32_t(0x39331320u);
constexpr int32_t kVelocityKey = int32_t(0xbff6f23bu);
constexpr int32_t kEntry1RotationKey = int32_t(0x311029ddu);
constexpr int32_t kEntry1RotationInvertedKey = int32_t(0xdc08b04eu);
constexpr int32_t kEntry2RotationKey = int32_t(0x14c68a1eu);
constexpr int32_t kEntry2RotationInvertedKey = int32_t(0x59d0170du);
constexpr int32_t kPrimaryWeaponPitchKey = int32_t(0xec783405u);
constexpr int32_t kSecondaryWeaponPitchKey = int32_t(0x310e0db3u);
constexpr int32_t kActiveWeaponNameKey = int32_t(0x39ec2f25u);
constexpr int32_t kInVehicleKey = int32_t(0x2202a853u);
constexpr int32_t kShortNameKey = int32_t(0x0cb77979u);
constexpr std::string_view kWeaponCompCategory = "ui/uicomponents/uiweaponcomp";
constexpr int32_t kActiveWeaponIsReloadingKey = int32_t(0x8a21ce13u);
constexpr int32_t kHeatKey = int32_t(0xf35e738eu);
constexpr int32_t kAmmoKey = int32_t(0x8d4b17b6u);
// SOURCE: notes R5-UI2 B3 (elevation = weapon pitch - hull pitch + 90; primary at precision 0, secondary truncated).
constexpr float kWeaponPitchOffset = 90.0f;

std::string fixed(double p_value, int p_precision) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", p_precision, p_value);
	return buf;
}

class VehicleSources {
public:
	void attach(const HudWidgetNode &p_node) {
		sources_.clear();
		for (const HudDataSource &s : p_node.sources) {
			if (!s.category.empty()) {
				sources_.push_back(s);
			}
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const {
		for (const HudDataSource &s : sources_) {
			if (std::optional<As2Value> v = hud_vehicle_source(p_model, p_client, s)) {
				r_out.push_back({ s.name, std::move(*v), s.on_initialize });
			} else if (p_client.flow_data) {
				if (std::optional<std::string> f = p_client.flow_data(s.category, s.key)) {
					r_out.push_back({ s.name, As2Value::of(std::move(*f)), s.on_initialize });
				}
			}
		}
	}

private:
	std::vector<HudDataSource> sources_;
};

std::string team_frame(const HudWidgetHost &p_host) {
	return hud_vehicle_russian(p_host.client()) ? "ru" : "us";
}

bool property_true(const HudWidgetInit &p_init, std::string_view p_name) {
	const std::string *v = p_init.property(p_name);
	return v != nullptr && to_lower_ascii(*v) == "true";
}

class VehicleWidget : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_.attach(p_node); }

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		sources_.bind(p_model, p_client, r_out);
	}

private:
	VehicleSources sources_;
};

class TurretDisplayClass final : public VehicleWidget {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip display = r_host.clip("turretDisplay_mc");
		display.goto_and_stop(team_frame(r_host));
		body_ = display.child("body_mc");
		turret_ = display.child("turret_mc");
		gunner_ = display.child("gunner_mc");
		for (const char *armor : { "frontArmor_mc", "backArmor_mc", "leftArmor_mc", "rightArmor_mc" }) {
			body_.child(armor).set_visible(false);
		}
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		main_turret_ = property_true(p_init, "IsMainTurret");
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "VehicleRotation") {
			body_rotation_ = p_value.to_number();
			body_.set_rotation(float(body_rotation_));
			if (!main_turret_) {
				update_turret(As2Value::undefined());
			}
		} else if (p_key == "TurretRotation") {
			update_turret(p_value);
		} else if (p_key == "GunTowerRotation") {
			gunner_rotation_ = p_value.to_number();
			gunner_.set_rotation(float(body_rotation_ - gunner_rotation_));
		}
	}

private:
	void update_turret(const As2Value &p_value) {
		if (p_value.defined()) {
			turret_rotation_ = p_value.to_number();
		}
		turret_.set_rotation(float(turret_rotation_ + body_rotation_));
	}

	As2Clip body_;
	As2Clip turret_;
	As2Clip gunner_;
	bool main_turret_ = true;
	double body_rotation_ = 0.0;
	double turret_rotation_ = 0.0;
	double gunner_rotation_ = 0.0;
};

class VehicleHudWarningsClass final : public VehicleWidget {
public:
	static constexpr uint32_t kRedColor = 16711680u;
	static constexpr uint32_t kUsColor = 10288464u;
	static constexpr uint32_t kRuColor = 16737792u;

	void on_clip_load(HudWidgetHost &r_host) override {
		vehicle_ = param_string(r_host, "m_Vehicle");
		message_type_ = param_string(r_host, "m_MessageType");
		align_ = param_string(r_host, "m_AlignStr");
		solid_color_ = hud_vehicle_russian(r_host.client()) ? kRuColor : kUsColor;
		warn_color_ = kRedColor;
		warning_ = r_host.clip("warning");
		start_x_ = r_host.self().x();
		start_w_ = r_host.self().width();
		setup_clips(r_host);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Locking") {
			if (p_value.to_number() > 0.0) {
				blink(r_host);
			} else {
				hide(r_host);
			}
		} else if (p_key == "Locked") {
			if (p_value.truthy()) {
				solid(r_host);
			} else {
				hide(r_host);
			}
		} else if (p_key == "Health") {
			update_health(r_host, p_value);
		} else if (p_key == "Disabled") {
			update_disabled(r_host, p_value);
		} else if (p_key == "Reloading") {
			reloading_ = p_value.truthy();
			if (reloading_) {
				blink(r_host);
			} else if (!heated_) {
				solid(r_host);
			}
		} else if (p_key == "Heat") {
			update_heat(r_host, p_value);
		} else if (p_key == "RadioAltitude" && p_value.defined()) {
			radio_altitude_ = as2_round(p_value.to_number());
			check_state(r_host);
		} else if (p_key == "WarnLevel" && p_value.defined()) {
			warn_level_ = p_value.to_number();
			check_state(r_host);
		}
	}

private:
	void update_health(HudWidgetHost &r_host, const As2Value &p_value) {
		if (p_value.to_number() < 100.0) {
			damaged_ = true;
			if (!disabled_) {
				blink(r_host);
				if (damage_string_) {
					set_text(r_host, *damage_string_);
				}
			} else {
				solid(r_host);
				if (disabled_string_) {
					set_text(r_host, *disabled_string_);
				}
			}
		} else {
			damaged_ = false;
			hide(r_host);
			if (damage_string_) {
				set_text(r_host, *damage_string_);
			}
		}
	}

	void update_disabled(HudWidgetHost &r_host, const As2Value &p_value) {
		if (p_value.truthy()) {
			disabled_ = true;
			solid(r_host);
			if (disabled_string_) {
				set_text(r_host, *disabled_string_);
			}
			return;
		}
		disabled_ = false;
		if (damaged_) {
			blink(r_host);
		} else {
			hide(r_host);
		}
		if (damage_string_) {
			set_text(r_host, *damage_string_);
		}
	}

	void update_heat(HudWidgetHost &r_host, const As2Value &p_value) {
		const double heat = p_value.to_number();
		if (std::isnan(heat)) {
			return;
		}
		const bool was = heated_;
		if (!(heat < 1.0)) {
			heated_ = true;
		} else if (!(heat > 0.0)) {
			heated_ = false;
		}
		if (heated_ == was) {
			return;
		}
		if (heated_) {
			blink(r_host);
		} else if (!reloading_) {
			solid(r_host);
		}
	}

	void check_state(HudWidgetHost &r_host) {
		if (!(radio_altitude_ > warn_level_)) {
			blink(r_host);
		} else {
			hide(r_host);
		}
	}

	void setup_clips(HudWidgetHost &r_host) {
		left_ = warning_.child("left");
		centre_ = warning_.child("center");
		right_ = warning_.child("right");
		if (vehicle_ == "DefaultAir") {
			style(r_host, { { "warning", "ID_H_VEHICLE_WARN" }, { "lock", "ID_H_VEHICLE_LOCK" } }, "terrain",
					"ID_H_VEHICLE_TERRAIN");
		} else if (vehicle_ == "DefaultTank" || vehicle_ == "DefaultTankGunner") {
			style(r_host, { { "warning", "ID_H_VEHICLE_WARN" }, { "lock", "ID_H_VEHICLE_LOCK" } }, "reload",
					"ID_H_VEHICLE_RDY");
		} else if (vehicle_ == "DefaultHelicopter") {
			std::string text;
			if (message_type_ == "warning") {
				set_color(warn_color_);
				damage_string_ = "ENG1 RPM";
				disabled_string_ = "ID_H_VEHICLE_HELICOPTER_WARN";
				text = *damage_string_;
			} else if (message_type_ == "lock") {
				set_color(solid_color_);
				text = "ID_H_VEHICLE_LOCK";
			}
			set_text(r_host, text);
		}
		left_.set_visible(false);
		centre_.set_visible(false);
		right_.set_visible(false);
	}

	void style(HudWidgetHost &r_host, std::initializer_list<std::pair<std::string_view, std::string_view>> p_warned,
			std::string_view p_plain_type, std::string_view p_plain_text) {
		std::string text;
		for (const auto &[type, sid] : p_warned) {
			if (message_type_ == type) {
				set_color(warn_color_);
				text = sid;
			}
		}
		if (message_type_ == p_plain_type) {
			if (p_plain_type == "terrain") {
				set_color(warn_color_);
			}
			text = p_plain_text;
			set_bg_visibility(false);
		}
		set_text(r_host, text);
	}

	void blink(HudWidgetHost &r_host) {
		if (has_bg_) {
			show_brackets(false);
		}
		check_style(r_host, true);
		warning_.goto_and_play("blink");
	}

	void solid(HudWidgetHost &r_host) {
		if (has_bg_) {
			show_brackets(true);
		}
		check_style(r_host, false);
		warning_.goto_and_play("solid");
	}

	void hide(HudWidgetHost &r_host) {
		if (has_bg_) {
			show_brackets(false);
		}
		check_style(r_host, false);
		warning_.goto_and_stop("start");
	}

	void check_style(HudWidgetHost &r_host, bool p_blink) {
		if (vehicle_ != "DefaultTank" && vehicle_ != "DefaultTankGunner") {
			return;
		}
		if (message_type_ != "reload") {
			return;
		}
		set_color(p_blink ? warn_color_ : solid_color_);
		set_text(r_host, p_blink ? "ID_H_VEHICLE_WAIT" : "ID_H_VEHICLE_RDY");
	}

	void show_brackets(bool p_visible) const {
		left_.set_visible(p_visible);
		centre_.set_visible(p_visible);
		right_.set_visible(p_visible);
	}

	void set_bg_visibility(bool p_visible) {
		has_bg_ = p_visible;
		show_brackets(p_visible);
	}

	void set_color(uint32_t p_rgb) const { warning_.set_rgb(p_rgb); }

	void set_text(HudWidgetHost &r_host, std::string_view p_text) const {
		const As2Clip txt = warning_.child("textHolder").child("txt");
		txt.set_text(r_host.translate(p_text));
		txt.set_width(float(as2_round(txt.text_width() + 20.0f)));
		const double width = as2_round(txt.width()) + 5.0;
		if (has_bg_) {
			centre_.set_x(float(as2_round(left_.width())));
			centre_.set_width(float(width - (as2_round(centre_.x()) + as2_round(right_.width()))));
			right_.set_x(float(as2_round(centre_.x()) + as2_round(centre_.width())));
		}
		const As2Clip self = r_host.self();
		const double grown = double(self.width()) - double(start_w_);
		double x = as2_round(start_x_ - grown * 0.5);
		if (align_ == "Left") {
			x = as2_round(start_x_);
		} else if (align_ == "Right") {
			x = as2_round(start_x_ - grown);
		}
		self.set_x(float(x));
	}

	As2Clip warning_;
	As2Clip left_;
	As2Clip centre_;
	As2Clip right_;
	std::string vehicle_;
	std::string message_type_;
	std::string align_;
	std::optional<std::string> damage_string_;
	std::optional<std::string> disabled_string_;
	uint32_t solid_color_ = kUsColor;
	uint32_t warn_color_ = kRedColor;
	float start_x_ = 0.0f;
	float start_w_ = 0.0f;
	bool has_bg_ = true;
	bool disabled_ = false;
	bool damaged_ = false;
	bool reloading_ = false;
	bool heated_ = false;
	double radio_altitude_ = 0.0;
	double warn_level_ = 50.0;
};

class VehicleZoomFlirClass final : public VehicleWidget {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		mc_ = r_host.clip("vehicle_zoom_flir_mc");
		mc_.goto_and_stop(team_frame(r_host));
		set_text(r_host, "", 0);
		set_text(r_host, "", 1);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		always_active_ = property_true(p_init, "ZoomStateAlwaysActive");
		if (always_active_) {
			update(r_host, "ZoomEnabled", As2Value::of(true));
		}
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "ZoomEnabled") {
			zoom_enabled_ = p_value.defined() && p_value.truthy();
			set_start_text(r_host, zoom_enabled_);
		} else if (p_key == "FlirEnabled") {
			flir_enabled_ = p_value.defined() && p_value.truthy();
			set_start_text(r_host, flir_enabled_);
		} else if (p_key == "ZoomActive") {
			const char *text = string_true(p_value) ? "ID_H_3X" : zoom_enabled_ || flir_enabled_ ? "ID_H_1X" : "";
			set_text(r_host, text, 0);
		} else if (p_key == "FlirActive") {
			const char *text = string_true(p_value) ? "ID_H_FLIR" : zoom_enabled_ || flir_enabled_ ? "ID_H_60HZ" : "";
			set_text(r_host, text, 1);
		}
	}

private:
	void set_start_text(HudWidgetHost &r_host, bool p_show) {
		if (p_show) {
			set_text(r_host, "ID_H_1X", 0);
			set_text(r_host, "ID_H_60HZ", 1);
			return;
		}
		if (zoom_enabled_ || always_active_ || flir_enabled_) {
			return;
		}
		set_text(r_host, "", 0);
		set_text(r_host, "", 1);
	}

	void set_text(HudWidgetHost &r_host, std::string_view p_text, int p_index) const {
		mc_.child(p_index == 0 ? "txt" : "txt2").set_text(r_host.translate(p_text));
	}

	As2Clip mc_;
	bool always_active_ = false;
	bool zoom_enabled_ = false;
	bool flir_enabled_ = false;
};

class VehicleWeaponUnlockDisplayClass final : public VehicleWidget {
public:
	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		r_host.clip("weaponUnlockNormal").set_visible(false);
		holder_ = r_host.clip("weaponUnlockTank");
		const std::string align = param_string(r_host, "m_align");
		align_ = align == "undefined" ? "left" : align;
		holder_.child("txt").set_auto_size(auto_size(align_));
		team_ = team_frame(r_host);
		holder_.goto_and_stop(team_);
		holder_.child("weaponState").child("unlockState").goto_and_stop(team_);
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		static constexpr std::array<std::array<std::string_view, 5>, 4> kUnlocks{ {
				{ "SmokeActive", "SMOKE", "ID_H_VEHICLE_SMOKE_NAME", "ID_H_VEHICLE_SMOKE_READY",
						"ID_H_VEHICLE_SMOKE_WAIT" },
				{ "JammerActive", "JAMMER", "ID_H_VEHICLE_JAMMER_NAME", "ID_H_VEHICLE_JAMMER_READY",
						"ID_H_VEHICLE_JAMMER_WAIT" },
				{ "FlaresActive", "FLARES", "ID_H_VEHICLE_FLARES_NAME", "ID_H_VEHICLE_FLARES_READY",
						"ID_H_VEHICLE_FLARES_WAIT" },
				{ "FireExtinguisherActive", "EXTINGIUSHER", "ID_H_VEHICLE_EXTINGUISHER_NAME",
						"ID_H_VEHICLE_EXTINGUISHER_READY", "ID_H_VEHICLE_EXTINGUISHER_WAIT" },
		} };
		for (const auto &u : kUnlocks) {
			if (p_key == u[0]) {
				if (p_value.defined() && as2_equals_true(p_value)) {
					unlock_ = u;
					holder_.child("txt").set_text(r_host.translate(u[2]));
					set_state_text(r_host, false);
					r_host.self().set_visible(p_value.truthy());
				}
				return;
			}
		}
		if (p_key == "UnlockWeaponReloading") {
			const As2Clip state = holder_.child("weaponState");
			if (p_value.defined() && p_value.truthy()) {
				state.goto_and_play("blink");
				set_state_text(r_host, true);
			} else {
				state.goto_and_stop("solid");
				set_state_text(r_host, false);
				state.child("unlockState").goto_and_stop(team_);
			}
		}
	}

private:
	static SwfTextAutoSize auto_size(std::string_view p_align) {
		if (p_align == "left") {
			return SwfTextAutoSize::Left;
		}
		if (p_align == "center") {
			return SwfTextAutoSize::Center;
		}
		return p_align == "right" ? SwfTextAutoSize::Right : SwfTextAutoSize::None;
	}

	void set_state_text(HudWidgetHost &r_host, bool p_reloading) const {
		const As2Clip txt = holder_.child("weaponState").child("unlockState").child("txt");
		txt.set_text(unlock_ ? r_host.translate((*unlock_)[p_reloading ? 4 : 3]) : std::string());
		txt.set_auto_size(auto_size(align_));
	}

	As2Clip holder_;
	std::string align_ = "left";
	std::string team_ = "us";
	std::optional<std::array<std::string_view, 5>> unlock_;
};

class VehicleWeaponNameClass final : public VehicleWidget {
public:
	static constexpr float kTfDistance = 10.0f;
	static constexpr float kTextSize = 12.0f;

	void on_clip_load(HudWidgetHost &r_host) override {
		text_mc_ = r_host.clip("textHolder.textMc");
		text_mc_.goto_and_stop(team_frame(r_host));
		tf_ = text_mc_.child("txtDisplay");
		tf2_ = text_mc_.child("txtDisplay2");
		tf2_.set_visible(false);
		start_x_ = r_host.self().x();
		align_ = param_string(r_host, "m_AlignStr");
		const std::string type = param_string(r_host, "VehicleType");
		if (type == "Tank" || type == "Jet" || type == "Helicopter") {
			tf_.set_text_size(kTextSize);
			tf2_.set_text_size(kTextSize);
		}
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "WeaponName") {
			update_weapon_name(r_host, p_value);
		} else if (p_key == "Reloading") {
			reloading_ = p_value.truthy();
			if (reloading_) {
				blink(r_host, false);
			} else if (!heated_) {
				hide(r_host);
			}
		} else if (p_key == "Heat") {
			update_heat(r_host, p_value);
		} else if ((p_key == "Ammo" && use_ammo_) || (p_key == "Range" && use_range_)) {
			tf2_.set_text(p_value.to_string());
			tf2_.set_visible(true);
		}
	}

private:
	void update_weapon_name(HudWidgetHost &r_host, const As2Value &p_value) {
		use_ammo_ = false;
		std::string name = p_value.to_string();
		std::optional<std::string> second;
		const auto is = [&](std::initializer_list<std::string_view> p_names) {
			return std::find(p_names.begin(), p_names.end(), name) != p_names.end();
		};
		const bool base_text = tf2_.text() == "baseText0";
		if (is({ "ID_P_VWNAME_AH1ZRKT", "ID_P_VWNAME_MI28RKT" })) {
			use_ammo_ = true;
			if (base_text) {
				second = "0000";
			}
		} else if (is({ "ID_P_VWNAME_AH1ZAIM9" })) {
			second = "ID_H_VEHICLE_STB";
		} else if (is({ "ID_P_VWNAME_AH6LD", "ID_P_VWNAME_AH1ZLD" })) {
			use_range_ = true;
			if (base_text) {
				second = "-";
			}
		} else if (is({ "ID_P_VWNAME_AH1ZCANNON", "ID_P_VWNAME_MI28CANNON" })) {
			name = "ID_H_VEHICLE_ROUNDS";
			use_ammo_ = true;
			if (base_text) {
				second = "0000";
			}
		} else if (is({ "ID_P_VWNAME_AH1ZHELLFIRE", "ID_P_VWNAME_MI28HELLFIRE" })) {
			second = "ID_H_VEHICLE_STB";
		} else if (is({ "ID_P_VWNAME_AH6GAU2", "ID_P_VWNAME_AH6HELLFIRE", "ID_P_VWNAME_M1A2CANNON",
						   "ID_P_VWNAME_M1A2COAXLMG", "ID_P_VWNAME_LAV25CANNON", "ID_P_VWNAME_LAV25COAX",
						   "ID_P_VWNAME_LAV25TOW", "ID_P_VWNAME_LAV25APFS", "ID_P_VWNAME_LAV25HMG",
						   "ID_P_VWNAME_LAV25HELLFIRE", "ID_P_VWNAME_BMP2COAX", "ID_P_XP5_VWNAME_TVGM",
						   "ID_P_XP5_VWNAME_MINES" })) {
			second = "ID_H_VEHICLE_RDY";
			reload_states_ = true;
		}
		weapon_name_ = name;
		tf_.set_text(r_host.translate(weapon_name_));
		tf_.set_height(tf_.text_height() + 5.0f);
		tf_.set_width(tf_.text_width() + 5.0f);
		if (second) {
			tf2_.set_text(r_host.translate(*second));
			tf2_.set_height(tf2_.text_height() + 5.0f);
			tf2_.set_x(tf_.text_width() + kTfDistance);
			tf2_.set_width(tf2_.text_width() + 5.0f);
			tf2_.set_visible(true);
		}
		const As2Clip self = r_host.self();
		if (align_ == "Center") {
			self.set_x(float(as2_round(start_x_ - (second ? self.width() : tf_.text_width()) * 0.5)));
		} else if (align_ == "Right") {
			const float right = second ? tf2_.x() + tf2_.text_width() : tf_.x() + tf_.text_width();
			self.set_x(float(as2_round(start_x_ - (right + 5.0f))));
		}
	}

	void update_heat(HudWidgetHost &r_host, const As2Value &p_value) {
		const double heat = p_value.to_number();
		if (std::isnan(heat)) {
			return;
		}
		const bool was_heated = heated_;
		const bool was_heating = heating_;
		heated_ = !(heat < 1.0);
		heating_ = heat < 1.0 && heat > 0.0;
		if (heated_) {
			if (!was_heated) {
				blink(r_host, false);
			}
		} else if (heating_) {
			if (!was_heating) {
				blink(r_host, true);
			}
		} else if (!reloading_) {
			hide(r_host);
		}
	}

	void blink(HudWidgetHost &r_host, bool p_green) {
		r_host.clip("textHolder").goto_and_play(p_green ? "greenBlink" : "blink");
		text_mc_.goto_and_stop(team_frame(r_host));
		refill(r_host, p_green ? "ID_H_VEHICLE_RDY" : "ID_H_VEHICLE_WAIT");
	}

	void hide(HudWidgetHost &r_host) {
		r_host.clip("textHolder").goto_and_stop("start");
		refill(r_host, "ID_H_VEHICLE_RDY");
	}

	void refill(HudWidgetHost &r_host, std::string_view p_state) {
		tf_.set_text(r_host.translate(weapon_name_));
		tf2_.set_text("");
		if (reload_states_) {
			tf2_.set_text(r_host.translate(p_state));
			tf2_.set_width(tf2_.text_width() + 5.0f);
		}
	}

	As2Clip text_mc_;
	As2Clip tf_;
	As2Clip tf2_;
	std::string align_;
	std::string weapon_name_;
	float start_x_ = 0.0f;
	bool reload_states_ = false;
	bool reloading_ = false;
	bool heated_ = false;
	bool heating_ = false;
	bool use_ammo_ = false;
	bool use_range_ = false;
};

class VehicleStaticArtClass final : public VehicleWidget {
public:
	void construct(HudWidgetHost &r_host) override {
		r_host.clip("DefaultTank_AmmoLine").set_visible(false);
		r_host.clip("DefaultTankGunner_RangeBrackets").set_visible(false);
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		type_ = param_string(r_host, "m_type");
		if (type_ == "undefined") {
			type_ = "DefaultTank_AmmoLine";
		}
		r_host.clip(type_).goto_and_stop(team_frame(r_host));
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		r_host.clip(type_).set_visible(true);
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

private:
	std::string type_;
};

class VehicleGunnerWeaponElevationClass final : public VehicleWidget {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		const std::string team = team_frame(r_host);
		r_host.clip("ArrowMc").goto_and_stop(team);
		r_host.clip("bar").goto_and_stop(team);
		start_h_ = r_host.self().height();
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "TurretElevation") {
			const double drop = double(start_h_) * (p_value.to_number() / 180.0);
			r_host.clip("ArrowMc").set_y(float(double(start_h_) - drop));
		}
	}

private:
	float start_h_ = 0.0f;
};

struct CrosshairKind {
	std::string_view type;
	std::string_view clip;
	bool team_frames = false;
	bool zoom_brackets = false;
	bool blinks = false;
};

constexpr CrosshairKind kCrosshairs[] = {
	{ "ThirdPersonTank", "Vehicle_3p", false, true },
	{ "ThirdPersonTankGrade", "Vehicle_3p_grade", false, true },
	{ "SOFLAM", "SOFLAM", false, false },
	{ "EOD", "EOD", false, false },
	{ "Javelin", "Javelin", false, false },
	{ "DefaultHelicopter", "HeliDefaultCross", false, false },
	{ "HelliGunner", "HeliGunner", false, true, true },
	{ "DefaultJet", "jetDefaultCross", false, false },
	{ "ID_P_VWNAME_F18VULCAN", "jetDefaultCross", false, false },
	{ "ID_P_VWNAME_A10VULCAN", "jetDefaultCross", false, false },
	{ "ID_P_VWNAME_F18AIM9", "F18AIM9", false, false },
	{ "ID_P_VWNAME_A10AIM9", "F18AIM9", false, false },
	{ "MFD", "MFD", false, false, true },
	{ "SP_MFD", "SP_MFD", true, true, true },
	{ "ID_P_VWNAME_SPMFDCAS", "SP_MFD_CAS", false, true, true },
	{ "ID_P_VWNAME_SPMFDHARM", "SP_MFD_HARM", false, true, true },
	{ "ID_P_VWNAME_SPMFDJDAM", "SP_MFD_JDAM", false, true, true },
	{ "MFDMissile", "MFDMissile", false, false },
	{ "ID_P_VWNAME_SU25VULCAN", "jetDefaultCross", false, false },
	{ "ID_P_VWNAME_SU35VULCAN", "jetDefaultCross", false, false },
	{ "ID_P_VWNAME_SU25AIM9", "F18AIM9", false, false },
	{ "ID_P_VWNAME_SU35AIM9", "F18AIM9", false, false },
	{ "M242_HE", "M242_HE", true, true },
	{ "ID_P_VWNAME_LAV25CANNON", "M242_HE", true, true },
	{ "ID_P_VWNAME_LAV25COAX", "M240_762", true, true },
	{ "ID_P_VWNAME_LAV25APFS", "AFPSDS_T", true, true },
	{ "ID_P_VWNAME_LAV25TOW", "BGM_71", false, true },
	{ "ID_P_VWNAME_LAV25HMG", "M2_50_BMG", true, true },
	{ "ID_P_VWNAME_LAV25HELLFIRE", "FGM_148", false, true },
	{ "ID_P_VWNAME_LAVADCANNON", "GAU_12", true, true },
	{ "ID_P_VWNAME_LAVADAA", "FIM_92", true, true },
	{ "ID_P_VWNAME_BMP2CANNON", "M242_HE", true, true },
	{ "ID_P_VWNAME_BMP2COAX", "M240_762", true, true },
	{ "ID_P_VWNAME_BMP2APFS", "AFPSDS_T", true, true },
	{ "ID_P_VWNAME_BMP2TOW", "BGM_71", true, true },
	{ "ID_P_VWNAME_BMP2HELLFIRE", "FGM_148", false, true },
	{ "ID_P_VWNAME_TUNGAA", "FIM_92", true, true },
	{ "ID_P_VWNAME_BMP2HMG", "M2_50_BMG", true, true },
	{ "ID_P_VWNAME_TUNGCANNON", "GAU_12", true, true },
	{ "ID_P_VWNAME_M1A2CANNON", "AFPSDS_T", true, true },
	{ "ID_P_VWNAME_M1A2COAXLMG", "M240_762", true, true },
	{ "ID_P_VWNAME_M1A2COAXHMG", "M2_50_BMG", true, true },
	{ "ID_P_VWNAME_M1A2GUNNER", "M2_50_BMG", true, true },
	{ "ID_P_VWNAME_M1A2STAFF", "FGM_148", false, true },
	{ "ID_P_VWNAME_M1A2CANISTER", "M1028", false, true },
	{ "ID_P_VWNAME_AAVHMG", "M240_762", false, false },
	{ "ID_P_VWNAME_AAVGMG", "M1028", false, false },
	{ "ID_P_VWNAME_T90COAXLMG", "M240_762", true, true },
	{ "ID_P_VWNAME_T90CANNON", "AFPSDS_T", true, true },
	{ "ID_P_VWNAME_T90STAFF", "FGM_148", false, true },
	{ "ID_P_VWNAME_T90COAXHMG", "M2_50_BMG", true, true },
	{ "ID_P_VWNAME_T90GUNNER", "M2_50_BMG", true, true },
	{ "ID_P_VWNAME_T90CANISTER", "M1028", false, true },
	{ "ID_P_VWNAME_SPRUTSDHE", "AFPSDS_T", true, true },
	{ "HELLI_TEST_CROSS", "HeliDefaultCross", true, true },
	{ "ID_P_VWNAME_AH1ZGRKT", "HeliDefaultCross", true, true },
	{ "ID_P_VWNAME_AH1ZRKT", "HeliDefaultCross", true, true },
	{ "ID_P_VWNAME_AH1ZAIM9", "HeliAIM9", false, true },
	{ "ID_P_VWNAME_AH1ZCANNON", "HeliGunner", false, true, true },
	{ "ID_P_VWNAME_AH1ZHELLFIRE", "HeliGunnerHellfire", false, true, true },
	{ "ID_P_VWNAME_AH6FIM", "HeliAIM9", false, true },
	{ "ID_P_VWNAME_AH6GAU2", "HeliDefaultCross", false, true },
	{ "ID_P_VWNAME_AH6HELLFIRE", "HeliDefaultCross", false, true },
	{ "ID_P_VWNAME_MI28RKT", "HeliDefaultCross", true, true },
	{ "ID_P_VWNAME_MI28GRKT", "HeliDefaultCross", true, true },
	{ "ID_P_VWNAME_MI28AIM9", "HeliAIM9", false, true },
	{ "ID_P_VWNAME_MI28CANNON", "HeliGunner", false, true, true },
	{ "ID_P_VWNAME_MI28HELLFIRE", "HeliGunnerHellfire", false, true, true },
	{ "ID_P_VWNAME_Z11FIM", "HeliAIM9", false, true },
	{ "ID_P_VWNAME_Z11GAU2", "HeliDefaultCross", false, true },
	{ "ID_P_VWNAME_Z11HELLFIRE", "HeliDefaultCross", false, true },
	{ "ID_P_VWNAME_EODREPAIR", "EOD", false, false },
	{ "ID_P_VWNAME_MAVLD", "MAV", false, false },
	{ "ID_P_XP5_VWNAME_MINES", "SP_MFD_JDAM", false, true, true },
};

const CrosshairKind *crosshair_kind(std::string_view p_type) {
	for (const CrosshairKind &k : kCrosshairs) {
		if (k.type == p_type) {
			return &k;
		}
	}
	return nullptr;
}

constexpr std::string_view kCrosshairFirstFrameHidden[] = { "hitIndicator", "ZoomBrackets", "Vehicle_3p",
	"Vehicle_3p_grade", "M242_HE", "M240_762", "AFPSDS_T", "BGM_71", "FGM_148", "M2_50_BMG", "M1028", "GAU_12",
	"FIM_92", "HeliDefaultCross", "HeliGunner", "HeliGunnerHellfire", "HeliAIM9", "jetDefaultCross", "F18AIM9", "MFD",
	"MFDMissile", "SP_MFD", "SP_MFD_CAS", "SP_MFD_HARM", "SP_MFD_JDAM", "SOFLAM", "EOD", "Javelin", "MAV" };

// WHY: VehicleCrosshair's and VehicleStaticArt's first-frame scripts hide every crosshair type and art type; construct()
// runs them, so the AS2 shows only the selected one.
class VehicleCrosshairClass final : public VehicleWidget {
public:
	static constexpr double kShootBlinkSeconds = 1.0;

	void construct(HudWidgetHost &r_host) override {
		for (const std::string_view name : kCrosshairFirstFrameHidden) {
			r_host.clip(name).set_visible(false);
		}
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		team_ = std::string(team_frame(r_host)) + "_";
		r_host.clip("ZoomBrackets").goto_and_stop(team_ + "zoomBrackets");
		r_host.clip("hitIndicator").goto_and_stop("idle");
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		set_crosshair(r_host, param_string(r_host, "m_crosshairType"));
		if (p_init.property("ZoomStateAlwaysActive") != nullptr) {
			always_active_ = property_true(p_init, "ZoomStateAlwaysActive");
		}
		if (always_active_) {
			update(r_host, "ZoomEnabled", As2Value::of(true));
		}
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "WeaponName") {
			if (crosshair_kind(p_value.to_string()) != nullptr) {
				set_crosshair(r_host, p_value.to_string());
			}
		} else if (p_key == "ZoomEnabled") {
			const bool on = p_value.defined() && as2_equals_true(p_value);
			zoom_enabled_ = on || always_active_;
			const As2Clip brackets = r_host.clip("ZoomBrackets");
			if (!zoom_enabled_) {
				brackets.set_visible(false);
			} else if (show_zoom_brackets_) {
				brackets.set_visible(true);
			}
		} else if (p_key == "ZoomActive") {
			zoom_active_ = string_true(p_value);
			const As2Clip brackets = r_host.clip("ZoomBrackets");
			if (zoom_active_ && zoom_enabled_) {
				brackets.set_visible(false);
			} else if (!zoom_active_ && zoom_enabled_ && show_zoom_brackets_) {
				brackets.set_visible(true);
			}
			set_crosshair(r_host, type_);
		} else if (p_key == "Reloading") {
			const bool blinks = kind_ != nullptr && kind_->blinks;
			if (p_value.defined() && as2_equals_true(p_value)) {
				if (blinks) {
					blink(r_host);
				} else {
					r_host.self().set_visible(false);
				}
			} else if (blinks) {
				stop_blink(r_host);
			} else {
				r_host.self().set_visible(true);
			}
		} else if (p_key == "Ammo") {
			blink(r_host);
			if (shoot_interval_ != 0) {
				r_host.clear_interval(shoot_interval_);
			}
			shoot_interval_ = r_host.set_interval(kShootBlinkSeconds);
		} else if (p_key == "Busy") {
			const bool busy = string_true(p_value);
			if (busy) {
				if (type_ == "ID_P_VWNAME_SPMFDCAS") {
					r_host.self().set_visible(false);
				} else {
					blink(r_host);
				}
			} else if (type_ == "ID_P_VWNAME_SPMFDCAS") {
				r_host.self().set_visible(true);
			} else {
				stop_blink(r_host);
			}
		} else if (p_key == "IndicatorValue") {
			if (playing_sp_) {
				return;
			}
			const As2Clip hit = r_host.clip("hitIndicator");
			if (!p_value.defined() || p_value.to_number() == 0.0) {
				hit.goto_and_stop("idle");
				return;
			}
			hit.set_visible(true);
			hit.goto_and_play("level" + p_value.to_string());
		} else if (p_key == "PlayingSp") {
			playing_sp_ = p_value.defined() && string_true(p_value);
			r_host.clip("hitIndicator").set_visible(!playing_sp_);
		}
	}

	void interval(HudWidgetHost &r_host, uint32_t p_id) override {
		if (p_id == shoot_interval_) {
			stop_blink(r_host);
		}
	}

	const std::string &crosshair_type() const { return type_; }

private:
	void set_crosshair(HudWidgetHost &r_host, const std::string &p_type) {
		if (kind_ != nullptr) {
			r_host.clip(kind_->clip).set_visible(false);
		}
		type_ = p_type;
		kind_ = crosshair_kind(p_type);
		if (kind_ == nullptr) {
			return;
		}
		const As2Clip clip = r_host.clip(kind_->clip);
		clip.set_visible(true);
		if (!kind_->team_frames) {
			return;
		}
		clip.goto_and_stop(team_ + (zoom_active_ ? "zoomed" : "neutral"));
		show_zoom_brackets_ = kind_->zoom_brackets;
	}

	void blink(HudWidgetHost &r_host) const {
		if (kind_ == nullptr || !kind_->blinks) {
			return;
		}
		const std::string state = kind_->team_frames ? (zoom_active_ ? "zoomed_" : "neutral_") : "";
		r_host.clip(kind_->clip).goto_and_play(team_ + state + "blink");
	}

	void stop_blink(HudWidgetHost &r_host) const {
		if (kind_ == nullptr || !kind_->blinks) {
			return;
		}
		const std::string state = kind_->team_frames ? (zoom_active_ ? "zoomed" : "neutral") : "";
		r_host.clip(kind_->clip).goto_and_stop(team_ + state);
	}

	std::string team_ = "us_";
	std::string type_;
	const CrosshairKind *kind_ = nullptr;
	bool always_active_ = false;
	bool zoom_enabled_ = false;
	bool zoom_active_ = false;
	bool show_zoom_brackets_ = false;
	bool playing_sp_ = false;
	uint32_t shoot_interval_ = 0;
};

}

namespace {

// INFERRED: while seated, UIWeaponComp's Reloading, Heat and Ammo are the seat weapon's; notes R5-UI2 D leaves the two
// key sets unassigned. Settle: a note assigning UIWeaponComp's key sets.
std::optional<As2Value> hud_seat_weapon_source(const HudVehicle &p_vehicle, int32_t p_key) {
	if (!p_vehicle.in_vehicle || !p_vehicle.seat_weapon) {
		return std::nullopt;
	}
	switch (p_key) {
		case kActiveWeaponIsReloadingKey:
			return As2Value::of(p_vehicle.seat_weapon_reloading);
		case kHeatKey:
			return As2Value::of(double(p_vehicle.seat_weapon_heat));
		case kAmmoKey:
			return As2Value::of(p_vehicle.seat_weapon_ammo);
		default:
			return std::nullopt;
	}
}

}

std::optional<As2Value> hud_vehicle_source(
		const HudModel &p_model, const HudClientState &p_client, const HudDataSource &p_source) {
	if (p_source.category == kWeaponCompCategory) {
		return hud_seat_weapon_source(p_model.vehicle, p_source.key);
	}
	if (p_source.category != kVehicleCompCategory) {
		return std::nullopt;
	}
	const HudVehicle &v = p_model.vehicle;
	// SOURCE: notes R5-UI2 B3 and its checker (EntryN Rotation is -(entry yaw - hull yaw), Inverted unnegated, precision 1).
	// INFERRED: the yaw sign, the view yaw the compass negates. Settle: footage of a turned turret.
	const double inverted[2] = { double(v.entry_yaw_deg[0] - v.hull_yaw_deg),
		double(v.entry_yaw_deg[1] - v.hull_yaw_deg) };
	switch (p_source.key) {
		case kInVehicleKey:
			return As2Value::of(v.in_vehicle);
		// SOURCE: notes R5-UI2 B3 (Health is an integer percent, truncated).
		case kVehicleHealthKey:
			return As2Value::of(std::trunc(double(v.health)));
		case kDamageStateDisabledKey:
			return As2Value::of(v.damage_state_disabled);
		// INFERRED: ShortName is VehicleEntityData.NameSid; fb.HelperFunctions' twelve Russian names are NameSids.
		// Settle: a note on the ShortName field.
		case kShortNameKey:
			return As2Value::of(hud_string(v.short_name));
		case kVelocityKey:
			return v.in_vehicle ? std::optional<As2Value>(As2Value::of(v.speed_kmh)) : std::nullopt;
		case kEntry1RotationKey:
			return As2Value::of(fixed(-inverted[0], 1));
		case kEntry1RotationInvertedKey:
			return As2Value::of(fixed(inverted[0], 1));
		case kEntry2RotationKey:
			return As2Value::of(fixed(-inverted[1], 1));
		case kEntry2RotationInvertedKey:
			return As2Value::of(fixed(inverted[1], 1));
		case kPrimaryWeaponPitchKey:
			if (v.weapon_orientations && v.primary_pitch_deg) {
				return As2Value::of(fixed(double(*v.primary_pitch_deg) + kWeaponPitchOffset, 0));
			}
			return std::nullopt;
		case kSecondaryWeaponPitchKey:
			if (v.weapon_orientations && v.secondary_pitch_deg) {
				return As2Value::of(std::trunc(double(*v.secondary_pitch_deg)) + kWeaponPitchOffset);
			}
			return std::nullopt;
		// INFERRED: ActiveWeaponName is the uivehicleweaponmetadata Name whose ItemIds hold the seat weapon's hash.
		// Settle: a note on ActiveWeaponName's lookup.
		case kActiveWeaponNameKey: {
			const ItemIcon *item =
					p_client.items != nullptr && v.weapon_item != 0 ? p_client.items->find(v.weapon_item) : nullptr;
			if (item == nullptr || item->source != ItemIconSource::VehicleWeapon) {
				return std::nullopt;
			}
			return As2Value::of(item->name_sid);
		}
		default:
			return std::nullopt;
	}
}

void register_hud_widgets_vehicle_screens(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.TurretDisplay", [] { return std::make_unique<TurretDisplayClass>(); });
	r_registry.add("Widget.Hud.VehicleHudWarnings", [] { return std::make_unique<VehicleHudWarningsClass>(); });
	r_registry.add("Widget.Hud.VehicleZoomFlir", [] { return std::make_unique<VehicleZoomFlirClass>(); });
	r_registry.add("Widget.Hud.VehicleWeaponUnlockDisplay",
			[] { return std::make_unique<VehicleWeaponUnlockDisplayClass>(); });
	r_registry.add("Widget.Hud.VehicleWeaponName", [] { return std::make_unique<VehicleWeaponNameClass>(); });
	r_registry.add("Widget.Hud.VehicleStaticArt", [] { return std::make_unique<VehicleStaticArtClass>(); });
	r_registry.add("Widget.Hud.VehicleGunnerWeaponElevation",
			[] { return std::make_unique<VehicleGunnerWeaponElevationClass>(); });
	r_registry.add("Widget.Crosshair.VehicleCrosshair", [] { return std::make_unique<VehicleCrosshairClass>(); });
}

}

#pragma once

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_messages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

// SOURCE: the AS2 class fb.Defines (bf3dump swf as2 actionscriptlibrary): DEFAULT_HUD_PERSPECTIVE 25.
inline constexpr float kHudPerspectiveFov = 25.0f;
inline constexpr std::string_view kBgDrawRenderer = "t2;iBgDraw";
inline constexpr std::string_view kSoldierRenderer = "t2;iUISoldierCompRender";
inline constexpr std::string_view kMinimapRenderer = "t2;iUIMinimapRender";
inline constexpr std::string_view kSpawnRowType = "baseSpawn";
inline constexpr std::string_view kSpawnRowClass = "Widget.List.Rows.BaseSpawn";
// INFERRED: a row without a vehicle shows the VehicleIcon frame Blank, read off the 18.57 footage.
// Settle: a note on UISpawnPointComp's VehicleIcon value.
inline constexpr std::string_view kSpawnRowNoVehicleIcon = "Blank";
inline constexpr std::string_view kSquadSpawnRowType = "squadSpawn";
// STAND-IN: no voice chat is modelled, so Voip shows frame 2, taken as the icon's empty frame. Settle: build voice state.
inline constexpr int32_t kSquadSpawnVoipOffFrame = 2;
// SOURCE: the AS2 class Widget.Hud.Ammo: m_firingModes, the same six names in this order.
inline constexpr std::array<std::string_view, 6> kAs2FiringModes{ "single", "single", "auto", "burst", "auto", "single" };
// SOURCE: the AS2 class Shared.HudBackground: HORIZONTAL_FADE_PERCENTAGE 20, VERTICAL_FADE_PERCENTAGE 4.25.
inline constexpr double kHorizontalFadePercentage = 20.0;
inline constexpr double kVerticalFadePercentage = 4.25;

class HudBackgroundApi {
public:
	virtual ~HudBackgroundApi() = default;
	virtual void set_width(double p_value, std::string_view p_dir) = 0;
	virtual void set_height(double p_value, std::string_view p_dir) = 0;
	virtual double start_width() const = 0;
	virtual double start_height() const = 0;
};

class HudBackgroundWidgetApi {
public:
	virtual ~HudBackgroundWidgetApi() = default;
	virtual void initialize_without_data() = 0;
	virtual void set_width(double p_value) = 0;
	virtual void set_height(double p_value) = 0;
	virtual double width_value() const = 0;
	virtual double height_value() const = 0;
	virtual bool hardcore() const = 0;
	virtual std::string tilt_mode() const = 0;
};

class HudMessageApi {
public:
	virtual ~HudMessageApi() = default;
	virtual uint64_t rows_added() const = 0;
	virtual size_t rows() const = 0;
	virtual double max_row_count() const = 0;
	virtual HudMessageSource source() const = 0;
};

enum class HudTagComponent : uint8_t {
	TeamSupport,
	Mapmarker,
	Alert,
	Capturepoint,
};

struct HudWorldTag {
	uint32_t id = 0;
	HudTagComponent component = HudTagComponent::Mapmarker;
	float x = 0.0f;
	float y = 0.0f;
	float scale = 1.0f;
	float alpha = 1.0f;
	HudData data;
};

struct HudWorldTags {
	std::vector<HudWorldTag> tags;
};

class HudIcon3DManagerApi {
public:
	virtual ~HudIcon3DManagerApi() = default;
	virtual HudTagComponent tag_component() const = 0;
	virtual const std::string &published_path() const = 0;
	virtual double published_key() const = 0;
	virtual const std::string &component_name() const = 0;
	virtual std::vector<std::string> tag_paths() const = 0;
};

// INFERRED: every member of this block is fitted to the 18.57 footage (premultiplied light, coverage and the ramps),
// as the engine draws iBgDraw. Settle: a note on the iBgDraw renderer.
struct HudBgDrawLook {
	std::array<float, 3> premul_rgb255{ 0.91f, 31.7f, 54.12f };
	float coverage = 0.1458f;
	float light_start_of_fade = 0.27f;
	float light_full_of_fade = 1.05f;
	float coverage_start_of_fade = 0.26f;
	float coverage_full_of_fade = 0.73f;
};

inline constexpr HudBgDrawLook kHudBgDrawLook{};

// INFERRED: every member of this block is read off the 18.57 footage at full health; the engine draws the EKG.
// Settle: a note on the EKG renderer and whether it follows health.
struct HudEkgLook {
	float rect_x0 = 1056.0f;
	float rect_y0 = 566.0f;
	float rect_x1 = 1166.0f;
	float rect_y1 = 592.0f;
	float scroll_px_per_second = 78.7f;
	float period_seconds = 1.04f;
	float baseline_y = 578.0f;
	float rise_px = 5.0f;
	float peak_px = 9.0f;
	float fall_px = 5.0f;
	float undershoot_px = 3.0f;
	float recover_px = 8.0f;
	float line_px = 2.0f;
	std::array<uint8_t, 3> rgb{ 201, 221, 232 };
	float fade_start_x = 1055.0f;
	float fade_full_x = 1150.0f;
};

inline constexpr HudEkgLook kHudEkgLook{};

// INFERRED: read off the 18.57 footage at MapZoom 0, the small map InitSmall starts with.
// Settle: a note on the minimap renderer's scale.
inline constexpr float kMinimapMetresPerLocalUnit = 1.375f;

// INFERRED: the (1 + MapZoom) factor, from the MapZoom steps 0, 0.4 and 1.2, not seen in footage.
// Settle: footage of the zoomed large map.
constexpr float minimap_metres_per_unit(float p_zoom) {
	return kMinimapMetresPerLocalUnit * (1.0f + (p_zoom > 0.0f ? p_zoom : 0.0f));
}

// INFERRED: the footage's minimap shows no slope on the satellite texture. Settle: a note on the minimap shader.
inline constexpr float kMinimapSatelliteWeight = 0.0f;
// INFERRED: the minimap square in local units, read off the 18.57 footage (the AS2 HudBackgroundWidget's MINIMAP_SIZE is
// also 170). Settle: a note on the minimap render target.
inline constexpr float kMinimapSquareLocal = 170.0f;
inline constexpr std::string_view kMinimapMaskTexture = "ui/hud/minimap/minimapmask";
// SOURCE: the AS2 class fb.Defines: UIHudIcon_SquadLeader 7, UIHudIcon_Dead 145, UIHudIcon_Voip 122,
// ObjectiveAttack 18, EnemyBase 35, ObjectiveScout 15, EnemyPlayer 4, UISpottedInfo_None 0.
inline constexpr int32_t kUiHudIconSquadLeader = 7;
inline constexpr int32_t kUiHudIconDead = 145;
inline constexpr int32_t kUiHudIconVoip = 122;
// SOURCE: the AS2 class Widget.Icon3D.Mapmarker.Mapmarkertag: these icons take the red background.
inline constexpr std::array<int32_t, 4> kUiHudIconsOnRedMarker{ 18, 35, 15, 4 };
inline constexpr int32_t kUiSpottedInfoNone = 0;
// INFERRED: read off the 18.57 footage at FOV 100; its dependence on the field of view is not known.
// Settle: footage at another field of view, or a note on UIWeaponComp's Dispersion.
inline constexpr double kHudCrosshairDispersionPerDegree = 0.2;

int32_t hud_compass_frame(float p_heading_deg);

int32_t hud_spawn_timer_seconds(float p_time_to_respawn);

inline constexpr int32_t kSelectSpawnPointActionKey = -1239372646;
std::optional<int32_t> hud_selected_spawn_point(std::span<const std::string> p_params);

double hud_crosshair_dispersion(float p_spread_deg);
double hud_crosshair_arm(float p_spread_deg, double p_base_offset);
float hud_crosshair_spread_for_arm(double p_arm, double p_base_offset);

double hud_bg_light_ramp_fraction_x();
double hud_bg_light_ramp_fraction_y();
float hud_bg_light_ramp(const HudBgDrawLook &p_look, float p_inward_of_fade);
float hud_bg_coverage_ramp(const HudBgDrawLook &p_look, float p_inward_of_fade);

float hud_ekg_beat_px(const HudEkgLook &p_look, float p_distance_px);
float hud_ekg_offset_px(const HudEkgLook &p_look, double p_clock_seconds, float p_screen_x);
float hud_ekg_alpha(const HudEkgLook &p_look, float p_screen_x);

std::string hud_list_access_handle(const HudDataSource &p_query);
bool hud_list_set_rows(HudScreen &r_screen, std::string_view p_instance, const As2Value &p_rows);
bool hud_list_set_direct_rows(HudScreen &r_screen, std::string_view p_instance, std::vector<As2Value> p_rows);

int32_t hud_eor_place(const HudEndOfRound &p_eor);
std::string hud_eor_round_duration(float p_seconds);
std::string hud_eor_play_time(float p_seconds);
std::string hud_eor_distance(float p_metres);
int32_t hud_eor_seconds_left(float p_seconds);
std::vector<As2Value> hud_scoreboard_rows(const HudModel &p_model, const HudPlayerList &p_list);
As2Value hud_objective_status(const HudCapturePointList &p_points);
As2Value hud_passenger_list(const HudPassengerList &p_list);
std::optional<As2Value> hud_vehicle_source(
		const HudModel &p_model, const HudClientState &p_client, const HudDataSource &p_source);

struct HudFlagIcon {
	HudIconChoice icon;
	float x = 0.0f;
	float z = 0.0f;
	char letter = 0;
};
std::vector<HudFlagIcon> hud_minimap_flags(const HudCapturePointList &p_points);
}

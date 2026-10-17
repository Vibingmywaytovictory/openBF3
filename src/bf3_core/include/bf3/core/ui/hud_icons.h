#pragma once
// HUD icons outside the SWFs (plan §3.8, D16; research/topics/gap-r1-ui-hud-minimap-localisation.md §1, §5 "Icons",
// "Kill feed"; docs/design/ui-minimap-icons.md §6). Godot-free.
//
// * The UIHudIcon atlas: ui/static/hud3dicons (Win32/UI, bundle ui/flow/bundle/ingamebundlemp) is one
//   UIMinimapIconTextureAtlasAsset. Icons: 161 UIMinimapIconTexture entries {IconType (UIHudIcon_* enum), States}, 159
//   distinct IconType names (UIHudIcon_RadarSweepComponent and UIHudIcon_LocalPlayerBigIcon appear twice; only the
//   second LocalPlayerBigIcon has states), 93 entries with states. A state is UIMinimapIconTextureState {State
//   (UIIconState_Default 54, _Squad 38, _Neutral 38, _Friendly 35, _Enemy 34, _Blink 3), FrameRate, ShouldRotate,
//   TextureInfos: frames of UIMinimapIconUv {MinUv, MaxUv}} (1 frame on 180 states, 2 on 18, 3 on 2, 10 on 2).
//   TextureAtlas -> ui/static/hud3diconswin32: DxTexture ARGB8888 (R, G, B, A in memory) 256 x 1024, one mip.
//   UVs land on whole texels (e.g. LocalPlayer Default 0.13671875..0.203125 x 0.61816406..0.63671875 = x 35..52,
//   y 633..652).
// * Item icons (kill feed, kill card, loadout): UIWeaponDescription (ui/uiweaponmetadata, 111 entries) and
//   UIKitItemDescription (ui/uikititemmetadata, 17) carry ItemIds (the unlock identifiers) and IconTexturePath
//   ("UI/Art/Persistence/Weapons/M16A4_small"); the path lowercased is the TextureAsset / DxTexture res name in
//   Win32/UI (DXT5 64 x 16, one mip, for the weapons). ItemIds holds SoldierWeaponUnlockAsset.Identifier values
//   (WeaponDef::unlock_identifier). Verified 2026-10-15 by identifier, not by name: of MP_Subway's 172 catalog
//   variants, 158 find their icon through the weapon metadata (the description's Name equals the variant's name SID
//   on all 158) and the other 14 (13 gadgets, ui/art/persistence/kititem/*_small, and the M67,
//   ui/art/persistence/weapons/grenade_small) through the kit-item metadata; every one of the 172 textures exists in
//   Win32/UI; 2 ids are listed by two descriptions (the first wins). UIVehicleDescription (ui/uivehiclemetadata, 56)
//   has TexturePath only.
// * Vehicles: VehicleEntityData.HudData (VehicleHudData) {MinimapIcon (UIHudIcon_*), Texture (TextureAsset, null on
//   e.g. the Tunguska), VehicleItemHash}.
//
// Hostile input: readers charge a gameplay::WorkBudget; at most kMaxHudIcons icons, kMaxHudIconStates states per
// icon, kMaxHudIconFrames frames per state; UVs must be finite and inside [0, 1]; images must be the
// expected format and at most kMaxHudIconImageSide a side.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/result.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class LevelMount;
}

namespace bf3::core::ui {

inline constexpr std::string_view kHudIconAtlasPartition = "ui/static/hud3dicons";
inline constexpr std::string_view kWeaponMetadataPartition = "ui/uiweaponmetadata";
inline constexpr std::string_view kKitItemMetadataPartition = "ui/uikititemmetadata";
inline constexpr std::string_view kVehicleMetadataPartition = "ui/uivehiclemetadata";

inline constexpr size_t kMaxHudIcons = 1024; // retail 161
inline constexpr size_t kMaxHudIconStates = 16; // retail: 6 UIIconState values exist
inline constexpr size_t kMaxHudIconFrames = 64; // retail at most 10
inline constexpr uint32_t kMaxHudIconImageSide = 4096; // retail atlas 256 x 1024, item icons 64 x 16
inline constexpr size_t kMaxItemIcons = 4096; // retail 111 + 17 + 56

// UIIconState_*; Unknown keeps a state whose name this build does not know (counted, never dropped silently).
enum class HudIconState : uint8_t {
	Default,
	Friendly,
	Enemy,
	Squad,
	Neutral,
	Blink,
	Unknown,
};
std::string_view hud_icon_state_name(HudIconState p_state); // "UIIconState_Friendly"
HudIconState parse_hud_icon_state(std::string_view p_enum_name);

// UIMinimapIconUv: normalised atlas coordinates, v = 0 at the top row.
struct HudIconUv {
	float min_u = 0.0f;
	float min_v = 0.0f;
	float max_u = 0.0f;
	float max_v = 0.0f;
};

// An atlas rectangle in texels (AtlasTexture::set_region / draw_texture_rect_region source rect), always with a
// non-negative size. A frame whose MinUv lies right of (or below) its MaxUv is drawn mirrored on that axis: 8 retail
// frames are written that way (the Blink state of the three flag icons in both axes, UIHudIcon_PercetageBarMiddle and
// UIHudIcon_PercentageBarBackground in u; verified 2026-10-15 on ui/static/hud3dicons), so flip_x / flip_y keep the
// order the data gives instead of refusing it.
struct HudIconRect {
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = 0;
	int32_t height = 0;
	bool flip_x = false; // MinUv.x > MaxUv.x
	bool flip_y = false; // MinUv.y > MaxUv.y
};

// Rounded to whole texels (retail UVs are exact multiples of 1/256 and 1/1024).
inline HudIconRect hud_icon_rect(const HudIconUv &p_uv, uint32_t p_atlas_width, uint32_t p_atlas_height) {
	const int32_t x0 = int32_t(std::lround(double(p_uv.min_u) * p_atlas_width));
	const int32_t y0 = int32_t(std::lround(double(p_uv.min_v) * p_atlas_height));
	const int32_t x1 = int32_t(std::lround(double(p_uv.max_u) * p_atlas_width));
	const int32_t y1 = int32_t(std::lround(double(p_uv.max_v) * p_atlas_height));
	HudIconRect r;
	r.flip_x = x1 < x0;
	r.flip_y = y1 < y0;
	r.x = r.flip_x ? x1 : x0;
	r.y = r.flip_y ? y1 : y0;
	r.width = r.flip_x ? x0 - x1 : x1 - x0;
	r.height = r.flip_y ? y0 - y1 : y1 - y0;
	return r;
}

struct HudIconStateFrames {
	HudIconState state = HudIconState::Default;
	float frame_rate = 0.0f; // FrameRate
	bool should_rotate = false; // ShouldRotate: the icon turns with its object's heading
	std::vector<HudIconUv> frames; // TextureInfos
};

// The frame index shown p_seconds after an animation started (FrameRate frames per second, looping).
inline size_t hud_icon_frame(const HudIconStateFrames &p_state, double p_seconds) {
	if (p_state.frames.size() <= 1 || !(p_state.frame_rate > 0.0f) || !(p_seconds >= 0.0)) {
		return 0;
	}
	return size_t(std::floor(p_seconds * double(p_state.frame_rate))) % p_state.frames.size();
}

struct HudIcon {
	std::string type; // UIHudIcon_FriendlyFlag
	std::vector<HudIconStateFrames> states; // data order

	const HudIconStateFrames *state(HudIconState p_state) const {
		for (const HudIconStateFrames &s : states) {
			if (s.state == p_state) {
				return &s;
			}
		}
		return nullptr;
	}
};

struct HudIconAtlas {
	std::string partition; // ui/static/hud3dicons
	std::string texture; // TextureAtlas: ui/static/hud3diconswin32
	std::vector<HudIcon> icons; // data order, duplicates kept (161 on retail)
	size_t unknown_states = 0;

	// The entry for a UIHudIcon name: the first with states, else the first entry (the duplicates, header comment).
	const HudIcon *find(std::string_view p_type) const;
	size_t with_states() const;
};

// Reads the atlas asset (not its pixels). p_budget null: a default WorkBudget.
Result<HudIconAtlas> read_hud_icon_atlas(const EbxResolver &p_ui, gameplay::WorkBudget *p_budget = nullptr,
		std::string_view p_partition = kHudIconAtlasPartition);

// ARGB8888 pixels as R, G, B, A bytes (Godot Image FORMAT_RGBA8), row 0 at the top.
struct Rgba8Image {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> rgba;
};
// p_mount: a mount of the superbundle holding the atlas texture (Win32/UI).
Result<Rgba8Image> load_hud_icon_texture(const LevelMount &p_mount, const HudIconAtlas &p_atlas);
// The decode step on a parsed header and its whole chunk (ARGB8888, 2D, one face, at most kMaxHudIconImageSide a side;
// the unit and fuzz tests feed it synthetic and mutated headers). p_name is for messages.
Result<Rgba8Image> decode_rgba8_texture(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name);

// ---- which atlas entry an object shows (design §10.2; M1 has one local team, kM1LocalTeam in minimap_hud.h) -------
// Retail states of the entries used (bf3dump dump ui/static/hud3dicons, 2026-10-15): LocalPlayer {Default, Squad},
// Player {Friendly, Enemy, Squad, Neutral}, NeutralFlag {Blink, Neutral}, FriendlyFlag {Friendly, Blink, Neutral},
// EnemyFlag {Enemy, Blink, Neutral}, Base {Friendly, Enemy}; FriendlyBase {Friendly, Neutral} and EnemyBase {Default,
// Neutral} are not used. Team 0 is neutral; an owner equal to the local team is friendly, any other non-zero owner is
// enemy. Blink (a flag being captured) needs the capture logic (M4).
struct HudIconChoice {
	std::string_view icon;
	HudIconState state = HudIconState::Default;
};
// A flag or HQ on the minimap and in the world (p_hq: CapturableType CTUnableToChangeTeam).
constexpr HudIconChoice objective_icon(bool p_hq, int32_t p_owner_team, int32_t p_local_team) {
	if (p_hq) {
		return { "UIHudIcon_Base", p_owner_team != 0 && p_owner_team == p_local_team ? HudIconState::Friendly : HudIconState::Enemy };
	}
	if (p_owner_team == 0) {
		return { "UIHudIcon_NeutralFlag", HudIconState::Neutral };
	}
	return p_owner_team == p_local_team ? HudIconChoice{ "UIHudIcon_FriendlyFlag", HudIconState::Friendly }
										: HudIconChoice{ "UIHudIcon_EnemyFlag", HudIconState::Enemy };
}
// The local player (always LocalPlayer Default until squads exist) and the other soldiers.
constexpr HudIconChoice soldier_icon(bool p_local, int32_t p_team, int32_t p_local_team) {
	if (p_local) {
		return { "UIHudIcon_LocalPlayer", HudIconState::Default };
	}
	if (p_team == 0) {
		return { "UIHudIcon_Player", HudIconState::Neutral };
	}
	return { "UIHudIcon_Player", p_team == p_local_team ? HudIconState::Friendly : HudIconState::Enemy };
}

// ---- item icons -------------------------------------------------------------------------------------------------------

enum class ItemIconSource : uint8_t {
	Weapon, // UIWeaponDescription.IconTexturePath
	KitItem, // UIKitItemDescription.IconTexturePath
	Vehicle, // UIVehicleDescription.TexturePath (no small icon in the data)
};

struct ItemIcon {
	uint32_t item_id = 0; // one of ItemIds (the unlock identifier, WeaponDef::unlock_identifier)
	ItemIconSource source = ItemIconSource::Weapon;
	std::string name_sid; // Name, e.g. ID_P_WNAME_M16A4
	std::string path; // as written: UI/Art/Persistence/Weapons/M16A4_small
	std::string res_name; // lowercased: ui/art/persistence/weapons/m16a4_small
	std::string hud_icon; // UIKitItemDescription.HudIcon (UIHudIcon_Unused on retail), else empty
};

// Every ItemIds entry of the three metadata partitions, sorted by id; the first description listing an id wins
// (weapon, then kit item, then vehicle).
class ItemIconIndex {
public:
	static Result<ItemIconIndex> read(const EbxResolver &p_ui, gameplay::WorkBudget *p_budget = nullptr);

	const ItemIcon *find(uint32_t p_item_id) const;
	const std::vector<ItemIcon> &icons() const { return icons_; }
	size_t duplicate_ids() const { return duplicate_ids_; }

private:
	std::vector<ItemIcon> icons_;
	size_t duplicate_ids_ = 0;
};

// An item icon's pixels, kept as stored (DXT5 blocks for the retail _small icons: Godot FORMAT_DXT5, no transcode).
struct ItemIconImage {
	uint32_t width = 0;
	uint32_t height = 0;
	res::TextureFormat format = res::TextureFormat::Dxt5;
	std::vector<uint8_t> data; // mip 0
};
Result<ItemIconImage> load_item_icon(const LevelMount &p_mount, const ItemIcon &p_icon);
// The decode step (2D, one face, a format of known size, mip 0 resident and complete).
Result<ItemIconImage> decode_item_icon(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name);

// ---- vehicles ---------------------------------------------------------------------------------------------------------

struct VehicleHudIcon {
	std::string minimap_icon; // HudData.MinimapIcon, e.g. UIHudIcon_TankAA
	std::string texture; // HudData.Texture (TextureAsset partition), empty when null
	uint32_t vehicle_item_hash = 0; // HudData.VehicleItemHash
};
// p_vehicle: a VehicleEntityData instance.
Result<VehicleHudIcon> read_vehicle_hud_icon(const EbxResolver &p_db, const EbxInstance &p_vehicle, gameplay::WorkBudget &r_budget);

} // namespace bf3::core::ui

#pragma once

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

inline constexpr size_t kMaxHudIcons = 1024;
inline constexpr size_t kMaxHudIconStates = 16;
inline constexpr size_t kMaxHudIconFrames = 64;
inline constexpr uint32_t kMaxHudIconImageSide = 4096;
inline constexpr size_t kMaxItemIcons = 4096;

enum class HudIconState : uint8_t {
	Default,
	Friendly,
	Enemy,
	Squad,
	Neutral,
	Blink,
	Unknown,
};
std::string_view hud_icon_state_name(HudIconState p_state);
HudIconState parse_hud_icon_state(std::string_view p_enum_name);

struct HudIconUv {
	float min_u = 0.0f;
	float min_v = 0.0f;
	float max_u = 0.0f;
	float max_v = 0.0f;
};

struct HudIconRect {
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = 0;
	int32_t height = 0;
	bool flip_x = false;
	bool flip_y = false;
};

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
	float frame_rate = 0.0f;
	bool should_rotate = false;
	std::vector<HudIconUv> frames;
};

inline size_t hud_icon_frame(const HudIconStateFrames &p_state, double p_seconds) {
	if (p_state.frames.size() <= 1 || !(p_state.frame_rate > 0.0f) || !(p_seconds >= 0.0)) {
		return 0;
	}
	return size_t(std::floor(p_seconds * double(p_state.frame_rate))) % p_state.frames.size();
}

struct HudIcon {
	std::string type;
	std::vector<HudIconStateFrames> states;

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
	std::string partition;
	std::string texture;
	std::vector<HudIcon> icons;
	size_t unknown_states = 0;

	const HudIcon *find(std::string_view p_type) const;
	size_t with_states() const;
};

Result<HudIconAtlas> read_hud_icon_atlas(const EbxResolver &p_ui, gameplay::WorkBudget *p_budget = nullptr,
		std::string_view p_partition = kHudIconAtlasPartition);

struct Rgba8Image {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> rgba;
};

Result<Rgba8Image> load_hud_icon_texture(const LevelMount &p_mount, const HudIconAtlas &p_atlas);

Result<Rgba8Image> decode_rgba8_texture(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name);

struct HudIconChoice {
	std::string_view icon;
	HudIconState state = HudIconState::Default;
};

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

constexpr HudIconChoice soldier_icon(bool p_local, int32_t p_team, int32_t p_local_team) {
	if (p_local) {
		return { "UIHudIcon_LocalPlayer", HudIconState::Default };
	}
	if (p_team == 0) {
		return { "UIHudIcon_Player", HudIconState::Neutral };
	}
	return { "UIHudIcon_Player", p_team == p_local_team ? HudIconState::Friendly : HudIconState::Enemy };
}

constexpr HudIconChoice local_player_icon(bool p_in_squad) {
	return { "UIHudIcon_LocalPlayer", p_in_squad ? HudIconState::Squad : HudIconState::Default };
}

enum class ItemIconSource : uint8_t {
	Weapon,
	KitItem,
	Vehicle,
};

struct ItemIcon {
	uint32_t item_id = 0;
	ItemIconSource source = ItemIconSource::Weapon;
	std::string name_sid;
	std::string path;
	std::string res_name;
	std::string hud_icon;
};

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

struct ItemIconImage {
	uint32_t width = 0;
	uint32_t height = 0;
	res::TextureFormat format = res::TextureFormat::Dxt5;
	std::vector<uint8_t> data;
};
Result<ItemIconImage> load_item_icon(const LevelMount &p_mount, const ItemIcon &p_icon);

Result<ItemIconImage> decode_item_icon(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name);

struct VehicleHudIcon {
	std::string minimap_icon;
	std::string texture;
	uint32_t vehicle_item_hash = 0;
};

Result<VehicleHudIcon> read_vehicle_hud_icon(const EbxResolver &p_db, const EbxInstance &p_vehicle, gameplay::WorkBudget &r_budget);

}

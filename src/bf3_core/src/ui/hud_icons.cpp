#include "bf3/core/ui/hud_icons.h"

#include "bf3/core/res/res_loader.h"
#include "bf3/core/superbundle_mount.h"
#include "ui_ebx.h"

#include <cstring>

namespace bf3::core::ui {

namespace {

constexpr std::string_view kStateNames[] = {
	"UIIconState_Default",
	"UIIconState_Friendly",
	"UIIconState_Enemy",
	"UIIconState_Squad",
	"UIIconState_Neutral",
	"UIIconState_Blink",
};

std::string_view text_of(const std::optional<EbxValue> &p_value) {
	if (!p_value) {
		return {};
	}
	if (const std::optional<std::string_view> e = p_value->enum_name()) {
		return *e;
	}
	return p_value->as_str().value_or(std::string_view());
}

Result<std::string> copy_charged(gameplay::WorkBudget &r_budget, std::string_view p_text) {
	BF3_TRY(r_budget.charge_bytes(p_text.size()));
	return std::string(p_text);
}

Status read_uv(const EbxStruct &p_frame, HudIconUv &r_uv) {
	const std::optional<std::array<float, 2>> mn = detail::vec2(p_frame, "MinUv");
	const std::optional<std::array<float, 2>> mx = detail::vec2(p_frame, "MaxUv");
	if (!mn || !mx) {
		return make_error("UIMinimapIconUv without MinUv / MaxUv");
	}
	r_uv = { (*mn)[0], (*mn)[1], (*mx)[0], (*mx)[1] };
	if (!detail::finite_in_unit(r_uv.min_u) || !detail::finite_in_unit(r_uv.min_v) || !detail::finite_in_unit(r_uv.max_u) ||
			!detail::finite_in_unit(r_uv.max_v)) {
		return make_error("UV outside [0, 1] or not finite");
	}
	return Status::success();
}

}

std::string_view hud_icon_state_name(HudIconState p_state) {
	const size_t i = size_t(p_state);
	return i < std::size(kStateNames) ? kStateNames[i] : std::string_view("UIIconState_Unknown");
}

HudIconState parse_hud_icon_state(std::string_view p_enum_name) {
	for (size_t i = 0; i < std::size(kStateNames); ++i) {
		if (p_enum_name == kStateNames[i]) {
			return HudIconState(i);
		}
	}
	return HudIconState::Unknown;
}

const HudIcon *HudIconAtlas::find(std::string_view p_type) const {
	const HudIcon *first = nullptr;
	for (const HudIcon &icon : icons) {
		if (icon.type != p_type) {
			continue;
		}
		if (!icon.states.empty()) {
			return &icon;
		}
		if (first == nullptr) {
			first = &icon;
		}
	}
	return first;
}

size_t HudIconAtlas::with_states() const {
	size_t n = 0;
	for (const HudIcon &icon : icons) {
		n += icon.states.empty() ? 0 : 1;
	}
	return n;
}

Result<HudIconAtlas> read_hud_icon_atlas(const EbxResolver &p_ui, gameplay::WorkBudget *p_budget, std::string_view p_partition) {
	gameplay::WorkBudget local;
	gameplay::WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	BF3_TRY(budget.status());
	BF3_TRY(budget.charge(1 + p_partition.size() / gameplay::WorkBudget::kBytesPerUnit));
	const EbxPartition *partition = p_ui.partition_by_name(p_partition);
	if (partition == nullptr) {
		return make_error(std::string(p_partition) + ": partition not found");
	}
	const EbxInstance asset = partition->primary();
	if (!asset.is_a("UIMinimapIconTextureAtlasAsset")) {
		return make_error(std::string(p_partition) + ": a " + std::string(asset.type_name()) + ", not a UIMinimapIconTextureAtlasAsset");
	}
	BF3_TRY(budget.charge_type(asset.data()));
	HudIconAtlas out;
	out.partition = to_lower_ascii(p_partition);
	const std::optional<EbxRef> tex = detail::ref(asset.data(), "TextureAtlas");
	const std::optional<std::string> tex_name = tex ? detail::ref_partition(p_ui, *partition, *tex) : std::nullopt;
	if (!tex_name || tex_name->empty()) {
		return make_error(out.partition + ": TextureAtlas does not resolve");
	}
	Result<std::string> tex_copy = copy_charged(budget, *tex_name);
	BF3_TRY(tex_copy);
	out.texture = std::move(*tex_copy);
	const std::optional<EbxValue> icons_value = asset.get("Icons");
	const OptionalEbxArray icons = icons_value ? icons_value->as_array() : OptionalEbxArray();
	if (!icons) {
		return make_error(out.partition + ": no Icons array");
	}
	if (icons->size() > kMaxHudIcons) {
		return make_error(out.partition + ": " + std::to_string(icons->size()) + " icons, more than " + std::to_string(kMaxHudIcons));
	}
	BF3_TRY(budget.charge(icons->size()));
	out.icons.reserve(icons->size());
	for (const EbxValue icon_value : *icons) {
		const std::optional<EbxStruct> icon = icon_value.as_struct();
		if (!icon) {
			return make_error(out.partition + ": an Icons entry is not a struct");
		}
		BF3_TRY(budget.charge_type(*icon));
		HudIcon h;
		Result<std::string> type = copy_charged(budget, text_of(icon->get("IconType")));
		BF3_TRY(type);
		h.type = std::move(*type);
		const std::optional<EbxValue> states_value = icon->get("States");
		const OptionalEbxArray states = states_value ? states_value->as_array() : OptionalEbxArray();
		if (states) {
			if (states->size() > kMaxHudIconStates) {
				return make_error(out.partition + " " + h.type + ": " + std::to_string(states->size()) + " states, more than " + std::to_string(kMaxHudIconStates));
			}
			BF3_TRY(budget.charge(states->size()));
			for (const EbxValue state_value : *states) {
				const std::optional<EbxStruct> state = state_value.as_struct();
				if (!state) {
					return make_error(out.partition + " " + h.type + ": a States entry is not a struct");
				}
				BF3_TRY(budget.charge_type(*state));
				HudIconStateFrames s;
				const std::string_view state_name = text_of(state->get("State"));
				BF3_TRY(budget.charge_bytes(state_name.size()));
				s.state = parse_hud_icon_state(state_name);
				if (s.state == HudIconState::Unknown) {
					++out.unknown_states;
				}
				s.frame_rate = detail::f32_or(*state, "FrameRate", 0.0f);
				if (!std::isfinite(s.frame_rate)) {
					return make_error(out.partition + " " + h.type + ": FrameRate not finite");
				}
				s.should_rotate = detail::flag_or(*state, "ShouldRotate", false);
				const std::optional<EbxValue> frames_value = state->get("TextureInfos");
				const OptionalEbxArray frames = frames_value ? frames_value->as_array() : OptionalEbxArray();
				if (frames) {
					if (frames->size() > kMaxHudIconFrames) {
						return make_error(out.partition + " " + h.type + ": " + std::to_string(frames->size()) + " frames, more than " +
								std::to_string(kMaxHudIconFrames));
					}
					BF3_TRY(budget.charge(frames->size()));
					for (const EbxValue frame_value : *frames) {
						const std::optional<EbxStruct> frame = frame_value.as_struct();
						if (!frame) {
							return make_error(out.partition + " " + h.type + ": a TextureInfos entry is not a struct");
						}
						BF3_TRY(budget.charge_type(*frame));
						HudIconUv uv;
						if (const Status st = read_uv(*frame, uv); !st) {
							return make_error(out.partition + " " + h.type + " " + std::string(state_name) + ": " + st.error().message);
						}
						s.frames.push_back(uv);
					}
				}
				h.states.push_back(std::move(s));
			}
		}
		out.icons.push_back(std::move(h));
	}
	BF3_TRY(budget.status());
	return out;
}

Result<Rgba8Image> decode_rgba8_texture(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name) {
	const res::DxTexture &tex = p_texture;
	const std::string where = "res " + std::string(p_name);
	if (tex.format != res::TextureFormat::Argb8888) {
		return make_error(where + ": format " + std::to_string(int(tex.format)) + ", not ARGB8888");
	}
	if (tex.kind != res::TextureKind::Texture2D || tex.faces() != 1) {
		return make_error(where + ": not a 2D texture with one face");
	}
	if (tex.width == 0 || tex.height == 0 || tex.width > kMaxHudIconImageSide || tex.height > kMaxHudIconImageSide) {
		return make_error(where + ": size out of range");
	}
	std::vector<res::MipSlice> mips;
	const res::ResError e = tex.split_mips(res::Bytes(p_chunk.data(), p_chunk.size()), 0, mips);
	if (!e.ok()) {
		return make_error(where + ": " + e.message());
	}
	if (mips.empty()) {
		return make_error(where + ": no mip");
	}
	const res::MipSlice &m = mips.front();
	if (m.width == 0 || m.height == 0 || m.width > kMaxHudIconImageSide || m.height > kMaxHudIconImageSide || m.depth != 1) {
		return make_error(where + ": mip size out of range");
	}
	const size_t row = size_t(m.width) * 4;
	const size_t pitch = m.row_pitch == 0 ? row : m.row_pitch;
	if (pitch < row || m.data.size() < pitch * (size_t(m.height) - 1) + row) {
		return make_error(where + ": mip holds fewer bytes than " + std::to_string(m.width) + "x" + std::to_string(m.height) + "x4");
	}
	Rgba8Image out;
	out.width = m.width;
	out.height = m.height;
	out.rgba.resize(row * m.height);
	for (uint32_t y = 0; y < m.height; ++y) {
		std::memcpy(out.rgba.data() + y * row, m.data.data() + y * pitch, row);
	}
	return out;
}

Result<Rgba8Image> load_hud_icon_texture(const LevelMount &p_mount, const HudIconAtlas &p_atlas) {
	Result<res::LoadedTexture> t = res::load_texture(p_mount, p_atlas.texture);
	if (!t) {
		return t.error();
	}
	return decode_rgba8_texture(t->texture, std::span<const uint8_t>(t->chunk.bytes), p_atlas.texture);
}

Result<VehicleHudIcon> read_vehicle_hud_icon(const EbxResolver &p_db, const EbxInstance &p_vehicle, gameplay::WorkBudget &r_budget) {
	BF3_TRY(r_budget.status());
	BF3_TRY(r_budget.charge_type(p_vehicle.data()));
	if (!p_vehicle.is_a("VehicleEntityData")) {
		return make_error(std::string("a ") + std::string(p_vehicle.type_name()) + ", not a VehicleEntityData");
	}
	const std::optional<EbxStruct> hud = detail::sub(p_vehicle.data(), "HudData");
	if (!hud) {
		return make_error("VehicleEntityData without HudData");
	}
	VehicleHudIcon out;
	Result<std::string> icon = copy_charged(r_budget, text_of(hud->get("MinimapIcon")));
	BF3_TRY(icon);
	out.minimap_icon = std::move(*icon);
	if (const std::optional<EbxRef> tex = detail::ref(*hud, "Texture"); tex && !tex->is_null()) {
		const std::optional<std::string> name = detail::ref_partition(p_db, p_vehicle.partition(), *tex);
		if (!name) {
			return make_error("HudData.Texture does not resolve");
		}
		Result<std::string> copy = copy_charged(r_budget, *name);
		BF3_TRY(copy);
		out.texture = std::move(*copy);
	}
	const std::optional<EbxValue> hash = hud->get("VehicleItemHash");
	out.vehicle_item_hash = hash ? hash->as_u32().value_or(0) : 0;
	return out;
}

}

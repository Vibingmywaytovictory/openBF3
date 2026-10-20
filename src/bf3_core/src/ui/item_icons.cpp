#include "bf3/core/ui/hud_icons.h"

#include "bf3/core/res/res_loader.h"
#include "bf3/core/superbundle_mount.h"
#include "ui_ebx.h"

#include <algorithm>
#include <unordered_set>

namespace bf3::core::ui {

namespace {

constexpr size_t kMaxItemIconPath = 512;

struct Source {
	std::string_view partition;
	ItemIconSource kind;
	std::string_view path_field;
};

constexpr Source kSources[] = {
	{ kWeaponMetadataPartition, ItemIconSource::Weapon, "IconTexturePath" },
	{ kKitItemMetadataPartition, ItemIconSource::KitItem, "IconTexturePath" },
	{ kVehicleMetadataPartition, ItemIconSource::Vehicle, "TexturePath" },
};

std::string res_key(std::string_view p_path) {
	std::string out = to_lower_ascii(p_path);
	std::replace(out.begin(), out.end(), '\\', '/');
	return out;
}

}

Result<ItemIconIndex> ItemIconIndex::read(const EbxResolver &p_ui, gameplay::WorkBudget *p_budget) {
	gameplay::WorkBudget local;
	gameplay::WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	BF3_TRY(budget.status());
	ItemIconIndex out;
	std::unordered_set<uint32_t> seen;
	for (const Source &source : kSources) {
		BF3_TRY(budget.charge(1));
		const EbxPartition *partition = p_ui.partition_by_name(source.partition);
		if (partition == nullptr) {
			return make_error(std::string(source.partition) + ": partition not found");
		}
		const EbxInstance asset = partition->primary();
		BF3_TRY(budget.charge_type(asset.data()));
		const std::optional<EbxValue> items_value = asset.get("Items");
		const OptionalEbxArray items = items_value ? items_value->as_array() : OptionalEbxArray();
		if (!items) {
			return make_error(std::string(source.partition) + ": no Items array");
		}
		BF3_TRY(budget.charge(items->size()));
		for (const EbxValue item_value : *items) {
			const std::optional<EbxRef> r = item_value.as_ref();
			if (!r || r->is_null()) {
				continue;
			}
			const std::optional<EbxInstance> item = p_ui.resolve(*partition, *r);
			if (!item || !item->is_a("UIItemDescription")) {
				continue;
			}
			BF3_TRY(budget.charge_type(item->data()));
			const std::optional<EbxValue> ids_value = item->get("ItemIds");
			const OptionalEbxArray ids = ids_value ? ids_value->as_array() : OptionalEbxArray();
			if (!ids || ids->empty()) {
				continue;
			}
			BF3_TRY(budget.charge(ids->size()));
			const std::string_view path = item->get(source.path_field) ? item->get(source.path_field)->as_str().value_or("") : std::string_view();
			const std::string_view name = item->get("Name") ? item->get("Name")->as_str().value_or("") : std::string_view();
			std::string_view hud_icon;
			if (const std::optional<EbxValue> h = item->get("HudIcon")) {
				hud_icon = h->enum_name().value_or(h->as_str().value_or(""));
			}
			if (path.size() > kMaxItemIconPath || name.size() > kMaxItemIconPath || hud_icon.size() > kMaxItemIconPath) {
				return make_error(std::string(source.partition) + ": a description's text is longer than " + std::to_string(kMaxItemIconPath) + " bytes");
			}
			for (const EbxValue id_value : *ids) {
				const std::optional<uint32_t> id = id_value.as_u32();
				if (!id) {
					continue;
				}
				if (!seen.insert(*id).second) {
					++out.duplicate_ids_;
					continue;
				}
				if (out.icons_.size() >= kMaxItemIcons) {
					return make_error("item icon index: more than " + std::to_string(kMaxItemIcons) + " ids");
				}
				BF3_TRY(budget.charge_bytes(2 * path.size() + name.size() + hud_icon.size()));
				ItemIcon icon;
				icon.item_id = *id;
				icon.source = source.kind;
				icon.name_sid = std::string(name);
				icon.path = std::string(path);
				icon.res_name = res_key(path);
				icon.hud_icon = std::string(hud_icon);
				out.icons_.push_back(std::move(icon));
			}
		}
	}
	std::sort(out.icons_.begin(), out.icons_.end(), [](const ItemIcon &a, const ItemIcon &b) { return a.item_id < b.item_id; });
	BF3_TRY(budget.status());
	return out;
}

const ItemIcon *ItemIconIndex::find(uint32_t p_item_id) const {
	const auto it = std::lower_bound(icons_.begin(), icons_.end(), p_item_id, [](const ItemIcon &a, uint32_t b) { return a.item_id < b; });
	return it != icons_.end() && it->item_id == p_item_id ? &*it : nullptr;
}

Result<ItemIconImage> decode_item_icon(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name) {
	const res::DxTexture &tex = p_texture;
	const std::string where = "res " + std::string(p_name);
	if (tex.kind != res::TextureKind::Texture2D || tex.faces() != 1) {
		return make_error(where + ": not a 2D texture with one face");
	}
	if (tex.width == 0 || tex.height == 0 || tex.width > kMaxHudIconImageSide || tex.height > kMaxHudIconImageSide) {
		return make_error(where + ": size out of range");
	}
	const int64_t bytes = res::texture_image_bytes(tex.format, tex.width, tex.height);
	if (bytes <= 0) {
		return make_error(where + ": format " + std::to_string(int(tex.format)) + " has no known size");
	}
	std::vector<res::MipSlice> mips;
	const res::ResError e = tex.split_mips(res::Bytes(p_chunk.data(), p_chunk.size()), 0, mips);
	if (!e.ok()) {
		return make_error(where + ": " + e.message());
	}
	if (mips.empty() || mips.front().width != tex.width || mips.front().height != tex.height) {
		return make_error(where + ": mip 0 is not resident");
	}
	if (mips.front().data.size() < size_t(bytes)) {
		return make_error(where + ": mip 0 holds " + std::to_string(mips.front().data.size()) + " bytes, fewer than " + std::to_string(bytes));
	}
	ItemIconImage out;
	out.width = tex.width;
	out.height = tex.height;
	out.format = tex.format;
	out.data.assign(mips.front().data.begin(), mips.front().data.begin() + bytes);
	return out;
}

Result<ItemIconImage> load_item_icon(const LevelMount &p_mount, const ItemIcon &p_icon) {
	if (p_icon.res_name.empty()) {
		return make_error("item " + std::to_string(p_icon.item_id) + ": no icon path");
	}
	Result<res::LoadedTexture> t = res::load_texture(p_mount, p_icon.res_name);
	if (!t) {
		return t.error();
	}
	return decode_item_icon(t->texture, std::span<const uint8_t>(t->chunk.bytes), p_icon.res_name);
}

}

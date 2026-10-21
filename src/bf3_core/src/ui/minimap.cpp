#include "bf3/core/ui/minimap.h"

#include "bf3/core/install.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "ui_ebx.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace bf3::core::ui {

namespace {

using level::charged_str;
using level::WorkBudget;

struct FieldRef {
	std::string asset;
	std::string texture;
};

Result<FieldRef> read_field_ref(const EbxResolver &p_db, const EbxPartition &p_from, const std::optional<EbxRef> &p_ref, WorkBudget &r_budget,
		std::string_view p_what) {
	FieldRef out;
	if (!p_ref || p_ref->is_null()) {
		return out;
	}
	BF3_TRY(r_budget.charge(1));
	const std::optional<EbxInstance> asset = p_db.resolve(p_from, *p_ref);
	if (!asset) {
		return make_error(std::string(p_what) + ": the UIDistanceFieldAsset does not resolve");
	}
	if (!asset->is_a("UIDistanceFieldAsset")) {
		return make_error(std::string(p_what) + ": a " + std::string(asset->type_name()) + ", not a UIDistanceFieldAsset");
	}
	const std::optional<std::string> asset_name = detail::ref_partition(p_db, p_from, *p_ref);
	if (!asset_name) {
		return make_error(std::string(p_what) + ": the UIDistanceFieldAsset partition has no name");
	}
	out.asset = *asset_name;
	const std::optional<EbxRef> tex = detail::ref(asset->data(), "DistanceFieldTexture");
	if (!tex || tex->is_null()) {
		return make_error(out.asset + ": no DistanceFieldTexture");
	}
	const std::optional<std::string> tex_name = detail::ref_partition(p_db, asset->partition(), *tex);
	if (!tex_name || tex_name->empty()) {
		return make_error(out.asset + ": the DistanceFieldTexture partition does not resolve");
	}
	BF3_TRY(r_budget.charge(level::text_units(tex_name->size())));
	out.texture = *tex_name;
	return out;
}

Status read_field_params(const EbxResolver &p_db, const EbxPartition &p_from, const EbxStruct &p_params, WorkBudget &r_budget, std::string_view p_what,
		MinimapFieldParams &r_out) {
	BF3_TRY(r_budget.charge(1));
	Result<FieldRef> f = read_field_ref(p_db, p_from, detail::ref(p_params, "DistanceField"), r_budget, p_what);
	BF3_TRY(f);
	r_out.distance_field_asset = f->asset;
	r_out.texture = f->texture;
	r_out.alpha_threshold = detail::f32_or(p_params, "AlphaThreshold", 0.5f);
	r_out.distance_scale = detail::f32_or(p_params, "DistanceScale", 1.0f);
	r_out.outline_inner = detail::f32_or(p_params, "OutlineInner", 0.0f);
	r_out.outline_outer = detail::f32_or(p_params, "OutlineOuter", 0.0f);
	r_out.color_tint = detail::vec4_or(p_params, "ColorTint", { 1, 1, 1, 1 });
	r_out.outline_color = detail::vec4_or(p_params, "OutlineColor", { 0, 0, 0, 0 });
	if (!detail::finite_in_unit(r_out.alpha_threshold)) {
		return make_error(std::string(p_what) + ".AlphaThreshold outside [0, 1]");
	}
	return Status::success();
}

Result<std::string> texture_name(const EbxResolver &p_db, const EbxPartition &p_from, const EbxStruct &p_s, std::string_view p_field, WorkBudget &r_budget) {
	const std::optional<EbxRef> r = detail::ref(p_s, p_field);
	if (!r || r->is_null()) {
		return std::string();
	}
	const std::optional<std::string> name = detail::ref_partition(p_db, p_from, *r);
	if (!name) {
		return make_error(std::string(p_field) + ": the TextureAsset partition does not resolve");
	}
	BF3_TRY(r_budget.charge(level::text_units(name->size())));
	return *name;
}

Result<std::optional<level::Property>> fed(const level::World &p_world, const level::PlacedObject &p_object, std::string_view p_field) {
	const EbxInstance &target = p_object.reference ? *p_object.reference : p_object.entity;
	Result<std::optional<level::Property>> r = p_world.scopes.property(p_object.scope, target, level::field_id(p_field));
	if (!r || *r || !p_object.reference) {
		return r;
	}
	return p_world.scopes.property(p_object.scope, p_object.entity, level::field_id(p_field));
}

Result<int32_t> read_team(const level::World &p_world, const level::PlacedObject &p_object, std::string_view p_field) {
	Result<std::optional<level::Property>> f = fed(p_world, p_object, p_field);
	BF3_TRY(f);
	if (*f) {
		if (const std::optional<uint32_t> t = (*f)->as_team(); t && *t < 1024u) {
			return int32_t(*t);
		}
		if (const std::optional<int64_t> v = (*f)->as_i64(); v && *v >= 0 && *v < 1024) {
			return int32_t(*v);
		}
	}
	const std::optional<EbxValue> own = p_object.entity.get(p_field);
	if (own) {
		if (const std::optional<int32_t> e = own->enum_value(); e && *e >= 0 && *e < 1024) {
			return *e;
		}
	}
	return 0;
}

Result<std::string> read_enum_name(const level::World &p_world, const level::PlacedObject &p_object, std::string_view p_field) {
	WorkBudget &budget = *p_world.budget;
	Result<std::optional<level::Property>> f = fed(p_world, p_object, p_field);
	BF3_TRY(f);
	if (*f) {
		std::string_view text;
		if ((*f)->kind == level::Property::Kind::Field) {
			text = (*f)->field.enum_name().value_or(std::string_view());
		} else if ((*f)->kind == level::Property::Kind::Constant || (*f)->kind == level::Property::Kind::Text) {
			text = (*f)->text;
			if (const size_t space = text.rfind(' '); space != std::string_view::npos) {
				text = text.substr(space + 1);
			}
		}
		if (!text.empty()) {
			BF3_TRY(level::charge_text(budget, text));
			return std::string(text);
		}
	}
	Result<std::optional<std::string_view>> own = level::charged_enum_name(p_object.entity.get(p_field), budget);
	BF3_TRY(own);
	return std::string(own->value_or(""));
}

Result<std::string> read_text(const level::World &p_world, const level::PlacedObject &p_object, std::string_view p_field) {
	WorkBudget &budget = *p_world.budget;
	Result<std::optional<level::Property>> f = fed(p_world, p_object, p_field);
	BF3_TRY(f);
	if (*f) {
		if (const std::optional<std::string_view> s = (*f)->as_str()) {
			BF3_TRY(level::charge_text(budget, *s));
			return std::string(*s);
		}
	}
	Result<std::optional<std::string_view>> own = charged_str(p_object.entity.data(), p_field, budget);
	BF3_TRY(own);
	return std::string(own->value_or(""));
}

std::array<float, 2> texel_world(const MinimapMapping &p_m, uint8_t p_variant, uint32_t p_side, uint32_t p_i, uint32_t p_j) {
	float u = (float(p_i) + 0.5f) / float(p_side);
	float v = (float(p_j) + 0.5f) / float(p_side);
	if (p_variant & 4) {
		std::swap(u, v);
	}
	const float a = (p_variant & 1) ? u - 0.5f : 0.5f - u;
	const float b = (p_variant & 2) ? v - 0.5f : 0.5f - v;
	return { p_m.center_x + a * p_m.size, p_m.center_z + b * p_m.size };
}

bool in_polygon(std::span<const std::array<float, 2>> p_polygon, float p_x, float p_z) {
	bool inside = false;
	for (size_t i = 0, j = p_polygon.size() - 1; i < p_polygon.size(); j = i++) {
		const float xi = p_polygon[i][0], zi = p_polygon[i][1];
		const float xj = p_polygon[j][0], zj = p_polygon[j][1];
		if ((zi > p_z) != (zj > p_z) && p_x < (xj - xi) * (p_z - zi) / (zj - zi) + xi) {
			inside = !inside;
		}
	}
	return inside;
}

template <class Inside>
double mask_iou(const std::vector<bool> &p_mask, uint32_t p_side, const MinimapMapping &p_mapping, uint8_t p_variant, Inside p_inside) {
	if (p_side == 0 || p_side > kMaxIouSide || p_mask.size() != size_t(p_side) * p_side || !(p_mapping.size > 0.0f) || !std::isfinite(p_mapping.size)) {
		return std::nan("");
	}
	size_t both = 0;
	size_t any = 0;
	for (uint32_t j = 0; j < p_side; ++j) {
		for (uint32_t i = 0; i < p_side; ++i) {
			const std::array<float, 2> w = texel_world(p_mapping, p_variant, p_side, i, j);
			const bool a = p_mask[size_t(j) * p_side + i];
			const bool b = p_inside(w[0], w[1]);
			both += (a && b) ? 1 : 0;
			any += (a || b) ? 1 : 0;
		}
	}
	return any == 0 ? 0.0 : double(both) / double(any);
}

}

Result<MinimapData> read_minimap_data(const EbxResolver &p_db, const EbxInstance &p_level_root, std::string_view p_level_id, WorkBudget &r_budget) {
	BF3_TRY(r_budget.status());
	BF3_TRY(r_budget.charge(1));
	const std::string where = std::string(p_level_id) + " UIMinimapData";
	const std::optional<EbxStruct> description = detail::sub(p_level_root.data(), "LevelDescription");
	if (!description) {
		return make_error(std::string(p_level_id) + ": the level root has no LevelDescription");
	}
	std::vector<EbxInstance> components;
	BF3_TRY(level::charged_follow_all(p_db, *description, "Components", r_budget, components));
	std::optional<EbxInstance> component;
	std::optional<EbxStruct> minimap;
	for (const EbxInstance &c : components) {
		BF3_TRY(r_budget.charge(1));
		if (!c.is_a("UILevelDescriptionComponent")) {
			continue;
		}
		if ((minimap = detail::sub(c.data(), "MinimapData"))) {
			component = c;
			break;
		}
	}
	if (!minimap) {
		return make_error(std::string(p_level_id) + ": no UILevelDescriptionComponent with MinimapData in the LevelData");
	}
	const EbxPartition &from = component->partition();
	MinimapData d;
	d.level_id = std::string(p_level_id);
	if (const std::optional<std::string_view> name = p_db.name_of(p_level_root.partition().guid())) {
		d.partition = std::string(*name);
	}
	const std::optional<std::array<float, 2>> center = detail::vec2(*minimap, "WorldCenter");
	if (!center || !std::isfinite((*center)[0]) || !std::isfinite((*center)[1])) {
		return make_error(where + ": WorldCenter missing or not finite");
	}
	d.world_center = *center;
	d.world_size = detail::f32_or(*minimap, "WorldSize", 0.0f);
	d.world_rotation = detail::f32_or(*minimap, "WorldRotation", 0.0f);
	d.world_range = detail::f32_or(*minimap, "WorldRange", 0.0f);
	d.air_radar_range = detail::f32_or(*minimap, "AirRadarRange", 0.0f);
	if (!std::isfinite(d.world_size) || d.world_size <= 0.0f) {
		return make_error(where + ": WorldSize " + std::to_string(d.world_size) + " is not a positive finite number");
	}
	if (d.world_rotation != 0.0f) {
		return make_error(where + ": WorldRotation " + std::to_string(d.world_rotation) + " is not 0 (design §5.1)");
	}
	Result<std::string> sat = texture_name(p_db, from, *minimap, "DetailTexture", r_budget);
	BF3_TRY(sat);
	d.satellite = *sat;
	d.detail_texture_tint = detail::vec4_or(*minimap, "DetailTextureTint", { 1, 1, 1, 1 });
	if (const std::optional<EbxStruct> p = detail::sub(*minimap, "Detail")) {
		BF3_TRY(read_field_params(p_db, from, *p, r_budget, where + ".Detail", d.detail));
	}
	if (const std::optional<EbxStruct> p = detail::sub(*minimap, "Vegetation")) {
		BF3_TRY(read_field_params(p_db, from, *p, r_budget, where + ".Vegetation", d.vegetation));
	}
	d.combat_area_alpha_threshold = detail::f32_or(*minimap, "CombatAreaAlphaThreshold", kResearchCombatAreaThreshold);
	if (!detail::finite_in_unit(d.combat_area_alpha_threshold)) {
		return make_error(where + ": CombatAreaAlphaThreshold outside [0, 1]");
	}
	d.combat_area_distance_scale = detail::f32_or(*minimap, "CombatAreaDistanceScale", 0.0f);
	d.combat_area_scale = detail::f32_or(*minimap, "CombatAreaScale", 1.0f);
	d.combat_area_fade_speed = detail::f32_or(*minimap, "CombatAreaFadeSpeed", 1.0f);
	d.combat_area_color = detail::vec4_or(*minimap, "CombatAreaColor", { 1, 1, 1, 1 });
	Result<std::string> multiply = texture_name(p_db, from, *minimap, "CombatAreaMultiplyTexture", r_budget);
	BF3_TRY(multiply);
	d.combat_area_multiply_texture = *multiply;
	if (const std::optional<std::array<float, 2>> wrap = detail::vec2(*minimap, "CombatAreaMultiplyWrapAmount")) {
		d.combat_area_multiply_wrap = *wrap;
	}
	Result<std::string> fade = texture_name(p_db, from, *minimap, "FadeTexture", r_budget);
	BF3_TRY(fade);
	d.fade_texture = *fade;
	Result<std::string> air = texture_name(p_db, from, *minimap, "AirRadarFadeTexture", r_budget);
	BF3_TRY(air);
	d.air_radar_fade_texture = *air;
	d.use_combat_area_texture = detail::flag_or(*minimap, "UseCombatAreaTexture", true);
	d.use_streaming_minimap = detail::flag_or(*minimap, "UseStreamingMinimap", false);
	return d;
}

Result<CombatAreaMask> read_combat_area(const EbxResolver &p_db, std::string_view p_asset_partition, WorkBudget &r_budget) {
	BF3_TRY(r_budget.status());
	BF3_TRY(r_budget.charge(1 + level::text_units(p_asset_partition.size())));
	const EbxPartition *partition = p_db.partition_by_name(p_asset_partition);
	if (partition == nullptr) {
		return make_error(std::string(p_asset_partition) + ": partition not found");
	}
	const EbxInstance asset = partition->primary();
	if (!asset.is_a("UICombatAreaAsset")) {
		return make_error(std::string(p_asset_partition) + ": a " + std::string(asset.type_name()) + ", not a UICombatAreaAsset");
	}
	CombatAreaMask m;
	m.asset = to_lower_ascii(p_asset_partition);
	Result<std::optional<std::string_view>> prefix = charged_str(asset.data(), "Prefix", r_budget);
	BF3_TRY(prefix);
	m.prefix = std::string(prefix->value_or(""));
	Result<FieldRef> field = read_field_ref(p_db, *partition, detail::ref(asset.data(), "DistanceField"), r_budget, m.asset + ".DistanceField");
	BF3_TRY(field);

	m.distance_field_asset = field->asset;
	m.texture = field->texture;
	Result<FieldRef> surr = read_field_ref(p_db, *partition, detail::ref(asset.data(), "SurroundingDistanceField"), r_budget, m.asset + ".SurroundingDistanceField");
	BF3_TRY(surr);
	m.surrounding_asset = surr->asset;
	m.surrounding_texture = surr->texture;
	return m;
}

Result<MinimapLevel> read_minimap_level(const level::LoadedLevel &p_level, const level::World &p_world) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	const size_t spent_before = budget.spent();
	const EbxResolver &db = p_level.db();
	MinimapLevel out;
	out.mode = p_level.mode;
	Result<MinimapData> data = read_minimap_data(db, p_level.root(), p_level.info.id.empty() ? std::string_view(p_level.root_name) : std::string_view(p_level.info.id), budget);
	BF3_TRY(data);
	out.data = std::move(*data);
	for (const level::PlacedObject &object : p_world.objects) {
		BF3_TRY(budget.charge(1));
		if (object.kind == level::ObjectKind::Other && object.entity.is_a("UICombatAreaEntityData")) {
			if (out.combat_areas.size() >= kMaxCombatAreas) {
				return make_error(out.data.level_id + " " + out.mode + ": more than " + std::to_string(kMaxCombatAreas) + " combat areas");
			}
			const std::optional<EbxRef> asset = detail::ref(object.entity.data(), "Asset");
			if (!asset || asset->is_null()) {
				continue;
			}
			const std::optional<std::string> name = detail::ref_partition(db, object.entity.partition(), *asset);
			const std::string label = detail::instance_label(db, object.entity);
			if (!name || name->empty()) {
				return make_error(label + ": the combat-area asset does not resolve");
			}
			Result<CombatAreaMask> mask = read_combat_area(db, *name, budget);
			if (!mask) {
				return error_at(label, mask.error());
			}
			mask->entity = label;
			out.combat_areas.push_back(std::move(*mask));
			continue;
		}
		if (object.kind != level::ObjectKind::CapturePoint) {
			continue;
		}
		if (out.objectives.size() >= kMaxObjectives) {
			return make_error(out.data.level_id + " " + out.mode + ": more than " + std::to_string(kMaxObjectives) + " capture points");
		}
		MinimapObjective o;
		o.position = object.transform.trans;
		if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2])) {
			return make_error(detail::instance_label(db, object.entity) + ": capture point position not finite");
		}
		const level::Vec3 &f = object.transform.forward;
		o.yaw = (std::isfinite(f[0]) && std::isfinite(f[2]) && (f[0] != 0.0f || f[2] != 0.0f)) ? std::atan2(f[0], f[2]) : 0.0f;
		Result<std::string> capturable = read_enum_name(p_world, object, "CapturableType");
		BF3_TRY(capturable);
		o.kind = *capturable == "CTUnableToChangeTeam" ? ObjectiveKind::Hq : ObjectiveKind::Flag;
		Result<int32_t> owner = read_team(p_world, object, "InitialOwnerTeam");
		BF3_TRY(owner);
		o.initial_owner = *owner;
		Result<std::string> label_sid = read_text(p_world, object, "LocationNameSid");
		BF3_TRY(label_sid);
		o.label_sid = std::move(*label_sid);
		Result<std::string> location_sid = read_text(p_world, object, "LocationTextSid");
		BF3_TRY(location_sid);
		o.location_sid = std::move(*location_sid);
		o.entity = detail::instance_label(db, object.entity);
		out.objectives.push_back(std::move(o));
	}
	BF3_TRY(budget.status());
	out.work = budget.spent() - spent_before;
	return out;
}

Result<MinimapLevel> load_minimap_level(std::shared_ptr<const Install> p_install, std::string_view p_level_id, std::string_view p_mode,
		const MinimapLoadOptions &p_options) {
	level::LoadOptions lo;
	lo.threads = p_options.threads;
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(std::move(p_install), p_level_id, p_mode, lo);
	if (!loaded) {
		return loaded.error();
	}
	Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
	if (!world) {
		return error_at(std::string(p_level_id) + " " + std::string(p_mode) + " traversal", world.error());
	}
	return read_minimap_level(**loaded, **world);
}

Result<std::vector<bool>> combat_area_union(std::span<const L8Image> p_masks, float p_threshold) {
	uint32_t w = 0, h = 0;
	for (const L8Image &m : p_masks) {
		if (m.empty()) {
			continue;
		}
		if (w == 0) {
			w = m.width;
			h = m.height;
		} else if (m.width != w || m.height != h) {
			return make_error("combat-area masks of different sizes (" + std::to_string(w) + "x" + std::to_string(h) + " and " + std::to_string(m.width) + "x" +
					std::to_string(m.height) + ")");
		}
		if (m.texels.size() != size_t(m.width) * m.height) {
			return make_error("combat-area mask with a short texel buffer");
		}
	}
	if (w == 0) {
		return make_error("no combat-area mask");
	}
	std::vector<bool> out(size_t(w) * h, false);
	for (const L8Image &m : p_masks) {
		if (m.empty()) {
			continue;
		}
		for (size_t i = 0; i < out.size(); ++i) {
			if (sdf_inside(m.texels[i], p_threshold)) {
				out[i] = true;
			}
		}
	}
	return out;
}

Result<L8Image> combat_area_min_field(std::span<const L8Image> p_masks) {
	L8Image out;
	for (const L8Image &m : p_masks) {
		if (m.empty()) {
			continue;
		}
		if (m.texels.size() != size_t(m.width) * m.height) {
			return make_error("combat-area mask with a short texel buffer");
		}
		if (out.empty()) {
			out = m;
			continue;
		}
		if (m.width != out.width || m.height != out.height) {
			return make_error("combat-area masks of different sizes");
		}
		for (size_t i = 0; i < out.texels.size(); ++i) {
			out.texels[i] = std::min(out.texels[i], m.texels[i]);
		}
	}
	if (out.empty()) {
		return make_error("no combat-area mask");
	}
	return out;
}

double mask_box_iou(const std::vector<bool> &p_mask, uint32_t p_side, const MinimapMapping &p_mapping, uint8_t p_variant,
		std::array<float, 4> p_box_x0_x1_z0_z1) {
	const auto [x0, x1, z0, z1] = p_box_x0_x1_z0_z1;
	if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(z0) || !std::isfinite(z1)) {
		return std::nan("");
	}
	return mask_iou(p_mask, p_side, p_mapping, p_variant, [&](float p_x, float p_z) { return p_x >= x0 && p_x <= x1 && p_z >= z0 && p_z <= z1; });
}

double mask_polygon_iou(const std::vector<bool> &p_mask, uint32_t p_side, const MinimapMapping &p_mapping, uint8_t p_variant,
		std::span<const std::array<float, 2>> p_polygon) {
	if (p_polygon.size() < 3 || p_polygon.size() > kMaxAreaPolygonPoints) {
		return std::nan("");
	}
	for (const std::array<float, 2> &p : p_polygon) {
		if (!std::isfinite(p[0]) || !std::isfinite(p[1])) {
			return std::nan("");
		}
	}
	return mask_iou(p_mask, p_side, p_mapping, p_variant, [&](float p_x, float p_z) { return in_polygon(p_polygon, p_x, p_z); });
}

Result<std::vector<std::array<float, 2>>> read_area_polygon(const level::LoadedLevel &p_level, const level::World &p_world, std::string_view p_partition) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	const EbxResolver &db = p_level.db();
	const level::PlacedObject *found = nullptr;
	size_t count = 0;
	for (const level::PlacedObject &object : p_world.objects) {
		BF3_TRY(budget.charge(1));
		if (object.kind != level::ObjectKind::VolumeVectorShape) {
			continue;
		}
		const std::optional<std::string_view> name = db.name_of(object.entity.partition().guid());
		if (!name || !iequals_ascii(*name, p_partition)) {
			continue;
		}
		++count;
		found = &object;
	}
	if (count != 1) {
		return make_error(std::string(p_partition) + ": " + std::to_string(count) + " VolumeVectorShape placements, expected exactly 1");
	}
	const std::optional<EbxValue> points = found->entity.get("Points");
	const OptionalEbxArray list = points ? points->as_array() : OptionalEbxArray();
	if (!list) {
		return make_error(std::string(p_partition) + ": the VolumeVectorShapeData has no Points");
	}
	if (list->size() < 3 || list->size() > kMaxAreaPolygonPoints) {
		return make_error(std::string(p_partition) + ": " + std::to_string(list->size()) + " points (3.." + std::to_string(kMaxAreaPolygonPoints) + " expected)");
	}
	BF3_TRY(budget.charge(list->size()));
	std::vector<std::array<float, 2>> out;
	out.reserve(list->size());
	for (const EbxValue p : *list) {
		const std::optional<core::Vec3> v = p.as_vec3();
		if (!v || !std::isfinite((*v)[0]) || !std::isfinite((*v)[1]) || !std::isfinite((*v)[2])) {
			return make_error(std::string(p_partition) + ": a point is missing or not finite");
		}
		const level::Vec3 w = found->transform.transform_point(*v);
		if (!std::isfinite(w[0]) || !std::isfinite(w[2])) {
			return make_error(std::string(p_partition) + ": a placed point is not finite");
		}
		out.push_back({ w[0], w[2] });
	}
	return out;
}

}

#include "bf3/fx/triggers.h"

#include "fx_ebx.h"

#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/util/finite.h"
#include "bf3/sim/weapons/sim_time.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace bf3::fx {

namespace {

namespace gp = core::gameplay;

constexpr float kPi = 3.14159265358979f;
constexpr size_t kMaxFireEffects = 32;
constexpr size_t kFireEffectsWork = 4096;
constexpr uint32_t kMaxShotMemory = 4096;

Vec3 add(const Vec3 &a, const Vec3 &b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 sub(const Vec3 &a, const Vec3 &b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 mul(const Vec3 &a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3 &a, const Vec3 &b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
using bf3::core::finite;
Vec3 normalize_or(const Vec3 &a, const Vec3 &p_fallback) {
	const float l = length(a);
	if (!(l > 1e-6f) || !std::isfinite(l)) {
		return p_fallback;
	}
	return mul(a, 1.0f / l);
}
Vec3 from(const float (&p)[3]) { return { p[0], p[1], p[2] }; }

Vec3 rotate(const Transform &t, const Vec3 &p) { return add(add(mul(t.right, p.x), mul(t.up, p.y)), mul(t.forward, p.z)); }
Vec3 apply(const Transform &t, const Vec3 &p) { return add(t.trans, rotate(t, p)); }

Transform compose(const Transform &a, const Transform &b) {
	Transform c;
	c.right = rotate(a, b.right);
	c.up = rotate(a, b.up);
	c.forward = rotate(a, b.forward);
	c.trans = apply(a, b.trans);
	return c;
}

Transform inverse(const Transform &t) {
	Transform i;
	i.right = { t.right.x, t.up.x, t.forward.x };
	i.up = { t.right.y, t.up.y, t.forward.y };
	i.forward = { t.right.z, t.up.z, t.forward.z };
	i.trans = mul(rotate(i, t.trans), -1.0f);
	return i;
}

float seconds_of(int64_t p_units) {
	return float(double(p_units) / double(sim::weapons::kTimeUnitsPerSecond));
}

Vec3 spread_direction(const Vec3 &p_axis, float p_degrees, uint64_t p_seed) {
	if (!(p_degrees > 0.0f)) {
		return p_axis;
	}
	const float max_rad = std::min(p_degrees, 180.0f) * core::kDegToRadF;
	const float u = fx_unit(trigger_seed(p_seed, TriggerPurpose::ImpactSpread, 0));
	const float v = fx_unit(trigger_seed(p_seed, TriggerPurpose::ImpactSpread, 1));
	const float cos_t = 1.0f - u * (1.0f - std::cos(max_rad));
	const float sin_t = std::sqrt(std::max(0.0f, 1.0f - cos_t * cos_t));
	const float phi = 2.0f * kPi * v;
	const Transform f = effect_frame({}, p_axis, { 0, 0, 1 });
	return normalize_or(add(add(mul(f.up, cos_t), mul(f.right, sin_t * std::cos(phi))), mul(f.forward, sin_t * std::sin(phi))), p_axis);
}

uint8_t zone_material(uint32_t p_zone) {
	switch (p_zone) {
		case 0:
			return kSoldierHeadMaterial;
		case 2:
			return kSoldierChestMaterial;
		default:
			return kSoldierBodyMaterial;
	}
}

std::string_view short_name(std::string_view p_name) {
	const size_t s = p_name.rfind('/');
	return s == std::string_view::npos ? p_name : p_name.substr(s + 1);
}

void read_fire_list(detail::Reader &r, const core::EbxStruct &p_ffd, std::string_view p_field, std::vector<FireEffect> &r_out) {
	const std::optional<core::EbxValue> v = r.maybe(p_ffd, p_field);
	if (!v) {
		return;
	}
	const core::OptionalEbxArray a = r.array(p_ffd, p_field, kMaxFireEffects);
	if (!a) {
		return;
	}
	for (const core::EbxValue e : *a) {
		if (!r.ok() || !r.charge()) {
			return;
		}
		const std::optional<core::EbxStruct> s = e.as_struct();
		if (!s) {
			r.fail(std::string(p_field) + ": an entry is not a struct");
			return;
		}
		const std::string blueprint = r.external_name(*s, "Effect");
		if (!r.ok() || blueprint.empty()) {
			continue;
		}
		FireEffect fe;
		fe.blueprint = blueprint;
		const Vec3 offset = r.vec3(*s, "Offset");
		const std::optional<core::EbxValue> rot = r.maybe(*s, "Rotation");
		const Vec3 rotation = rot ? r.vec3(*s, "Rotation") : Vec3{};
		if (!finite(offset) || !finite(rotation)) {
			r.fail(std::string(p_field) + ": a non-finite Offset or Rotation");
			return;
		}
		fe.offset = fire_effect_offset(offset, rotation);
		fe.update_transform = r.flag_or(*s, "UpdateTransform", false);
		fe.disable_during_zoom = r.flag_or(*s, "DisableDuringZoom", false);
		fe.shell = blueprint.find("/shells/") != std::string::npos;
		r_out.push_back(std::move(fe));
	}
}

std::optional<core::EbxInstance> projectile_of(detail::Reader &r, const core::EbxStruct &p_ffd) {
	const std::optional<core::EbxValue> shot = r.maybe(p_ffd, "Shot");
	const std::optional<core::EbxStruct> ss = shot ? shot->as_struct() : std::nullopt;
	if (!ss || !r.charge()) {
		return std::nullopt;
	}
	return r.db().follow(*ss, "ProjectileData");
}

void read_def(const core::EbxResolver &p_db, const gp::WeaponDef &p_def, size_t p_index, WeaponFireEffects &r_w, std::vector<std::string> &r_notes) {
	r_w.trace_frequency = p_def.fire.trace_frequency;
	r_w.bullet_material = p_def.projectile.material.resolved ? p_def.projectile.material.material() : kDefaultBulletMaterial;
	r_w.tracer_speed = std::isfinite(p_def.fire.initial_speed[2]) ? std::max(0.0f, p_def.fire.initial_speed[2]) : 0.0f;
	r_w.tracer_ttl = std::isfinite(p_def.projectile.time_to_live) ? std::max(0.0f, p_def.projectile.time_to_live) : 0.0f;
	r_w.converge = p_def.projectile.visual_converge_distance > 0.0f && std::isfinite(p_def.projectile.visual_converge_distance) ?
			p_def.projectile.visual_converge_distance :
			kTracerConvergeM;
	const std::string where = "def " + std::to_string(p_index) + " (" + p_def.blueprint + ")";
	const size_t hash = p_def.firing_data.find('#');
	if (hash == std::string::npos) {
		r_notes.push_back(where + ": no firing data");
		r_w.firearm = false;
		return;
	}
	const std::string partition = detail::canonical_name(std::string_view(p_def.firing_data).substr(0, hash));
	const std::optional<core::Guid> guid = core::Guid::parse(std::string_view(p_def.firing_data).substr(hash + 1));
	const core::EbxPartition *part = p_db.partition_by_name(partition);
	const std::optional<core::EbxInstance> wfd = part != nullptr && guid ? part->instance_by_guid(*guid) : std::nullopt;
	if (!wfd) {
		r_notes.push_back(where + ": firing data " + p_def.firing_data + " not found");
		r_w.firearm = false;
		return;
	}
	gp::WorkBudget budget(kFireEffectsWork);
	detail::Reader r(p_db, budget);
	r.set_where(where);
	const std::optional<core::EbxInstance> ffd = p_db.follow(wfd->data(), "PrimaryFire");
	if (!ffd) {
		r_notes.push_back(where + ": WeaponFiringData.PrimaryFire unresolved");
		r_w.firearm = false;
		return;
	}
	const core::EbxStruct f = ffd->data();
	read_fire_list(r, f, "FireEffects1p", r_w.first_person);
	read_fire_list(r, f, "FireEffects3p", r_w.third_person);
	if (const std::optional<core::EbxInstance> proj = projectile_of(r, f)) {
		const core::EbxStruct ps = proj->data();
		if (r.maybe(ps, "TrailEffect")) {
			r_w.tracer = r.external_name(ps, "TrailEffect");
		}
		if (r.ok()) {
			if (const std::optional<core::EbxInstance> ex = p_db.follow(ps, "Explosion")) {
				if (r.maybe(ex->data(), "DetonationEffect")) {
					r_w.detonation = r.external_name(ex->data(), "DetonationEffect");
				}
			}
		}
	}
	if (!r.ok()) {
		r_notes.push_back(r.message());
		r_w.first_person.clear();
		r_w.third_person.clear();
		r_w.tracer.clear();
		r_w.detonation.clear();
	}

	r_w.firearm = p_def.projectile.kind == gp::ProjectileKind::Bullet && !(r_w.first_person.empty() && r_w.third_person.empty());
}

enum class KeyKind : uint8_t { Shot = 1, Impact = 2, Soldier = 3, SoldierHit = 4, Detonation = 5 };

uint64_t start_key(KeyKind p_kind, uint8_t p_slot, uint32_t p_id, uint8_t p_pellet, uint32_t p_ordinal) {
	uint64_t k = splitmix64(
			(uint64_t(uint8_t(p_kind)) << 56) ^ (uint64_t(p_slot) << 48) ^ (uint64_t(p_pellet) << 40) ^ uint64_t(p_id));
	return splitmix64(k ^ uint64_t(p_ordinal));
}

uint64_t shot_key(uint8_t p_slot, uint32_t p_id) {
	return (uint64_t(p_slot) << 32) | uint64_t(p_id);
}

uint64_t pack(EffectHandle p_h) {
	return (uint64_t(p_h.slot) << 32) | uint64_t(p_h.generation);
}

}

Transform effect_frame(const Vec3 &p_origin, const Vec3 &p_axis, const Vec3 &p_forward_hint) {
	const Vec3 u = normalize_or(p_axis, { 0, 1, 0 });
	Vec3 f = finite(p_forward_hint) ? sub(p_forward_hint, mul(u, dot(p_forward_hint, u))) : Vec3{};
	if (length(f) < 1e-4f) {
		f = sub(Vec3{ 0, 0, 1 }, mul(u, u.z));
		if (length(f) < 1e-4f) {
			f = sub(Vec3{ 1, 0, 0 }, mul(u, u.x));
		}
	}
	f = normalize_or(f, { 0, 0, 1 });
	Transform t;
	t.right = normalize_or(cross(u, f), { 1, 0, 0 });
	t.up = u;
	t.forward = f;
	t.trans = p_origin;
	return t;
}

Transform effect_frame_of_weapon(const Transform &p_weapon) {
	return effect_frame(p_weapon.trans, p_weapon.forward, p_weapon.up);
}

Transform weapon_of_effect_frame(const Transform &p_effect) {
	Transform w;
	w.forward = p_effect.up;
	w.up = p_effect.forward;
	w.right = cross(w.up, w.forward);
	w.trans = p_effect.trans;
	return w;
}

MuzzleSample aim_muzzle(uint8_t p_slot, const Vec3 &p_eye, const Vec3 &p_direction, const Vec3 &p_velocity) {
	MuzzleSample m;
	m.slot = p_slot;
	if (!finite(p_eye)) {
		return m;
	}
	const Vec3 d = normalize_or(p_direction, { 0, 0, 1 });
	const Vec3 right = normalize_or(cross(d, Vec3{ 0, 1, 0 }), { 1, 0, 0 });
	const Vec3 up = normalize_or(cross(right, d), { 0, 1, 0 });
	const Vec3 pos = add(add(add(p_eye, mul(right, kFallbackMuzzleRight)), mul(up, kFallbackMuzzleUp)), mul(d, kFallbackMuzzleForward));
	m.valid = true;
	m.transform = effect_frame(pos, d, up);
	m.velocity = finite(p_velocity) ? p_velocity : Vec3{};
	return m;
}

MuzzleSample fallback_muzzle(const present::PresentedSoldier &p_soldier) {
	const float cp = std::cos(p_soldier.pitch);
	const Vec3 dir{ std::sin(p_soldier.yaw) * cp, std::sin(p_soldier.pitch), std::cos(p_soldier.yaw) * cp };
	const Vec3 eye{ p_soldier.position[0], p_soldier.position[1] + p_soldier.eye_height, p_soldier.position[2] };
	return aim_muzzle(p_soldier.slot, eye, dir, { p_soldier.velocity[0], p_soldier.velocity[1], p_soldier.velocity[2] });
}

Transform fire_effect_offset(const Vec3 &p_offset, const Vec3 &p_rotation_degrees) {
	Transform t;
	t.trans = finite(p_offset) ? p_offset : Vec3{};
	if (!finite(p_rotation_degrees) ||
			(p_rotation_degrees.x == 0 && p_rotation_degrees.y == 0 && p_rotation_degrees.z == 0)) {
		return t;
	}
	auto rot = [](Vec3 v, int axis, float a) {
		const float c = std::cos(a), s = std::sin(a);
		if (axis == 0) {
			return Vec3{ v.x, c * v.y - s * v.z, s * v.y + c * v.z };
		}
		if (axis == 1) {
			return Vec3{ c * v.x + s * v.z, v.y, -s * v.x + c * v.z };
		}
		return Vec3{ c * v.x - s * v.y, s * v.x + c * v.y, v.z };
	};
	for (Vec3 *v : { &t.right, &t.up, &t.forward }) {
		*v = rot(rot(rot(*v, 0, p_rotation_degrees.x * core::kDegToRadF), 1, p_rotation_degrees.y * core::kDegToRadF), 2, p_rotation_degrees.z * core::kDegToRadF);
	}
	return t;
}

std::vector<std::string> FireEffectTable::blueprints() const {
	std::set<std::string> names;
	for (const WeaponFireEffects &w : defs) {
		for (const FireEffect &e : w.first_person) {
			names.insert(e.blueprint);
		}
		for (const FireEffect &e : w.third_person) {
			names.insert(e.blueprint);
		}
		if (!w.tracer.empty()) {
			names.insert(w.tracer);
		}
		if (!w.detonation.empty()) {
			names.insert(w.detonation);
		}
	}
	if (!default_detonation.empty()) {
		names.insert(default_detonation);
	}
	return { names.begin(), names.end() };
}

core::Result<FireEffectTable> load_fire_effects(const core::EbxResolver &p_db, std::span<const core::gameplay::WeaponDef> p_defs) {
	FireEffectTable t;
	t.defs.resize(p_defs.size());
	for (size_t i = 0; i < p_defs.size(); ++i) {
		read_def(p_db, p_defs[i], i, t.defs[i], t.notes);
		if (t.default_detonation.empty() && short_name(t.defs[i].detonation) == kGrenadeEffectName) {
			t.default_detonation = t.defs[i].detonation;
		}
	}
	return t;
}

core::Status add_unlock_fire_effects(
		FireEffectTable &r_table, const core::EbxResolver &p_db, uint16_t p_def, std::string_view p_weapon_unlock) {
	if (p_def >= r_table.defs.size()) {
		return core::make_error(
				std::string(p_weapon_unlock) + ": def " + std::to_string(p_def) + " has no fire effects");
	}
	const core::Result<std::vector<gp::WeaponUnlockModifier>> mods =
			gp::weapon_unlock_modifiers(p_db, p_weapon_unlock, "WeaponFiringEffectsModifier");
	if (!mods) {
		return mods.error();
	}
	for (const gp::WeaponUnlockModifier &m : *mods) {
		gp::WorkBudget budget(kFireEffectsWork);
		detail::Reader r(p_db, budget);
		r.set_where(std::string(p_weapon_unlock) + " " + m.unlock);
		WeaponFireEffects w = r_table.defs[p_def];
		std::vector<FireEffect> first, third;
		const core::EbxStruct f = m.modifier.data();
		read_fire_list(r, f, "FireEffects1p", first);
		read_fire_list(r, f, "FireEffects3p", third);
		if (!r.ok()) {
			return core::make_error(r.message());
		}
		if (first.empty() && third.empty()) {
			continue;
		}
		if (!first.empty()) {
			w.first_person = std::move(first);
		}
		if (!third.empty()) {
			w.third_person = std::move(third);
		}
		r_table.variants.push_back(FireEffectVariant{ p_def, uint16_t(r_table.defs.size()), m.unlock });
		r_table.defs.push_back(std::move(w));
	}
	return core::Status();
}

std::vector<std::string> impact_sound_names(const ImpactTable &p_table) {
	std::set<std::string> names;
	for (size_t r = 0; r < p_table.rows(); ++r) {
		for (size_t c = 0; c < p_table.rows(); ++c) {
			const ImpactCell &cell = p_table.cell(r, c);
			if (!cell.impact_sound.empty()) {
				names.insert(cell.impact_sound);
			}
		}
	}
	return { names.begin(), names.end() };
}

std::vector<std::string> impact_effect_names(const ImpactTable &p_table) {
	std::set<std::string> names;
	for (size_t r = 0; r < p_table.rows(); ++r) {
		for (size_t c = 0; c < p_table.rows(); ++c) {
			for (const std::string &e : p_table.cell(r, c).effects) {
				names.insert(e);
			}
		}
	}
	return { names.begin(), names.end() };
}

std::vector<std::string> impact_decal_names(const ImpactTable &p_table) {
	std::set<std::string> names;
	names.insert(std::string(kScorchDecal));
	for (size_t r = 0; r < p_table.rows(); ++r) {
		for (size_t c = 0; c < p_table.rows(); ++c) {
			const ImpactCell &cell = p_table.cell(r, c);
			if (!cell.decal.empty()) {
				names.insert(cell.decal);
			}
		}
	}
	return { names.begin(), names.end() };
}

std::string pick_decal_texture(std::string_view p_decal, std::span<const std::string> p_textures) {
	const std::string decal = detail::canonical_name(p_decal);
	const size_t slash = decal.rfind('/');
	const std::string folder = slash == std::string::npos ? std::string() : decal.substr(0, slash + 1);
	const std::string leaf = decal.substr(folder.size());
	std::string best;
	size_t best_prefix = 0;
	for (const std::string &t : p_textures) {
		if (t == decal + "_d") {
			return t;
		}
		if (t.size() <= folder.size() || t.compare(0, folder.size(), folder) != 0 || t.find('/', folder.size()) != std::string::npos) {
			continue;
		}
		const std::string_view tl = std::string_view(t).substr(folder.size());
		const bool diffuse = tl.ends_with("_d") || tl.ends_with("_c");
		if (!diffuse || tl.find("glow") != std::string_view::npos) {
			continue;
		}
		size_t prefix = 0;
		while (prefix < tl.size() && prefix < leaf.size() && tl[prefix] == leaf[prefix]) {
			++prefix;
		}
		if (prefix > best_prefix || (prefix == best_prefix && !best.empty() && t < best)) {
			best_prefix = prefix;
			best = t;
		}
	}
	return best_prefix >= 6 ? best : std::string();
}

core::Result<DecalTemplate> load_decal_template(const core::EbxResolver &p_db, std::string_view p_name, std::span<const std::string> p_textures) {
	const std::string name = detail::canonical_name(p_name);
	const core::EbxPartition *part = p_db.partition_by_name(name);
	if (part == nullptr) {
		return core::make_error(name + ": decal partition not found");
	}
	const core::EbxInstance primary = part->primary();
	if (primary.type_name() != "DecalTemplateData") {
		return core::make_error(name + ": primary is " + std::string(primary.type_name()) + ", not DecalTemplateData");
	}
	gp::WorkBudget budget(1024);
	detail::Reader r(p_db, budget);
	r.set_where(name);
	const core::EbxStruct s = primary.data();
	DecalTemplate t;
	t.name = name;
	t.size = r.number_or(s, "Size", 0.2f);
	t.random_size = r.number_or(s, "RandomSize", 0.0f);
	t.rotation = r.number_or(s, "Rotation", 0.0f);
	t.random_rotation = r.number_or(s, "RandomRotation", 0.0f);
	t.normal_offset = r.number_or(s, "NormalOffset", 0.001f);
	t.clip_angle = r.number_or(s, "ClipAngle", 70.0f);
	if (const std::optional<core::EbxValue> tile = r.maybe(s, "AtlasTile")) {
		if (const std::optional<core::EbxStruct> ts = tile->as_struct()) {
			auto count = [&](std::string_view p_field, uint32_t p_default) {
				const float v = r.number_or(*ts, p_field, float(p_default));
				return v >= 0.0f && v <= 64.0f ? uint32_t(v) : p_default;
			};
			t.tiles_x = std::max(1u, count("TileCountX", 1));
			t.tiles_y = std::max(1u, count("TileCountY", 1));
			t.tile_x = count("TileIndexX", 0);
			t.tile_y = count("TileIndexY", 0);
		}
	}
	if (!r.ok()) {
		return r.error();
	}
	if (!(t.size > 0.0f) || t.size > 100.0f) {
		return core::make_error(name + ": Size " + std::to_string(t.size) + " out of (0, 100]");
	}
	for (const float v : { t.random_size, t.rotation, t.random_rotation, t.normal_offset, t.clip_angle }) {
		if (!std::isfinite(v)) {
			return core::make_error(name + ": a non-finite RandomSize, Rotation, RandomRotation, NormalOffset or ClipAngle");
		}
	}
	t.random_size = std::clamp(t.random_size, 0.0f, 100.0f);
	t.random_rotation = std::clamp(t.random_rotation, 0.0f, 360.0f);
	t.texture = pick_decal_texture(name, p_textures);
	return t;
}

DecalPlacement decal_placement(const DecalTemplate &p_template, uint64_t p_seed) {
	DecalPlacement d;
	d.size = p_template.size + fx_unit(splitmix64(p_seed ^ 0x51ull)) * p_template.random_size;
	const float deg =
			p_template.rotation + (2.0f * fx_unit(splitmix64(p_seed ^ 0x52ull)) - 1.0f) * p_template.random_rotation;
	d.rotation = deg * core::kDegToRadF;
	return d;
}

namespace {

core::Result<std::vector<uint8_t>> decode_bc_level(std::span<const uint8_t> p_blocks, uint32_t p_width, uint32_t p_height, bool p_bc3, const char *p_what) {
	if (p_width == 0 || p_height == 0 || p_width > 4096 || p_height > 4096) {
		return core::Error{ std::string(p_what) + ": size out of range" };
	}
	const size_t stride = p_bc3 ? 16 : 8;
	const uint32_t bw = (p_width + 3) / 4, bh = (p_height + 3) / 4;
	if (p_blocks.size() < size_t(bw) * bh * stride) {
		return core::Error{ std::string(p_what) + ": fewer bytes than the blocks need" };
	}
	std::vector<uint8_t> out(size_t(p_width) * p_height * 4);
	auto expand = [](uint32_t p_c, uint8_t *r_rgb) {
		const uint32_t r = (p_c >> 11) & 31, g = (p_c >> 5) & 63, b = p_c & 31;
		r_rgb[0] = uint8_t((r * 527 + 23) >> 6);
		r_rgb[1] = uint8_t((g * 259 + 33) >> 6);
		r_rgb[2] = uint8_t((b * 527 + 23) >> 6);
	};
	for (uint32_t by = 0; by < bh; ++by) {
		for (uint32_t bx = 0; bx < bw; ++bx) {
			const uint8_t *block = p_blocks.data() + (size_t(by) * bw + bx) * stride;
			const uint8_t *b = block + (stride - 8);
			const uint32_t c0 = uint32_t(b[0]) | (uint32_t(b[1]) << 8), c1 = uint32_t(b[2]) | (uint32_t(b[3]) << 8);
			const bool four = p_bc3 || c0 > c1;
			uint8_t pal[4][4] = {};
			expand(c0, pal[0]);
			expand(c1, pal[1]);
			for (int k = 0; k < 3; ++k) {
				if (four) {
					pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
					pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
				} else {
					pal[2][k] = uint8_t((pal[0][k] + pal[1][k] + 1) / 2);
					pal[3][k] = 0;
				}
			}
			pal[0][3] = pal[1][3] = pal[2][3] = 255;
			pal[3][3] = four ? 255 : 0;
			uint8_t alpha[16];
			if (p_bc3) {
				const int a0 = block[0], a1 = block[1];
				int ap[8] = { a0, a1, 0, 0, 0, 0, 0, 0 };
				if (a0 > a1) {
					for (int i = 1; i <= 6; ++i) {
						ap[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
					}
				} else {
					for (int i = 1; i <= 4; ++i) {
						ap[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
					}
					ap[6] = 0;
					ap[7] = 255;
				}
				uint64_t abits = 0;
				for (int i = 0; i < 6; ++i) {
					abits |= uint64_t(block[2 + i]) << (8 * i);
				}
				for (int i = 0; i < 16; ++i) {
					alpha[i] = uint8_t(ap[(abits >> (3 * i)) & 7]);
				}
			}
			const uint32_t bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
			for (uint32_t i = 0; i < 16; ++i) {
				const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
				if (x >= p_width || y >= p_height) {
					continue;
				}
				const uint8_t *c = pal[(bits >> (2 * i)) & 3];
				uint8_t *o = out.data() + (size_t(y) * p_width + x) * 4;
				o[0] = c[0];
				o[1] = c[1];
				o[2] = c[2];
				o[3] = p_bc3 ? alpha[i] : c[3];
			}
		}
	}
	return out;
}

}

core::Result<std::vector<uint8_t>> decode_bc1(std::span<const uint8_t> p_blocks, uint32_t p_width, uint32_t p_height) {
	return decode_bc_level(p_blocks, p_width, p_height, false, "decode_bc1");
}

core::Result<std::vector<uint8_t>> decode_bc3(std::span<const uint8_t> p_blocks, uint32_t p_width, uint32_t p_height) {
	return decode_bc_level(p_blocks, p_width, p_height, true, "decode_bc3");
}

float magenta_share(std::span<const uint8_t> p_rgba) {
	const size_t n = p_rgba.size() / 4;
	if (n == 0) {
		return 0.0f;
	}
	size_t keyed = 0;
	for (size_t i = 0; i < n; ++i) {
		const uint8_t *t = p_rgba.data() + i * 4;
		keyed += t[0] >= 230 && t[2] >= 230 && t[1] <= 25 ? 1 : 0;
	}
	return float(double(keyed) / double(n));
}

void unkey_magenta(std::span<uint8_t> r_rgba) {
	auto q = [](float p_v) {
		return uint8_t(std::lround(std::clamp(p_v, 0.0f, 1.0f) * 255.0f));
	};
	for (size_t i = 0; i + 3 < r_rgba.size(); i += 4) {
		uint8_t *t = r_rgba.data() + i;
		const float r = float(t[0]) / 255.0f, g = float(t[1]) / 255.0f, b = float(t[2]) / 255.0f;
		const float k = std::clamp(std::min(r, b) - g, 0.0f, 1.0f);
		const float a = 1.0f - k;
		if (a < 1.0f / 255.0f) {
			t[0] = t[1] = t[2] = t[3] = 0;
			continue;
		}
		const float a0 = float(t[3]) / 255.0f;
		const uint8_t grey = q(g / a);
		t[0] = grey;
		t[1] = grey;
		t[2] = grey;
		t[3] = q(a * a0);
	}
}

float transparent_share(std::span<const uint8_t> p_rgba) {
	const size_t n = p_rgba.size() / 4;
	if (n == 0) {
		return 0.0f;
	}
	size_t clear = 0;
	for (size_t i = 0; i < n; ++i) {
		clear += p_rgba[i * 4 + 3] < 128 ? 1 : 0;
	}
	return float(double(clear) / double(n));
}

const char *decal_texture_kind_name(DecalTextureKind p_kind) {
	switch (p_kind) {
		case DecalTextureKind::Alpha:
			return "alpha";
		case DecalTextureKind::MagentaKeyed:
			return "magenta-keyed";
		case DecalTextureKind::Opaque:
			return "opaque";
		case DecalTextureKind::Unsupported:
			return "unsupported";
	}
	return "?";
}

core::Result<DecalTexturePrep> prepare_decal_texture(DecalPixelFormat p_format, std::span<const uint8_t> p_data, uint32_t p_width, uint32_t p_height) {
	DecalTexturePrep out;
	if (p_width == 0 || p_height == 0 || p_width > 4096 || p_height > 4096) {
		return core::Error{ "prepare_decal_texture: size out of range" };
	}
	core::Result<std::vector<uint8_t>> rgba = core::Error{ "" };
	switch (p_format) {
		case DecalPixelFormat::Bc1:
			rgba = decode_bc1(p_data, p_width, p_height);
			break;
		case DecalPixelFormat::Bc3:
			rgba = decode_bc3(p_data, p_width, p_height);
			break;
		case DecalPixelFormat::Rgba8: {
			const size_t bytes = size_t(p_width) * p_height * 4;
			if (p_data.size() < bytes) {
				return core::Error{ "prepare_decal_texture: fewer bytes than an RGBA8 level needs" };
			}
			rgba = std::vector<uint8_t>(p_data.begin(), p_data.begin() + ptrdiff_t(bytes));
			break;
		}
		case DecalPixelFormat::Other:
			out.kind = DecalTextureKind::Unsupported;
			return out;
	}
	if (!rgba) {
		return rgba.error();
	}
	out.magenta = magenta_share(*rgba);
	const bool keyed = out.magenta >= kMagentaKeyedShare;
	if (keyed) {
		unkey_magenta(*rgba);
	}
	out.transparent = transparent_share(*rgba);
	if (out.transparent < kMinDecalTransparentShare) {
		out.kind = DecalTextureKind::Opaque;
		return out;
	}
	if (keyed) {
		out.kind = DecalTextureKind::MagentaKeyed;
		out.rgba_chain = rgba_mip_chain(*rgba, p_width, p_height, &out.levels);
		if (out.rgba_chain.empty()) {
			return core::Error{ "prepare_decal_texture: no mip chain" };
		}
	} else {
		out.kind = DecalTextureKind::Alpha;
	}
	return out;
}

std::vector<uint8_t> rgba_mip_chain(std::span<const uint8_t> p_level0, uint32_t p_width, uint32_t p_height, uint32_t *r_levels) {
	if (r_levels != nullptr) {
		*r_levels = 0;
	}
	if (p_width == 0 || p_height == 0 || p_width > 4096 || p_height > 4096 || p_level0.size() < size_t(p_width) * p_height * 4) {
		return {};
	}
	std::vector<uint8_t> out(p_level0.begin(), p_level0.begin() + ptrdiff_t(size_t(p_width) * p_height * 4));
	uint32_t levels = 1;
	size_t prev = 0;
	uint32_t w = p_width, h = p_height;
	while (w > 1 || h > 1) {
		const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
		const size_t base = out.size();
		out.resize(base + size_t(nw) * nh * 4);
		for (uint32_t y = 0; y < nh; ++y) {
			for (uint32_t x = 0; x < nw; ++x) {
				float sum[4] = { 0, 0, 0, 0 };
				for (uint32_t dy = 0; dy < 2; ++dy) {
					for (uint32_t dx = 0; dx < 2; ++dx) {
						const uint32_t sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
						const uint8_t *t = out.data() + prev + (size_t(sy) * w + sx) * 4;
						const float a = float(t[3]) / 255.0f;
						sum[0] += float(t[0]) * a;
						sum[1] += float(t[1]) * a;
						sum[2] += float(t[2]) * a;
						sum[3] += a;
					}
				}
				uint8_t *o = out.data() + base + (size_t(y) * nw + x) * 4;
				for (int k = 0; k < 3; ++k) {
					o[k] = sum[3] > 0.0f ? uint8_t(std::lround(std::clamp(sum[k] / sum[3], 0.0f, 255.0f))) : 0;
				}
				o[3] = uint8_t(std::lround(std::clamp(sum[3] * 0.25f * 255.0f, 0.0f, 255.0f)));
			}
		}
		prev = base;
		w = nw;
		h = nh;
		++levels;
	}
	if (r_levels != nullptr) {
		*r_levels = levels;
	}
	return out;
}

void effect_sound_cues(std::span<const FxEvent> p_events, std::vector<EffectSoundCue> &r_out) {
	for (const FxEvent &e : p_events) {
		if (e.kind != FxEvent::Kind::SoundStart && e.kind != FxEvent::Kind::SoundStop) {
			continue;
		}
		if (e.def == nullptr || e.component >= e.def->components.size()) {
			continue;
		}
		const EffectComponentDef &c = e.def->components[e.component];
		if (c.asset.empty()) {
			continue;
		}
		EffectSoundCue cue;
		cue.patch = c.asset;
		cue.effect = pack(e.effect);
		cue.component = e.component;
		cue.position = e.transform.trans;
		cue.start = e.kind == FxEvent::Kind::SoundStart;
		r_out.push_back(cue);
	}
}

struct FxTriggers::Impl {
	FxWorld &world;
	EffectLibrary &library;
	std::shared_ptr<const FireEffectTable> fire;
	FxTriggerConfig config;
	FxTriggerStats stats;

	std::vector<MuzzleSample> muzzles;
	std::array<int32_t, 256> own_1p{};
	std::array<int32_t, 256> third{};

	std::vector<DecalCue> decals;
	std::vector<ImpactSoundCue> sounds;
	std::vector<TriggerStart> starts;

	std::deque<present::PresentEvent> pending;

	std::unordered_set<uint64_t> seen;
	std::deque<uint64_t> seen_order;

	struct ShotMemory {
		Vec3 direction;
		uint16_t def = 0xFFFF;
	};
	std::unordered_map<uint64_t, ShotMemory> shots;
	std::deque<uint64_t> shot_order;

	struct Attached {
		EffectHandle handle;
		uint8_t slot = 0;
		bool first_person = false;
		uint16_t def = 0;
		uint32_t entry = 0;
	};
	std::vector<Attached> attached;

	struct Tracer {
		EffectHandle handle;
		present::PresentTimeline timeline = present::PresentTimeline::Predicted;
		int64_t t0 = 0;
		Vec3 muzzle;
		Vec3 origin;
		Vec3 direction;
		float speed = 0;
		float max_distance = 0;
		float converge = kTracerConvergeM;
		uint8_t slot = 0;
		uint32_t id = 0;
	};
	std::vector<Tracer> tracers;

	Impl(FxWorld &r_world, EffectLibrary &r_library, std::shared_ptr<const FireEffectTable> p_fire, const FxTriggerConfig &p_config) :
			world(r_world), library(r_library), fire(std::move(p_fire)), config(p_config) {
		own_1p.fill(-1);
		third.fill(-1);
	}

	const WeaponFireEffects *def_of(uint16_t p_def) const {
		return fire && p_def < fire->defs.size() ? &fire->defs[p_def] : nullptr;
	}

	std::array<std::pair<uint16_t, uint16_t>, 256> slot_effects{};

	uint16_t effects_of(uint8_t p_slot, uint16_t p_def) const {
		const std::pair<uint16_t, uint16_t> &e = slot_effects[p_slot];
		return e.first == p_def && e.second != p_def ? e.second : p_def;
	}

	bool first_time(uint64_t p_key, bool p_count_double = true) {
		if (seen.contains(p_key)) {
			if (p_count_double) {
				++stats.double_starts;
			}
			return false;
		}
		seen.insert(p_key);
		seen_order.push_back(p_key);
		while (seen_order.size() > kDedupeKeys) {
			seen.erase(seen_order.front());
			seen_order.pop_front();
		}
		return true;
	}

	void remember_shot(uint8_t p_slot, uint32_t p_id, const Vec3 &p_dir, uint16_t p_def) {
		const uint64_t k = shot_key(p_slot, p_id);
		if (!shots.contains(k)) {
			shot_order.push_back(k);
		}
		shots[k] = ShotMemory{ p_dir, p_def };
		while (shot_order.size() > kMaxShotMemory) {
			shots.erase(shot_order.front());
			shot_order.pop_front();
		}
	}
	const ShotMemory *shot_of(uint8_t p_slot, uint32_t p_id) const {
		const auto it = shots.find(shot_key(p_slot, p_id));
		return it == shots.end() ? nullptr : &it->second;
	}

	std::shared_ptr<const EffectDef> get(std::string_view p_blueprint) {
		core::Result<std::shared_ptr<const EffectDef>> d = library.get(p_blueprint);
		if (!d) {
			++stats.compile_failures;
			return nullptr;
		}
		return *d;
	}

	EffectHandle start(const std::shared_ptr<const EffectDef> &p_def, const present::PresentEvent &p_e, TriggerPurpose p_purpose, uint32_t p_index, uint64_t p_seed,
			const Transform &p_frame, const Vec3 &p_velocity, float p_age, bool p_first_person, bool *r_culled = nullptr) {
		TriggerStart s;
		s.kind = p_e.kind;
		s.purpose = p_purpose;
		s.index = p_index;
		s.slot = p_e.slot;
		s.id = p_e.id;
		s.pellet = p_e.pellet;
		s.seed = p_seed;
		s.def = p_def;
		s.transform = p_frame;
		s.start_age = p_age;
		s.first_person = p_first_person;
		if (r_culled != nullptr) {
			*r_culled = false;
		}
		if (p_def->cull_distance > 0.0f && !p_first_person) {
			const float d = length(sub(p_frame.trans, view.camera_position));
			if (d > p_def->cull_distance) {
				++stats.culled;
				if (r_culled != nullptr) {
					*r_culled = true;
				}
				starts.push_back(std::move(s));
				return {};
			}
		}
		EffectStart es;
		es.transform = p_frame;
		es.velocity = p_velocity;
		es.seed = p_seed;
		es.tier = config.tier;
		es.first_person = p_first_person;
		es.start_age = p_age;
		core::Result<EffectHandle> h = world.start(p_def, es);
		if (!h) {
			++stats.start_failures;
			starts.push_back(std::move(s));
			return {};
		}
		s.started = true;
		s.handle = *h;
		starts.push_back(std::move(s));
		return *h;
	}

	FxView view;

	bool age_of(const present::PresentEvent &p_e, const present::PresentClock &p_now, float &r_age) const {
		if (p_e.timeline == present::PresentTimeline::Immediate) {
			r_age = 0.0f;
			return true;
		}
		if (!p_now.valid(p_e.timeline)) {
			return false;
		}
		const int64_t now = p_now.now(p_e.timeline);
		if (now < p_e.time) {
			return false;
		}
		r_age = std::clamp(seconds_of(now - p_e.time), 0.0f, kMaxStartAge);
		return true;
	}

	const MuzzleSample *muzzle_for(uint8_t p_slot, bool p_first_person) const {
		const int32_t i = p_first_person ? own_1p[p_slot] : third[p_slot];
		return i >= 0 ? &muzzles[size_t(i)] : nullptr;
	}

	static Transform entry_frame(const std::vector<FireEffect> &p_list, size_t p_i, const MuzzleSample &p_m) {
		if (p_i == 0) {
			return p_m.transform;
		}
		const Transform w_ref = weapon_of_effect_frame(p_m.transform);
		const Transform w_root = compose(w_ref, inverse(p_list[0].offset));
		return effect_frame_of_weapon(compose(w_root, p_list[p_i].offset));
	}

	void on_shot(const present::PresentEvent &p_e, float p_age) {
		const bool own = p_e.kind == present::PresentKind::OwnShot;
		++(own ? stats.own_shots : stats.remote_shots);
		const uint16_t effects = effects_of(p_e.slot, p_e.def);
		const WeaponFireEffects *w = def_of(effects);
		const Vec3 origin = from(p_e.pos);
		const Vec3 dir = normalize_or(from(p_e.dir), { 0, 0, 1 });
		remember_shot(p_e.slot, p_e.id, dir, p_e.def);
		if (w == nullptr) {
			++stats.unknown_defs;
			return;
		}
		if (!first_time(start_key(KeyKind::Shot, p_e.slot, p_e.id, 0, 0))) {
			return;
		}
		if (w->firearm) {
			++stats.firearm_shots;
		}
		if (effects != p_e.def) {
			++stats.accessory_effects;
		}
		MuzzleSample m;
		if (const MuzzleSample *s = muzzle_for(p_e.slot, own)) {
			m = *s;
		} else if (const MuzzleSample *s3 = own ? muzzle_for(p_e.slot, false) : nullptr) {
			m = *s3;
		} else {
			m = aim_muzzle(p_e.slot, origin, dir, {});
			++stats.muzzle_fallbacks;
		}

		const bool first_person = own && m.first_person;
		const std::vector<FireEffect> &list = first_person ? w->first_person : w->third_person;
		for (size_t i = 0; i < list.size(); ++i) {
			const FireEffect &fe = list[i];
			if (fe.shell && (!config.shells || !w->firearm)) {
				continue;
			}
			const std::shared_ptr<const EffectDef> def = get(fe.blueprint);
			if (!def) {
				continue;
			}
			const TriggerPurpose purpose = fe.shell ? TriggerPurpose::Shell : TriggerPurpose::Muzzle;
			const Transform frame = entry_frame(list, i, m);
			const EffectHandle h = start(def, p_e, purpose, uint32_t(i), trigger_seed(p_e.seed, purpose, uint32_t(i)), frame, m.velocity, p_age, first_person);
			if (!h.valid()) {
				continue;
			}
			++(fe.shell ? stats.shells : stats.muzzle_starts);
			if (fe.update_transform && m.valid) {
				if (attached.size() >= kMaxAttached) {
					attached.erase(attached.begin());
				}
				attached.push_back(Attached{ h, p_e.slot, first_person, effects, uint32_t(i) });
			}
		}
		if ((p_e.flags & present::flags::kTracer) != 0 && !w->tracer.empty()) {
			++stats.tracer_shots;
			if (config.tracers) {
				start_tracer(*w, p_e, m.transform.trans, origin, dir, p_age);
			}
		}
	}

	void start_tracer(const WeaponFireEffects &p_w, const present::PresentEvent &p_e, const Vec3 &p_muzzle, const Vec3 &p_origin, const Vec3 &p_dir, float p_age) {
		const std::shared_ptr<const EffectDef> def = get(p_w.tracer);
		if (!def) {
			return;
		}
		Tracer t;
		t.timeline = p_e.timeline;
		t.t0 = p_e.time;
		t.muzzle = p_muzzle;
		t.origin = p_origin;
		t.direction = p_dir;
		t.speed = p_w.tracer_speed > 0.0f ? p_w.tracer_speed : kTracerFallbackSpeed;
		t.max_distance = t.speed * (p_w.tracer_ttl > 0.0f ? p_w.tracer_ttl : kTracerFallbackTtl);
		t.converge = p_w.converge > 0.0f ? p_w.converge : kTracerConvergeM;
		t.slot = p_e.slot;
		t.id = p_e.id;
		const float s = std::min(p_age * t.speed, t.max_distance);
		const EffectHandle h = start(def, p_e, TriggerPurpose::Tracer, 0, trigger_seed(p_e.seed, TriggerPurpose::Tracer, 0), tracer_frame(t, s),
				mul(p_dir, t.speed), p_age, false);
		if (!h.valid()) {
			return;
		}
		++stats.tracers;
		t.handle = h;
		if (tracers.size() >= kMaxTracers) {
			world.stop(tracers.front().handle, StopMode::Kill);
			tracers.erase(tracers.begin());
			++stats.tracer_steals;
		}
		tracers.push_back(t);
	}

	static Transform tracer_frame(const Tracer &p_t, float p_s) {
		const float k = std::clamp(p_s / std::max(p_t.converge, 0.01f), 0.0f, 1.0f);
		const Vec3 pos = add(add(p_t.origin, mul(p_t.direction, p_s)), mul(sub(p_t.muzzle, p_t.origin), 1.0f - k));
		return effect_frame(pos, p_t.direction, { 0, 1, 0 });
	}

	void end_tracer_at(uint8_t p_slot, uint32_t p_id, const Vec3 &p_pos) {
		for (Tracer &t : tracers) {
			if (t.slot == p_slot && t.id == p_id) {
				const float d = dot(sub(p_pos, t.origin), t.direction);
				if (d >= 0.0f && d < t.max_distance) {
					t.max_distance = d;
				}
			}
		}
	}

	void step_tracers(const present::PresentClock &p_now) {
		size_t keep = 0;
		for (size_t i = 0; i < tracers.size(); ++i) {
			Tracer &t = tracers[i];
			bool live = world.alive(t.handle);
			if (live && (t.timeline == present::PresentTimeline::Immediate || p_now.valid(t.timeline))) {
				const int64_t now = t.timeline == present::PresentTimeline::Immediate ? t.t0 : p_now.now(t.timeline);
				const float s = std::max(0.0f, seconds_of(now - t.t0)) * t.speed;
				if (s >= t.max_distance) {
					world.stop(t.handle, StopMode::Stop);
					live = false;
				} else {
					world.set_transform(t.handle, tracer_frame(t, s), mul(t.direction, t.speed));
				}
			}
			if (live) {
				tracers[keep++] = t;
			}
		}
		tracers.resize(keep);
	}

	void play_cell(const ImpactCell &p_cell, const present::PresentEvent &p_e, uint64_t p_seed, const Vec3 &p_pos, const Vec3 &p_normal, float p_age, bool p_decal,
			bool p_sound) {
		bool culled = false;
		bool started = false;
		if (!p_cell.effects.empty()) {
			const size_t n = p_cell.effects.size();
			const size_t pick = std::min(n - 1, size_t(fx_unit(trigger_seed(p_seed, TriggerPurpose::ImpactChoice, 0)) * float(n)));
			if (const std::shared_ptr<const EffectDef> def = get(p_cell.effects[pick])) {
				Vec3 axis = p_normal;
				if (p_cell.mirror_impact_direction) {
					if (const ShotMemory *s = shot_of(p_e.slot, p_e.id)) {
						const Vec3 d = s->direction;
						const Vec3 r = sub(d, mul(p_normal, 2.0f * dot(d, p_normal)));
						axis = normalize_or(r, p_normal);
					}
				}
				axis = spread_direction(axis, p_cell.max_spread_degrees, p_seed);
				const Transform frame = effect_frame(p_pos, axis, { 0, 1, 0 });
				const EffectHandle h = start(def, p_e, TriggerPurpose::ImpactEffect, uint32_t(pick), trigger_seed(p_seed, TriggerPurpose::ImpactEffect, 0), frame, {},
						p_age, false, &culled);
				started = h.valid();
				if (culled) {
					++stats.impacts_culled;
					return;
				}
			}
		}
		if (started) {
			++stats.impacts;
		}
		if (p_decal && config.decals && !p_cell.decal.empty()) {
			DecalCue d;
			d.decal = p_cell.decal;
			d.transform = effect_frame(p_pos, p_normal, { 0, 1, 0 });
			d.seed = trigger_seed(p_seed, TriggerPurpose::Decal, 0);
			d.start_age = p_age;
			decals.push_back(d);
			++stats.decals;
		}
		if (p_sound && !p_cell.impact_sound.empty()) {
			ImpactSoundCue c;
			c.patch = p_cell.impact_sound;
			c.position = p_pos;
			c.normal = p_normal;
			c.seed = p_seed;
			c.time = p_e.time;
			c.timeline = p_e.timeline;
			sounds.push_back(c);
			++stats.sounds;
		}
	}

	uint8_t projectile_material(uint16_t p_def, uint8_t p_slot, uint32_t p_id) const {
		if (const WeaponFireEffects *w = def_of(p_def)) {
			return w->bullet_material;
		}
		if (const ShotMemory *s = shot_of(p_slot, p_id)) {
			if (const WeaponFireEffects *w = def_of(s->def)) {
				return w->bullet_material;
			}
		}
		return kDefaultBulletMaterial;
	}

	void on_impact(const present::PresentEvent &p_e, float p_age) {
		const Vec3 pos = from(p_e.pos);
		end_tracer_at(p_e.slot, p_e.id, pos);
		if (!config.impacts) {
			return;
		}
		if (!first_time(start_key(KeyKind::Impact, p_e.slot, p_e.id, p_e.pellet, p_e.extra))) {
			return;
		}
		if (p_e.def != 0xFFFF && def_of(p_e.def) == nullptr) {
			++stats.unknown_defs;
		}
		const ImpactTable *table = library.impacts();
		const ImpactCell *cell = table != nullptr ? table->select(projectile_material(p_e.def, p_e.slot, p_e.id), uint8_t(p_e.material & 0xFF)) : nullptr;
		if (cell == nullptr || cell->empty()) {
			++stats.impacts_empty;
			return;
		}
		play_cell(*cell, p_e, p_e.seed, pos, normalize_or(from(p_e.dir), { 0, 1, 0 }), p_age, true, true);
	}

	void on_soldier(const present::PresentEvent &p_e, float p_age) {
		const Vec3 pos = from(p_e.pos);
		const bool contact = p_e.kind == present::PresentKind::SoldierContact;
		if (contact) {
			end_tracer_at(p_e.slot, p_e.id, pos);
		}

		const uint64_t own_key = start_key(contact ? KeyKind::Soldier : KeyKind::SoldierHit, p_e.slot, p_e.id, p_e.pellet, contact ? p_e.aux : 0u);
		if (!first_time(own_key)) {
			return;
		}
		if (!contact && (p_e.flags & present::flags::kOwnVictim) != 0) {
			return;
		}
		if (contact && p_e.slot != config.own_slot) {
			return;
		}
		const uint64_t cell_key = start_key(KeyKind::Soldier, p_e.slot, p_e.id, p_e.pellet, kSoldierOrdinal);
		if (!first_time(cell_key, false)) {
			++stats.hits_confirmed;
			return;
		}
		if (!config.impacts) {
			return;
		}
		const uint32_t zone = contact ? (std::isfinite(p_e.value) ? uint32_t(std::clamp(p_e.value, 0.0f, 3.0f)) : 1u) : p_e.material;
		const ImpactTable *table = library.impacts();
		const ImpactCell *cell = table != nullptr ? table->select(projectile_material(p_e.def, p_e.slot, p_e.id), zone_material(zone)) : nullptr;
		if (cell == nullptr || cell->empty()) {
			++stats.impacts_empty;
			return;
		}
		++stats.soldier_hits;
		const Vec3 n = normalize_or(from(p_e.dir), { 0, 1, 0 });
		play_cell(*cell, p_e, soldier_seed(config.match_seed, p_e.slot, p_e.id, p_e.pellet), pos, n, p_age, true, true);
	}

	void on_detonation(const present::PresentEvent &p_e, float p_age) {
		if (!first_time(start_key(KeyKind::Detonation, p_e.slot, p_e.id, 0, 0))) {
			return;
		}
		if ((p_e.flags & present::flags::kDud) != 0) {
			++stats.duds;
			return;
		}
		const WeaponFireEffects *w = def_of(p_e.def);
		if (p_e.def != 0xFFFF && w == nullptr) {
			++stats.unknown_defs;
		}
		std::string_view name = w != nullptr && !w->detonation.empty() ? std::string_view(w->detonation) : std::string_view();
		if (name.empty() && fire) {
			name = fire->default_detonation;
		}
		const uint64_t seed = detonation_seed(config.match_seed, p_e.slot, p_e.id);
		const Vec3 pos = from(p_e.pos);
		const Vec3 n = normalize_or(from(p_e.dir), { 0, 1, 0 });
		bool culled = false;
		if (!name.empty()) {
			if (const std::shared_ptr<const EffectDef> def = get(name)) {
				const EffectHandle h = start(def, p_e, TriggerPurpose::Detonation, 0, seed, effect_frame(pos, n, { 0, 0, 1 }), {}, p_age, false, &culled);
				if (h.valid()) {
					++stats.detonations;
				}
			}
		}
		if (!culled && config.decals) {
			DecalCue d;
			d.decal = kScorchDecal;
			d.transform = effect_frame(pos, n, { 0, 0, 1 });
			d.seed = trigger_seed(seed, TriggerPurpose::Decal, 0);
			d.start_age = p_age;
			decals.push_back(d);
			++stats.decals;
		}
	}

	bool sane(const present::PresentEvent &p_e) const {
		for (size_t k = 0; k < 3; ++k) {
			if (!std::isfinite(p_e.pos[k]) || !std::isfinite(p_e.dir[k]) || std::fabs(p_e.pos[k]) > 1.0e7f) {
				return false;
			}
		}
		return uint8_t(p_e.kind) <= uint8_t(present::PresentKind::Revive) && uint8_t(p_e.timeline) <= uint8_t(present::PresentTimeline::Immediate);
	}

	void dispatch(const present::PresentEvent &p_e, float p_age) {
		switch (p_e.kind) {
			case present::PresentKind::OwnShot:
			case present::PresentKind::RemoteShot:
				on_shot(p_e, p_age);
				break;
			case present::PresentKind::Impact:
				on_impact(p_e, p_age);
				break;
			case present::PresentKind::SoldierContact:
			case present::PresentKind::Hit:
				on_soldier(p_e, p_age);
				break;
			case present::PresentKind::Detonation:
				on_detonation(p_e, p_age);
				break;
			default:
				break;
		}
	}

	void update_attached() {
		size_t keep = 0;
		for (size_t i = 0; i < attached.size(); ++i) {
			const Attached &a = attached[i];
			if (!world.alive(a.handle)) {
				continue;
			}
			const MuzzleSample *m = muzzle_for(a.slot, a.first_person);
			const WeaponFireEffects *w = def_of(a.def);
			if (m != nullptr && w != nullptr) {
				const std::vector<FireEffect> &list = a.first_person ? w->first_person : w->third_person;
				if (a.entry < list.size()) {
					world.set_transform(a.handle, entry_frame(list, a.entry, *m), m->velocity);
					++stats.attached_updates;
				}
			}
			attached[keep++] = a;
		}
		attached.resize(keep);
	}
};

FxTriggers::FxTriggers(FxWorld &r_world, EffectLibrary &r_library, std::shared_ptr<const FireEffectTable> p_fire, const FxTriggerConfig &p_config) :
		impl_(std::make_unique<Impl>(r_world, r_library, std::move(p_fire), p_config)) {}

FxTriggers::~FxTriggers() = default;

void FxTriggers::set_config(const FxTriggerConfig &p_config) {
	impl_->config = p_config;
}

void FxTriggers::set_slot_effects(uint8_t p_slot, uint16_t p_def, uint16_t p_effects) {
	impl_->slot_effects[p_slot] = { p_def, p_effects };
}

void FxTriggers::on_muzzles(std::span<const MuzzleSample> p_muzzles) {
	Impl &m = *impl_;
	m.muzzles.assign(p_muzzles.begin(), p_muzzles.end());
	m.own_1p.fill(-1);
	m.third.fill(-1);
	for (size_t i = 0; i < m.muzzles.size(); ++i) {
		const MuzzleSample &s = m.muzzles[i];
		if (!s.valid || !finite(s.transform.trans) || !finite(s.transform.up) || !finite(s.transform.forward) ||
				!finite(s.transform.right)) {
			continue;
		}
		if (!finite(s.velocity)) {
			m.muzzles[i].velocity = {};
		}
		(s.first_person ? m.own_1p : m.third)[s.slot] = int32_t(i);
	}
	m.update_attached();
}

void FxTriggers::on_events(std::span<const present::PresentEvent> p_events, const present::PresentClock &p_now, const FxView &p_view) {
	Impl &m = *impl_;
	m.decals.clear();
	m.sounds.clear();
	m.starts.clear();
	m.view = p_view;
	if (!finite(m.view.camera_position)) {
		m.view.camera_position = {};
	}

	const size_t held = m.pending.size();
	for (size_t i = 0; i < held; ++i) {
		const present::PresentEvent e = m.pending.front();
		m.pending.pop_front();
		float age = 0;
		if (m.age_of(e, p_now, age)) {
			m.dispatch(e, age);
		} else {
			m.pending.push_back(e);
		}
	}
	for (const present::PresentEvent &e : p_events) {
		++m.stats.events;
		if (!m.sane(e)) {
			++m.stats.rejected;
			continue;
		}
		float age = 0;
		if (m.age_of(e, p_now, age)) {
			m.dispatch(e, age);
			continue;
		}
		if (m.pending.size() >= kMaxPendingStarts) {
			m.pending.pop_front();
			++m.stats.pending_dropped;
		}
		m.pending.push_back(e);
	}
	m.stats.pending_now = m.pending.size();
	m.step_tracers(p_now);
}

std::span<const DecalCue> FxTriggers::decals() const {
	return impl_->decals;
}

std::span<const ImpactSoundCue> FxTriggers::sounds() const {
	return impl_->sounds;
}

const FxTriggerStats &FxTriggers::stats() const {
	return impl_->stats;
}

std::span<const TriggerStart> FxTriggers::starts() const {
	return impl_->starts;
}

size_t FxTriggers::pending() const {
	return impl_->pending.size();
}

size_t FxTriggers::live_tracers() const {
	return impl_->tracers.size();
}

}

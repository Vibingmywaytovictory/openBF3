// VFX3: gameplay events -> FxWorld::start (triggers.h; docs/design/m2a-fx3.md).
#include "bf3/fx/triggers.h"

#include "fx_ebx.h"

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

namespace hk = host;
namespace gp = core::gameplay;

constexpr float kPi = 3.14159265358979f;
constexpr size_t kMaxFireEffects = 32; // FireEffects1p / 3p entries per def (retail 3)
constexpr size_t kFireEffectsWork = 4096; // WorkBudget units per def
constexpr uint32_t kMaxShotMemory = 4096; // (slot, shot) -> direction / def, for mirror and Hit defs

Vec3 add(const Vec3 &a, const Vec3 &b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 sub(const Vec3 &a, const Vec3 &b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 mul(const Vec3 &a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3 &a, const Vec3 &b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
bool finite3(const Vec3 &a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
Vec3 normalize_or(const Vec3 &a, const Vec3 &p_fallback) {
	const float l = length(a);
	if (!(l > 1e-6f) || !std::isfinite(l)) {
		return p_fallback;
	}
	return mul(a, 1.0f / l);
}
Vec3 from(const float (&p)[3]) { return { p[0], p[1], p[2] }; }

// A point / direction of p_t's local frame in the world.
Vec3 rotate(const Transform &t, const Vec3 &p) { return add(add(mul(t.right, p.x), mul(t.up, p.y)), mul(t.forward, p.z)); }
Vec3 apply(const Transform &t, const Vec3 &p) { return add(t.trans, rotate(t, p)); }
// a ∘ b: b given in a's frame.
Transform compose(const Transform &a, const Transform &b) {
	Transform c;
	c.right = rotate(a, b.right);
	c.up = rotate(a, b.up);
	c.forward = rotate(a, b.forward);
	c.trans = apply(a, b.trans);
	return c;
}
// The inverse of a rigid transform (orthonormal rows).
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

// A direction inside a cone of p_degrees around p_axis, uniform in solid angle, from two draws.
Vec3 spread_direction(const Vec3 &p_axis, float p_degrees, uint64_t p_seed) {
	if (!(p_degrees > 0.0f)) {
		return p_axis;
	}
	const float max_rad = std::min(p_degrees, 180.0f) * (kPi / 180.0f);
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
			return kSoldierHeadMaterial; // HitZone::Head
		case 2:
			return kSoldierChestMaterial; // HitZone::Chest
		default:
			return kSoldierBodyMaterial; // Body, Legs (Legs -> Body [inferred]: the grid has no leg cell of its own)
	}
}

std::string_view short_name(std::string_view p_name) {
	const size_t s = p_name.rfind('/');
	return s == std::string_view::npos ? p_name : p_name.substr(s + 1);
}

// ---- load_fire_effects -----------------------------------------------------------------------------------

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
		fe.offset = fire_effect_offset(offset, rotation);
		fe.update_transform = r.flag_or(*s, "UpdateTransform", false);
		fe.disable_during_zoom = r.flag_or(*s, "DisableDuringZoom", false);
		fe.shell = blueprint.find("/shells/") != std::string::npos;
		r_out.push_back(std::move(fe));
	}
}

// The projectile entity of a FiringFunctionData: Shot.ProjectileData.
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
	// A firearm: a bullet def with a muzzle effect (the knife is a BulletEntityData without one; grenades throw).
	r_w.firearm = p_def.projectile.kind == gp::ProjectileKind::Bullet && !(r_w.first_person.empty() && r_w.third_person.empty());
}

// ---- triggers --------------------------------------------------------------------------------------------

// The start key families (with the event kind, slot, id, pellet and ordinal).
enum class KeyKind : uint8_t { Shot = 1, Impact = 2, Soldier = 3, SoldierHit = 4, Detonation = 5 };

uint64_t start_key(KeyKind p_kind, uint8_t p_slot, uint32_t p_id, uint8_t p_pellet, uint32_t p_ordinal) {
	uint64_t k = fx_mix64((uint64_t(uint8_t(p_kind)) << 56) ^ (uint64_t(p_slot) << 48) ^ (uint64_t(p_pellet) << 40) ^ uint64_t(p_id));
	return fx_mix64(k ^ uint64_t(p_ordinal));
}

uint64_t shot_key(uint8_t p_slot, uint32_t p_id) {
	return (uint64_t(p_slot) << 32) | uint64_t(p_id);
}

uint64_t pack(EffectHandle p_h) {
	return (uint64_t(p_h.slot) << 32) | uint64_t(p_h.generation);
}

} // namespace

// ---- free functions -----------------------------------------------------------------------------------------

Transform effect_frame(const Vec3 &p_origin, const Vec3 &p_axis, const Vec3 &p_forward_hint) {
	const Vec3 u = normalize_or(p_axis, { 0, 1, 0 });
	Vec3 f = finite3(p_forward_hint) ? sub(p_forward_hint, mul(u, dot(p_forward_hint, u))) : Vec3{};
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
	w.right = cross(w.up, w.forward); // x = y x z
	w.trans = p_effect.trans;
	return w;
}

MuzzleSample aim_muzzle(uint8_t p_slot, const Vec3 &p_eye, const Vec3 &p_direction, const Vec3 &p_velocity) {
	MuzzleSample m;
	m.slot = p_slot;
	if (!finite3(p_eye)) {
		return m;
	}
	const Vec3 d = normalize_or(p_direction, { 0, 0, 1 });
	const Vec3 right = normalize_or(cross(d, Vec3{ 0, 1, 0 }), { 1, 0, 0 });
	const Vec3 up = normalize_or(cross(right, d), { 0, 1, 0 });
	const Vec3 pos = add(add(add(p_eye, mul(right, kFallbackMuzzleRight)), mul(up, kFallbackMuzzleUp)), mul(d, kFallbackMuzzleForward));
	m.valid = true;
	m.transform = effect_frame(pos, d, up);
	m.velocity = finite3(p_velocity) ? p_velocity : Vec3{};
	return m;
}

MuzzleSample fallback_muzzle(const host::PresentedSoldier &p_soldier) {
	const float cp = std::cos(p_soldier.pitch);
	const Vec3 dir{ std::sin(p_soldier.yaw) * cp, std::sin(p_soldier.pitch), std::cos(p_soldier.yaw) * cp };
	const Vec3 eye{ p_soldier.position[0], p_soldier.position[1] + p_soldier.eye_height, p_soldier.position[2] };
	return aim_muzzle(p_soldier.slot, eye, dir, { p_soldier.velocity[0], p_soldier.velocity[1], p_soldier.velocity[2] });
}

Transform fire_effect_offset(const Vec3 &p_offset, const Vec3 &p_rotation_degrees) {
	Transform t;
	t.trans = finite3(p_offset) ? p_offset : Vec3{};
	if (!finite3(p_rotation_degrees) || (p_rotation_degrees.x == 0 && p_rotation_degrees.y == 0 && p_rotation_degrees.z == 0)) {
		return t;
	}
	const float d2r = kPi / 180.0f;
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
		*v = rot(rot(rot(*v, 0, p_rotation_degrees.x * d2r), 1, p_rotation_degrees.y * d2r), 2, p_rotation_degrees.z * d2r);
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

// ---- FxTriggers -----------------------------------------------------------------------------------------------

struct FxTriggers::Impl {
	FxWorld &world;
	EffectLibrary &library;
	std::shared_ptr<const FireEffectTable> fire;
	FxTriggerConfig config;
	FxTriggerStats stats;

	std::vector<MuzzleSample> muzzles;
	std::array<int32_t, 256> own_1p{}; // slot -> index of its 1p muzzle (-1 none)
	std::array<int32_t, 256> third{}; // slot -> index of its 3p muzzle

	std::vector<DecalCue> decals;
	std::vector<ImpactSoundCue> sounds;
	std::vector<TriggerStart> starts;

	std::deque<hk::PresentEvent> pending;

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
		hk::PresentTimeline timeline = hk::PresentTimeline::Predicted;
		int64_t t0 = 0;
		Vec3 muzzle; // where it leaves the barrel
		Vec3 origin; // the aim ray
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

	// True the first time; false (counted) for a key that already ran.
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

	// One FxWorld start; logs it. Returns the handle (invalid when culled or refused).
	EffectHandle start(const std::shared_ptr<const EffectDef> &p_def, const hk::PresentEvent &p_e, TriggerPurpose p_purpose, uint32_t p_index, uint64_t p_seed,
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

	// ---- the timelines ----
	// The start age of p_e now; false while it waits (invalid clock or ahead of the drawn time).
	bool age_of(const hk::PresentEvent &p_e, const hk::PresentClock &p_now, float &r_age) const {
		if (p_e.timeline == hk::PresentTimeline::Immediate) {
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

	// ---- shots ----
	const MuzzleSample *muzzle_for(uint8_t p_slot, bool p_first_person) const {
		const int32_t i = p_first_person ? own_1p[p_slot] : third[p_slot];
		return i >= 0 ? &muzzles[size_t(i)] : nullptr;
	}

	// The frame of entry p_i of p_list given the person's muzzle sample at entry 0.
	static Transform entry_frame(const std::vector<FireEffect> &p_list, size_t p_i, const MuzzleSample &p_m) {
		if (p_i == 0) {
			return p_m.transform;
		}
		const Transform w_ref = weapon_of_effect_frame(p_m.transform);
		const Transform w_root = compose(w_ref, inverse(p_list[0].offset));
		return effect_frame_of_weapon(compose(w_root, p_list[p_i].offset));
	}

	void on_shot(const hk::PresentEvent &p_e, float p_age) {
		const bool own = p_e.kind == hk::PresentKind::OwnShot;
		++(own ? stats.own_shots : stats.remote_shots);
		const WeaponFireEffects *w = def_of(p_e.def);
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
		const std::vector<FireEffect> &list = own ? w->first_person : w->third_person;
		MuzzleSample m;
		if (const MuzzleSample *s = muzzle_for(p_e.slot, own)) {
			m = *s;
		} else if (const MuzzleSample *s3 = own ? muzzle_for(p_e.slot, false) : nullptr) {
			m = *s3; // the own soldier drawn in 3p (no viewmodel): its crowd muzzle
		} else {
			m = aim_muzzle(p_e.slot, origin, dir, {});
			++stats.muzzle_fallbacks;
		}
		const bool first_person = own && m.first_person;
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
				attached.push_back(Attached{ h, p_e.slot, first_person, p_e.def, uint32_t(i) });
			}
		}
		if ((p_e.flags & hk::present_flags::kTracer) != 0 && !w->tracer.empty()) {
			++stats.tracer_shots;
			if (config.tracers) {
				start_tracer(*w, p_e, m.transform.trans, origin, dir, p_age);
			}
		}
	}

	void start_tracer(const WeaponFireEffects &p_w, const hk::PresentEvent &p_e, const Vec3 &p_muzzle, const Vec3 &p_origin, const Vec3 &p_dir, float p_age) {
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

	// The tracer after p_s metres: on the aim ray, plus the muzzle's offset fading out over the converge distance.
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

	void step_tracers(const hk::PresentClock &p_now) {
		size_t keep = 0;
		for (size_t i = 0; i < tracers.size(); ++i) {
			Tracer &t = tracers[i];
			bool live = world.alive(t.handle);
			if (live && (t.timeline == hk::PresentTimeline::Immediate || p_now.valid(t.timeline))) {
				const int64_t now = t.timeline == hk::PresentTimeline::Immediate ? t.t0 : p_now.now(t.timeline);
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

	// ---- impacts ----
	// Starts p_cell at the event; p_axis_seed: the event seed of the choice, spread and decal.
	void play_cell(const ImpactCell &p_cell, const hk::PresentEvent &p_e, uint64_t p_seed, const Vec3 &p_pos, const Vec3 &p_normal, float p_age, bool p_decal,
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

	void on_impact(const hk::PresentEvent &p_e, float p_age) {
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

	void on_soldier(const hk::PresentEvent &p_e, float p_age) {
		const Vec3 pos = from(p_e.pos);
		const bool contact = p_e.kind == hk::PresentKind::SoldierContact;
		if (contact) {
			end_tracer_at(p_e.slot, p_e.id, pos);
		}
		// Duplicate deliveries of the same event kind are double starts; a Hit after the contact is the confirmation.
		const uint64_t own_key = start_key(contact ? KeyKind::Soldier : KeyKind::SoldierHit, p_e.slot, p_e.id, p_e.pellet, contact ? p_e.aux : 0u);
		if (!first_time(own_key)) {
			return;
		}
		if (!contact && (p_e.flags & hk::present_flags::kOwnVictim) != 0) {
			return;
		}
		if (contact && p_e.slot != config.own_slot) {
			return; // a remote shot's predicted contact: its Hit plays the cell (§12.2: SoldierContact (own))
		}
		const uint64_t cell_key = start_key(KeyKind::Soldier, p_e.slot, p_e.id, p_e.pellet, kSoldierOrdinal);
		if (!first_time(cell_key, false)) {
			++stats.hits_confirmed;
			return;
		}
		if (!config.impacts) {
			return;
		}
		const uint32_t zone = contact ? uint32_t(std::clamp(p_e.value, 0.0f, 3.0f)) : p_e.material;
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

	void on_detonation(const hk::PresentEvent &p_e, float p_age) {
		if (!first_time(start_key(KeyKind::Detonation, p_e.slot, p_e.id, 0, 0))) {
			return;
		}
		if ((p_e.flags & hk::present_flags::kDud) != 0) {
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

	bool sane(const hk::PresentEvent &p_e) const {
		for (size_t k = 0; k < 3; ++k) {
			if (!std::isfinite(p_e.pos[k]) || !std::isfinite(p_e.dir[k]) || std::fabs(p_e.pos[k]) > 1.0e7f) {
				return false;
			}
		}
		return uint8_t(p_e.kind) <= uint8_t(hk::PresentKind::Spawn) && uint8_t(p_e.timeline) <= uint8_t(hk::PresentTimeline::Immediate);
	}

	void dispatch(const hk::PresentEvent &p_e, float p_age) {
		switch (p_e.kind) {
			case hk::PresentKind::OwnShot:
			case hk::PresentKind::RemoteShot:
				on_shot(p_e, p_age);
				break;
			case hk::PresentKind::Impact:
				on_impact(p_e, p_age);
				break;
			case hk::PresentKind::SoldierContact:
			case hk::PresentKind::Hit:
				on_soldier(p_e, p_age);
				break;
			case hk::PresentKind::Detonation:
				on_detonation(p_e, p_age);
				break;
			default:
				break; // OwnTimed, Death, Spawn: no effect in M2a
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

void FxTriggers::on_muzzles(std::span<const MuzzleSample> p_muzzles) {
	Impl &m = *impl_;
	m.muzzles.assign(p_muzzles.begin(), p_muzzles.end());
	m.own_1p.fill(-1);
	m.third.fill(-1);
	for (size_t i = 0; i < m.muzzles.size(); ++i) {
		const MuzzleSample &s = m.muzzles[i];
		if (!s.valid || !finite3(s.transform.trans) || !finite3(s.transform.up) || !finite3(s.transform.forward) || !finite3(s.transform.right)) {
			continue;
		}
		if (!finite3(s.velocity)) {
			m.muzzles[i].velocity = {};
		}
		(s.first_person ? m.own_1p : m.third)[s.slot] = int32_t(i);
	}
	m.update_attached();
}

void FxTriggers::on_events(std::span<const host::PresentEvent> p_events, const host::PresentClock &p_now, const FxView &p_view) {
	Impl &m = *impl_;
	m.decals.clear();
	m.sounds.clear();
	m.starts.clear();
	m.view = p_view;
	if (!finite3(m.view.camera_position)) {
		m.view.camera_position = {};
	}
	// The held events first (they are older), then this frame's.
	const size_t held = m.pending.size();
	for (size_t i = 0; i < held; ++i) {
		const host::PresentEvent e = m.pending.front();
		m.pending.pop_front();
		float age = 0;
		if (m.age_of(e, p_now, age)) {
			m.dispatch(e, age);
		} else {
			m.pending.push_back(e);
		}
	}
	for (const host::PresentEvent &e : p_events) {
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

} // namespace bf3::fx

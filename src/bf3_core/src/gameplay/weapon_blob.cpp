#include "bf3/core/gameplay/weapon_blob.h"

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/ebx.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/superbundle_mount.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <type_traits>

namespace bf3::core::gameplay {

namespace {

constexpr const char *kWeaponMagic = "OBF3WDEF";
constexpr const char *kDepsMagic = "OBF3WDEP";
constexpr size_t kMaxString = 64 * 1024;
constexpr size_t kMaxDeps = 1u << 16;

struct Writer {
	BlobWriter w;
	void v(float &p_v) { w.f32(p_v); }
	void v(bool &p_v) { w.boolean(p_v); }
	void v(int32_t &p_v) { w.i32(p_v); }
	void v(uint32_t &p_v) { w.u32(p_v); }
	void v(uint8_t &p_v) { w.u8(p_v); }
	void v(uint64_t &p_v) { w.u64(p_v); }
	void v(std::string &p_v) { w.str(p_v); }
	void v(Guid &p_v) { w.raw(p_v.bytes.data(), p_v.bytes.size()); }
	template <class E>
		requires std::is_enum_v<E>
	void v(E &p_v) {
		w.u8(static_cast<uint8_t>(p_v));
	}

	template <class T, class F>
	void vec(std::vector<T> &p_v, size_t, F &&p_each) {
		w.u64(p_v.size());
		for (T &e : p_v) {
			p_each(*this, e);
		}
	}
	bool ok() const { return true; }
};

struct Reader {
	explicit Reader(std::span<const uint8_t> p_bytes) :
			r(p_bytes) {}
	BlobReader r;
	void v(float &p_v) { p_v = r.f32(); }
	void v(bool &p_v) { p_v = r.boolean(); }
	void v(int32_t &p_v) { p_v = r.i32(); }
	void v(uint32_t &p_v) { p_v = r.u32(); }
	void v(uint8_t &p_v) { p_v = r.u8(); }
	void v(uint64_t &p_v) { p_v = r.u64(); }
	void v(std::string &p_v) { p_v = r.str(kMaxString); }
	void v(Guid &p_v) {
		for (uint8_t &b : p_v.bytes) {
			b = r.u8();
		}
	}
	template <class E>
		requires std::is_enum_v<E>
	void v(E &p_v) {
		p_v = static_cast<E>(r.u8());
	}

	template <class T, class F>
	void vec(std::vector<T> &p_v, size_t p_min_bytes, F &&p_each) {
		const size_t n = r.count(p_min_bytes);
		p_v.clear();
		p_v.resize(n);
		for (T &e : p_v) {
			if (!r.ok()) {
				break;
			}
			p_each(*this, e);
		}
	}
	bool ok() const { return r.ok(); }
};

template <class IO, class T, size_t N>
void arr(IO &io, std::array<T, N> &p_a) {
	for (T &e : p_a) {
		io.v(e);
	}
}

template <class IO> void arr(IO &io, Vec3 &p_v) {
	for (float &e : p_v) {
		io.v(e);
	}
}

template <class IO>
void io_dispersion(IO &io, Dispersion &d) {
	io.v(d.min_angle);
	io.v(d.max_angle);
	io.v(d.increase_per_shot);
	io.v(d.decrease_per_second);
}

template <class IO>
void io_recoil(IO &io, Recoil &r) {
	io.v(r.up_per_shot);
	io.v(r.up_max);
	io.v(r.horizontal_min);
	io.v(r.horizontal_max);
	io.v(r.horizontal_amplitude_max);
	io.v(r.decrease_factor);
}

template <class IO>
void io_sway_modifier(IO &io, SwayModifier &m) {
	io.v(m.min_angle);
	io.v(m.max_angle);
	io.v(m.increase_per_shot);
	io.v(m.decrease_per_second);
	io.v(m.recoil_magnitude);
	io.v(m.recoil_angle);
	io.v(m.lag_yaw);
	io.v(m.lag_pitch);
}

template <class IO>
void io_sway_state(IO &io, SwayState &s) {
	for (Dispersion &d : s.move) {
		io_dispersion(io, d);
	}
	arr(io, s.has_move);
	io_recoil(io, s.recoil);
}

template <class IO>
void io_gated(IO &io, SwayGatedModifier &g) {
	io.v(g.unlock);
	io.v(g.unlock_instance);
	io.v(g.only_in_supported_shooting);
	io.v(g.only_on_weapon_light_enabled);
	for (auto &stance : g.mod) {
		for (SwayModifier &m : stance) {
			io_sway_modifier(io, m);
		}
	}
}

template <class IO>
void io_sway(IO &io, GunSway &s) {
	io.v(s.partition);
	for (auto &stance : s.state) {
		for (SwayState &z : stance) {
			io_sway_state(io, z);
		}
	}
	for (GunSway::Transition &t : s.transitions) {
		io_dispersion(io, t.max_penalty);
		io.v(t.cool_down);
	}
	for (SwayModifier &m : s.suppression) {
		io_sway_modifier(io, m);
	}
	io.v(s.first_shot_recoil_multiplier);
	io.v(s.shooting_recoil_decrease_scale);
	arr(io, s.deviation_scale);
	arr(io, s.gameplay_deviation_scale);
	io.v(s.camera_spring_constant);
	io.v(s.camera_spring_damping);
	io.v(s.camera_spring_min_threshold_angle);
	constexpr size_t kGatedMinBytes = 4 + 16 + 2 + 6 * 8 * 4;
	io.vec(s.applied, kGatedMinBytes, [](IO &p_io, SwayGatedModifier &p_g) { io_gated(p_io, p_g); });
	io.vec(s.conditional, kGatedMinBytes, [](IO &p_io, SwayGatedModifier &p_g) { io_gated(p_io, p_g); });
	uint64_t total = s.modifiers_total, unresolved = s.modifiers_unresolved;
	io.v(total);
	io.v(unresolved);
	s.modifiers_total = static_cast<size_t>(total);
	s.modifiers_unresolved = static_cast<size_t>(unresolved);
}

template <class IO>
void io_fire(IO &io, FireFunction &f) {
	io.v(f.logic);
	io.vec(f.extra_modes, 1, [](IO &p_io, FireLogic &p_l) { p_io.v(p_l); });
	io.v(f.rate_of_fire);
	io.v(f.rate_of_fire_for_burst);
	io.v(f.client_fire_rate_multiplier);
	io.v(f.pre_fire_delay);
	io.v(f.automatic_delay);
	io.v(f.trigger_pull_weight);
	io.v(f.bolt_action_delay);
	io.v(f.bolt_action_time);
	io.v(f.unzoom_on_bolt_action);
	io.v(f.return_to_zoom_after_bolt_action);
	io.v(f.hold_bolt_action_until_fire_release);
	io.v(f.hold_bolt_action_until_zoom_release);
	io.v(f.force_bolt_action_on_fire_trigger);
	io.v(f.hold_delay);
	io.v(f.hold_max_time);
	io.v(f.hold_min_power);
	io.v(f.hold_max_power);
	io.v(f.hold_power_per_second);
	io.v(f.hold_killed_power_modifier);
	io.v(f.hold_force_fire_when_killed);
	io.v(f.reload_type);
	io.v(f.reload_time);
	io.v(f.reload_time_bullets_left);
	io.v(f.reload_delay);
	io.v(f.reload_threshold);
	io.v(f.always_auto_reload);
	io.v(f.reload_logic);
	io.v(f.magazine_capacity);
	io.v(f.number_of_magazines);
	io.v(f.trace_frequency);
	io.v(f.auto_replenish_delay);
	arr(io, f.initial_position);
	arr(io, f.initial_direction);
	arr(io, f.initial_speed);
	io.v(f.bullets_per_shell);
	io.v(f.bullets_per_shot);
	io.v(f.bullets_per_burst);
	io.v(f.spawn_delay);
	io.v(f.inherit_weapon_speed);
	io.v(f.active_force_spawn_to_camera);
	io.v(f.force_spawn_to_camera);
	io.v(f.heat_per_bullet);
	io.v(f.heat_drop_per_second);
	io.v(f.overheat_penalty_time);
	io.v(f.overheat_threshold);
	for (Dispersion &d : f.weapon_dispersion) {
		io_dispersion(io, d);
	}
	io.v(f.jump_dispersion_angle);
	io.v(f.move_dispersion_angle);
	io.v(f.move_zoomed_dispersion_angle);
	io.v(f.weapon_dispersion_decrease_per_second);
	uint64_t dispersions = f.firing_dispersions;
	io.v(dispersions);
	f.firing_dispersions = static_cast<size_t>(dispersions);
	io.v(f.fire_logic_recoil_nonzero);

	io.v(f.hold_off_reload_until_fire_release);
	io.v(f.force_reload_action_on_fire_trigger);
}

template <class IO>
void io_explosion(IO &io, Explosion &e) {
	io.v(e.present);
	io.v(e.partition);
	io.v(e.blast_damage);
	io.v(e.blast_radius);
	io.v(e.inner_blast_radius);
	io.v(e.blast_impulse);
	io.v(e.shockwave_damage);
	io.v(e.shockwave_radius);
	io.v(e.shockwave_impulse);
	io.v(e.shockwave_time);
	io.v(e.max_occlusion_raycast_radius);
	io.v(e.disable_occlusion);
	io.v(e.disable_static_entity_occlusion);
	io.v(e.is_causing_suppression);

	io.v(e.material.present);
	io.v(e.material.resolved);
	io.v(e.material.partition);
	io.v(e.material.instance);
	io.v(e.material.flags_and_index);
	io.v(e.material.physics_material_index);
	io.v(e.material.physics_property_index);
}

template <class IO>
void io_projectile(IO &io, ProjectileDef &p) {
	io.v(p.kind);
	io.v(p.type);
	io.v(p.partition);
	io.v(p.ammunition_type);
	io.v(p.gravity);
	io.v(p.time_to_live);
	io.v(p.record_initial_speed);
	io.v(p.visual_converge_distance);
	io.v(p.impact_impulse);
	io.v(p.detonate_on_timeout);
	io.v(p.server_projectile_disabled);
	io.v(p.start_damage);
	io.v(p.end_damage);
	io.v(p.damage_falloff_start);
	io.v(p.damage_falloff_end);
	io.v(p.time_to_arm_explosion);
	io.v(p.distribute_damage_over_time);
	io.v(p.instant_hit);
	io.v(p.missile_damage);
	io.v(p.engine_strength);
	io.v(p.max_speed);
	io.v(p.engine_time_to_ignition);
	io.v(p.engine_time_to_live);
	io.v(p.time_to_arm);
	io.v(p.drag);
	io.v(p.max_turn_angle);
	io.v(p.time_to_activate_guiding);
	io.v(p.collision_speed_multiplier);
	io.v(p.min_bounce_speed);
	io.v(p.grenade_type);
	io.v(p.material.present);
	io.v(p.material.resolved);
	io.v(p.material.partition);
	io.v(p.material.instance);
	io.v(p.material.flags_and_index);
	io.v(p.material.physics_material_index);
	io.v(p.material.physics_property_index);
	io_explosion(io, p.explosion);
	io_explosion(io, p.dud_explosion);
	io.v(p.suppression.present);
	io.v(p.suppression.min_multiplier);
	io.v(p.suppression.max_multiplier);
	io.v(p.suppression.min_distance);
	io.v(p.suppression.max_distance);
}

template <class IO>
void io_def(IO &io, WeaponDef &d) {
	io.v(d.variant);
	io.v(d.unlock_identifier);
	io.v(d.blueprint);
	io.v(d.blueprint_name);
	io.v(d.damage_giver_name);
	io.v(d.name_sid);
	io.v(d.name_sid_hash);
	io.vec(d.active_unlocks, 4, [](IO &p_io, std::string &p_s) { p_io.v(p_s); });
	io.vec(d.applied_modifiers, 4, [](IO &p_io, std::string &p_s) { p_io.v(p_s); });
	io.v(d.weapon_class);
	io.v(d.anim_base_set);
	io.v(d.weapon_anim_type);
	io.v(d.aiming_controller);
	io.v(d.sound);
	io.v(d.render_fov);
	io.v(d.zoom_render_fov);
	io.v(d.silenced);
	io.v(d.enable_breath_control);
	io.v(d.can_be_in_supported_shooting);
	io.v(d.firing_data);
	io.v(d.deploy_time);
	io.v(d.alt_deploy_time);
	io.v(d.disable_zoom_on_deploy_time);
	io.v(d.support_delay_stand);
	io.v(d.support_delay_prone);
	io.v(d.reload_whole_mags);
	io_fire(io, d.fire);
	io_projectile(io, d.projectile);
	io_sway(io, d.sway);
}

class RecordingResolver final : public EbxResolver {
public:
	RecordingResolver(const EbxResolver &p_inner, uint8_t p_tag, std::set<WeaponDependency> &r_deps) :
			inner_(p_inner), tag_(p_tag), deps_(r_deps) {}

	const EbxPartition *partition(const Guid &p_guid) const override {
		const EbxPartition *p = inner_.partition(p_guid);
		if (p != nullptr) {
			if (const std::optional<std::string_view> name = inner_.name_of(p_guid)) {
				deps_.insert(WeaponDependency{ tag_, to_lower_ascii(*name) });
			}
		}
		return p;
	}
	const EbxPartition *partition_by_name(std::string_view p_name) const override {
		deps_.insert(WeaponDependency{ tag_, to_lower_ascii(p_name) });
		return inner_.partition_by_name(p_name);
	}
	std::optional<std::string_view> name_of(const Guid &p_guid) const override {
		const std::optional<std::string_view> name = inner_.name_of(p_guid);
		if (name) {
			deps_.insert(WeaponDependency{ tag_, to_lower_ascii(*name) });
		}
		return name;
	}

private:
	const EbxResolver &inner_;
	uint8_t tag_;
	std::set<WeaponDependency> &deps_;
};

std::string variant_key_name(std::string_view p_variant) {
	return to_lower_ascii(p_variant);
}

}

std::vector<uint8_t> encode_weapon_def(const WeaponDef &p_def) {
	Writer io;
	io.w.magic(kWeaponMagic, kWeaponBlobFormatVersion);
	io_def(io, const_cast<WeaponDef &>(p_def));
	return io.w.take();
}

Result<WeaponDef> decode_weapon_def(std::span<const uint8_t> p_blob) {
	Reader io(p_blob);
	const uint32_t version = io.r.magic(kWeaponMagic);
	if (io.ok() && version != kWeaponBlobFormatVersion) {
		return make_error("weapon blob: format version " + std::to_string(version) + ", expected " + std::to_string(kWeaponBlobFormatVersion));
	}
	WeaponDef def;
	io_def(io, def);
	if (io.ok() && !io.r.at_end()) {
		io.r.fail(std::to_string(io.r.remaining()) + " trailing bytes");
	}
	if (!io.ok()) {
		return io.r.error("weapon blob");
	}
	return def;
}

PartitionDigest mount_partition_digest(const LevelMount &p_mount) {
	return [&p_mount](std::string_view p_name) { return p_mount.entry_sha1(EntryRef::ebx(p_name)); };
}

PartitionDigest library_partition_digest(const EbxLibrary &p_library) {
	return [&p_library](std::string_view p_name) -> std::optional<Sha1> {
		const std::optional<EbxLibrary::Origin> origin = p_library.origin(p_name);
		if (!origin) {
			return std::nullopt;
		}
		for (size_t i = 0; i < p_library.superbundle_count(); ++i) {
			if (p_library.superbundle(i) == origin->superbundle) {
				return p_library.mount(i).entry_sha1(EntryRef::ebx(p_name));
			}
		}
		return std::nullopt;
	};
}

Result<WeaponDef> resolve_weapon_recorded(const WeaponCacheSources &p_sources, std::string_view p_variant, std::vector<WeaponDependency> &r_deps) {
	r_deps.clear();
	if (p_sources.level == nullptr) {
		return make_error("weapon " + std::string(p_variant) + ": no level resolver");
	}
	std::set<WeaponDependency> deps;
	const RecordingResolver level(*p_sources.level, 0, deps);
	std::optional<RecordingResolver> ui;
	WeaponResolveOptions options;
	if (p_sources.ui != nullptr) {
		ui.emplace(*p_sources.ui, uint8_t(1), deps);
		options.ui = &*ui;
	}
	Result<WeaponDef> def = resolve_weapon(level, p_variant, options);
	r_deps.assign(deps.begin(), deps.end());
	return def;
}

std::vector<uint8_t> encode_weapon_deps(const std::vector<WeaponDependency> &p_deps) {
	BlobWriter w;
	w.magic(kDepsMagic, kWeaponBlobFormatVersion);
	w.u64(p_deps.size());
	for (const WeaponDependency &d : p_deps) {
		w.u8(d.resolver);
		w.str(d.name);
	}
	return w.take();
}

Result<std::vector<WeaponDependency>> decode_weapon_deps(std::span<const uint8_t> p_blob) {
	BlobReader r(p_blob);
	const uint32_t version = r.magic(kDepsMagic);
	if (r.ok() && version != kWeaponBlobFormatVersion) {
		return make_error("weapon deps blob: format version " + std::to_string(version));
	}
	const size_t n = r.count(5, kMaxDeps);
	std::vector<WeaponDependency> out;
	out.reserve(n);
	for (size_t i = 0; i < n && r.ok(); ++i) {
		WeaponDependency d;
		d.resolver = r.u8();
		if (d.resolver > 1) {
			r.fail("resolver tag " + std::to_string(d.resolver));
		}
		d.name = r.str(kMaxString);
		if (r.ok() && !out.empty() && !(out.back() < d)) {
			r.fail("dependencies not sorted and unique");
		}
		out.push_back(std::move(d));
	}
	if (r.ok() && !r.at_end()) {
		r.fail("trailing bytes");
	}
	if (!r.ok()) {
		return r.error("weapon deps blob");
	}
	return out;
}

std::optional<CacheKey> weapon_deps_key(const WeaponCacheSources &p_sources, std::string_view p_variant) {
	if (!p_sources.level_digest) {
		return std::nullopt;
	}
	const std::optional<Sha1> root = p_sources.level_digest(p_variant);
	if (!root) {
		return std::nullopt;
	}
	CacheKeyInputs in;
	in.kind = kWeaponDepsCacheKind;
	in.sources.push_back(source_digest(*root));
	in.converter_version = kWeaponConverterVersion;
	in.engine = std::string(cache_engine_id());
	in.format_version = kWeaponBlobFormatVersion;
	in.params = "variant=" + variant_key_name(p_variant) + ";ui=" + (p_sources.ui != nullptr ? "1" : "0");
	return make_cache_key(in);
}

std::optional<CacheKey> weapon_def_key(const WeaponCacheSources &p_sources, std::string_view p_variant, const std::vector<WeaponDependency> &p_deps) {
	CacheKeyInputs in;
	in.kind = kWeaponCacheKind;
	for (const WeaponDependency &d : p_deps) {
		const PartitionDigest &digest = d.resolver == 0 ? p_sources.level_digest : p_sources.ui_digest;
		if (!digest) {
			return std::nullopt;
		}
		const std::optional<Sha1> sha1 = digest(d.name);
		in.sources.push_back(sha1 ? source_digest(*sha1) : Sha1Digest{});
	}
	in.converter_version = kWeaponConverterVersion;
	in.engine = std::string(cache_engine_id());
	in.format_version = kWeaponBlobFormatVersion;
	in.params = "variant=" + variant_key_name(p_variant) + ";deps=" + sha256_of(encode_weapon_deps(p_deps)).hex();
	return make_cache_key(in);
}

Result<CachedWeapon> resolve_weapon_cached(Cache *p_cache, const WeaponCacheSources &p_sources, std::string_view p_variant) {
	CachedWeapon out;
	const bool use_cache = p_cache != nullptr && p_cache->is_open();
	const std::optional<CacheKey> deps_key = use_cache ? weapon_deps_key(p_sources, p_variant) : std::nullopt;
	std::optional<std::vector<WeaponDependency>> cached_deps;
	bool deps_dropped = false;
	if (deps_key) {
		GetResult got = p_cache->get(*deps_key);
		if (got.status == CacheStatus::Ok) {
			Result<std::vector<WeaponDependency>> d = decode_weapon_deps(got.entry.payload());
			if (d) {
				cached_deps = std::move(*d);
			} else {
				got.entry.reset();
				p_cache->remove(*deps_key);
				deps_dropped = true;
			}
		} else if (got.dropped_corrupt) {
			deps_dropped = true;
		}
	}
	if (cached_deps) {
		if (const std::optional<CacheKey> key = weapon_def_key(p_sources, p_variant, *cached_deps)) {
			GetResult got = p_cache->get(*key);
			if (got.status == CacheStatus::Ok) {
				Result<WeaponDef> def = decode_weapon_def(got.entry.payload());
				if (def) {
					out.def = std::move(*def);
					out.item.outcome = CacheOutcome::Hit;
					out.item.payload_bytes = got.entry.payload().size();
					out.dependencies = cached_deps->size();
					return out;
				}
				got.entry.reset();
				p_cache->remove(*key);
				out.item.dropped_corrupt = true;
			} else if (got.dropped_corrupt) {
				out.item.dropped_corrupt = true;
			}
		}
	}

	std::vector<WeaponDependency> deps;
	Result<WeaponDef> def = resolve_weapon_recorded(p_sources, p_variant, deps);
	if (!def) {
		return def.error();
	}
	out.dependencies = deps.size();
	const std::vector<uint8_t> blob = encode_weapon_def(*def);
	out.item.payload_bytes = blob.size();
	out.item.outcome = CacheOutcome::Uncached;
	out.item.dropped_corrupt = out.item.dropped_corrupt || deps_dropped;
	out.def = std::move(*def);
	if (!use_cache) {
		return out;
	}
	const std::optional<CacheKey> key = weapon_def_key(p_sources, p_variant, deps);
	if (!deps_key || !key) {
		out.item.cache_error = "no manifest SHA1 for a source partition";
		return out;
	}
	if (cached_deps && *cached_deps != deps) {
		p_cache->remove(*deps_key);
	}
	if (!cached_deps || *cached_deps != deps) {
		const PutResult put = p_cache->put(*deps_key, encode_weapon_deps(deps));
		if (put.status != CacheStatus::Ok) {
			out.item.cache_error = std::string("put deps: ") + cache_status_name(put.status);
			return out;
		}
	}
	const PutResult put = p_cache->put(*key, blob);
	if (put.status == CacheStatus::Ok) {
		out.item.outcome = put.written ? CacheOutcome::Converted : CacheOutcome::Reused;
	} else {
		out.item.cache_error = std::string("put: ") + cache_status_name(put.status);
	}
	return out;
}

std::vector<std::string> weapon_variants_of(const EbxDb &p_db) {
	std::vector<std::string> out;
	for (size_t i = 0; i < p_db.size(); ++i) {
		const std::string &name = p_db.name_at(i);
		if (name.starts_with("weapons/") && p_db.partition_at(i).primary().is_a("SoldierWeaponUnlockAsset")) {
			out.push_back(name);
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

}

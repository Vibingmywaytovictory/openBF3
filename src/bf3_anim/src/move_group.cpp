#include "bf3/anim/move_group.h"

#include "bf3/core/gameplay/work_budget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bf3::anim {

using core::ant::AntLibrary;
using core::ant::AntPackage;
using core::ant::GdArray;
using core::ant::GdGuid;
using core::ant::GdStruct;

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr uint32_t kMaxChooserEntries = 64;
constexpr uint32_t kMaxSignals = 16;

constexpr uint32_t kAntRefHigh = 0x15480000u;

std::string hex(uint32_t p_v) {
	char b[16];
	std::snprintf(b, sizeof(b), "0x%x", p_v);
	return b;
}

std::string label(const GdStruct &p_s) {
	return std::string(p_s.type_name()) + " '" + std::string(p_s.name()) + "'";
}

bool finite(float p_v) {
	return std::isfinite(p_v);
}

core::Result<GdStruct> find_static(const AntLibrary &p_library, std::string_view p_type, std::string_view p_name, core::gameplay::WorkBudget &r_budget) {
	const AntPackage *stat = p_library.static_package();
	if (stat == nullptr) {
		return core::make_error("the Ant library has no static package");
	}
	GdStruct found;
	uint32_t hits = 0;
	for (size_t i = 0; i < stat->object_count(); ++i) {
		if (core::Status s = r_budget.charge(1); !s.ok()) {
			return s.error();
		}
		const GdStruct o = stat->object(i);
		if (o.name() == p_name && o.type_name() == p_type) {
			++hits;
			found = o;
		}
	}
	if (hits != 1) {
		return core::make_error(std::string(p_type) + " '" + std::string(p_name) + "': " + std::to_string(hits) + " objects of that name (must be exactly one)");
	}
	return found;
}

bool signal_default(const AntLibrary &p_library, const AntPackage &p_from, const GdGuid &p_signal, bool &r_value) {
	const GdStruct s = p_library.resolve(p_from, p_signal);
	if (!s.valid() || s.type_name() != "BoolAsset") {
		return false;
	}
	return s.read_bool("Default", r_value);
}

core::Result<AntClipRoute> route_from(const AntLibrary &p_library, const AntPackage &p_from, GdStruct p_start, const ChooserIndices &p_indices,
		core::gameplay::WorkBudget &r_budget) {
	AntClipRoute route;
	GdStruct cur = p_start;
	const AntPackage *pkg = &p_from;
	auto follow = [&](const GdGuid &p_g, const char *p_what) -> core::Status {
		if (p_g.is_null()) {
			return core::make_error(label(cur) + ": empty " + p_what);
		}
		const GdStruct next = p_library.resolve(*pkg, p_g);
		if (!next.valid()) {
			return core::make_error(label(cur) + ": " + p_what + " " + hex(p_g.index()) + " does not resolve");
		}
		pkg = &p_library.package_of(*pkg, p_g);
		cur = next;
		return core::Status::success();
	};
	for (uint32_t depth = 0;; ++depth) {
		if (!cur.valid()) {
			return core::make_error("controller route: an invalid object");
		}
		if (depth >= kMaxAntRouteDepth) {
			return core::make_error(label(p_start) + ": more than " + std::to_string(kMaxAntRouteDepth) + " hops to a clip controller");
		}
		if (core::Status s = r_budget.charge(1); !s.ok()) {
			return s.error();
		}
		route.path.push_back(label(cur));
		const std::string_view type = cur.type_name();
		if (type == "ClipControllerAsset") {
			GdGuid anim;
			if (!cur.read_guid("Anim", anim) || anim.is_null()) {
				return core::make_error(label(cur) + ": no Anim");
			}
			float ticks_f = 0.0f, fps = 0.0f, scale = 0.0f;
			if (!cur.read_float("NumTicks", ticks_f) || !cur.read_float("FPS", fps) || !cur.read_float("FPSScale", scale) || !finite(ticks_f) || !finite(fps) ||
					!finite(scale) || ticks_f < 0.0f || ticks_f > 1.0e7f || !(fps > 0.0f) || !(scale > 0.0f)) {
				return core::make_error(label(cur) + ": no NumTicks / FPS / FPSScale");
			}
			const uint32_t ticks = uint32_t(ticks_f);
			route.clip_controller = cur.guid().index();
			route.controller = std::string(cur.name());
			route.controller_package = pkg;
			route.anim_index = anim.index();
			route.num_ticks = ticks;
			route.fps = fps;
			route.fps_scale = scale;
			if (!route.sequence) {
				route.start_in_tick = 0.0f;
				route.end_in_tick = float(ticks);
			}
			return route;
		}
		if (type == "IndexChooserControllerAsset") {
			GdGuid ia;
			if (!cur.read_guid("IndexAsset", ia) || ia.is_null()) {
				return core::make_error(label(cur) + ": no IndexAsset");
			}
			const GdStruct index_asset = p_library.resolve(*pkg, ia);
			int64_t index = 0;
			if (!index_asset.valid() || !index_asset.read_int("Default", index)) {
				return core::make_error(label(cur) + ": IndexAsset " + hex(ia.index()) + " has no Default");
			}
			if (index_asset.name() == "WeaponType_EnumGamestate") {
				index = int64_t(p_indices.weapon_type);
			} else if (index_asset.name() == "SpecificWeapon_EnumGamestate" && p_indices.specific_weapon >= 0) {
				index = p_indices.specific_weapon;
			}
			const GdArray list = cur.array("ChoiceAssetList");
			if (index < 0 || !list.valid() || uint64_t(index) >= list.size()) {
				return core::make_error(label(cur) + ": index " + std::to_string(index) + " outside ChoiceAssetList (" + std::to_string(list.size()) + ")");
			}
			GdGuid g;
			if (!list.read_guid(uint32_t(index), g)) {
				return core::make_error(label(cur) + ": unreadable ChoiceAssetList entry");
			}
			if (core::Status s = follow(g, "ChoiceAssetList entry"); !s.ok()) {
				return s.error();
			}
			continue;
		}
		if (type == "ChooserControllerAsset") {
			GdGuid policy;
			const GdStruct pol = cur.read_guid("PolicyAsset", policy) && !policy.is_null() ? p_library.resolve(*pkg, policy) : GdStruct();
			if (!pol.valid() || pol.type_name() != "SignalChooserPolicyAsset") {
				return core::make_error(label(cur) + ": not a signal chooser (PolicyAsset " + (pol.valid() ? std::string(pol.type_name()) : std::string("missing")) + ")");
			}
			const GdArray entries = cur.array("EntryAssetList");
			if (!entries.valid() || entries.empty() || entries.size() > kMaxChooserEntries) {
				return core::make_error(label(cur) + ": " + std::to_string(entries.size()) + " entries (1.." + std::to_string(kMaxChooserEntries) + ")");
			}
			GdGuid chosen;
			for (uint32_t e = 0; e < entries.size() && chosen.is_null(); ++e) {
				if (core::Status s = r_budget.charge(1); !s.ok()) {
					return s.error();
				}
				GdGuid eg;
				const GdStruct entry = entries.read_guid(e, eg) && !eg.is_null() ? p_library.resolve(*pkg, eg) : GdStruct();
				if (!entry.valid() || entry.type_name() != "SignalChooserEntryAsset") {
					return core::make_error(label(cur) + ": entry " + std::to_string(e) + " is not a SignalChooserEntryAsset");
				}
				const AntPackage &epkg = p_library.package_of(*pkg, eg);
				bool match = true;
				for (const auto &[field, want] : { std::pair<const char *, bool>{ "TrueSignals", true }, std::pair<const char *, bool>{ "FalseSignals", false } }) {
					const GdArray sig = entry.array(field);
					if (sig.size() > kMaxSignals) {
						return core::make_error(label(cur) + ": entry " + std::to_string(e) + " has " + std::to_string(sig.size()) + " " + field);
					}
					for (uint32_t k = 0; k < sig.size() && match; ++k) {
						GdGuid sg;
						bool value = false;
						if (!sig.read_guid(k, sg) || !signal_default(p_library, epkg, sg, value)) {
							return core::make_error(label(cur) + ": entry " + std::to_string(e) + " " + field + "[" + std::to_string(k) + "] is not a BoolAsset");
						}
						match = value == want;
					}
				}
				if (match) {
					if (!entry.read_guid("ControllerAsset", chosen) || chosen.is_null()) {
						return core::make_error(label(cur) + ": entry " + std::to_string(e) + " has no ControllerAsset");
					}
					pkg = &epkg;
				}
			}
			if (chosen.is_null()) {
				return core::make_error(label(cur) + ": no entry matches the signals' defaults");
			}
			const GdStruct next = p_library.resolve(*pkg, chosen);
			if (!next.valid()) {
				return core::make_error(label(cur) + ": the chosen controller " + hex(chosen.index()) + " does not resolve");
			}
			pkg = &p_library.package_of(*pkg, chosen);
			cur = next;
			continue;
		}
		if (type == "LoopingControllerAsset") {
			route.looping = true;
			GdGuid g;
			if (!cur.read_guid("SubjectControllerAsset", g)) {
				return core::make_error(label(cur) + ": no SubjectControllerAsset");
			}
			if (core::Status s = follow(g, "SubjectControllerAsset"); !s.ok()) {
				return s.error();
			}
			continue;
		}
		if (type == "LayersControllerAsset") {
			uint32_t master = 0;
			const GdArray layers = cur.array("ControllerAssets");
			if (!cur.read_u32("MasterEntryIndex", master) || !layers.valid() || master >= layers.size()) {
				return core::make_error(label(cur) + ": MasterEntryIndex outside ControllerAssets (" + std::to_string(layers.size()) + ")");
			}
			GdGuid g;
			if (!layers.read_guid(master, g)) {
				return core::make_error(label(cur) + ": unreadable ControllerAssets entry");
			}
			if (core::Status s = follow(g, "master layer"); !s.ok()) {
				return s.error();
			}
			continue;
		}
		if (type == "SequenceContainerAsset") {
			const GdArray actors = cur.array("ActorAssets");
			GdGuid g;
			if (!actors.valid() || actors.empty() || !actors.read_guid(0, g)) {
				return core::make_error(label(cur) + ": no ActorAssets");
			}
			if (core::Status s = follow(g, "ActorAssets[0]"); !s.ok()) {
				return s.error();
			}
			continue;
		}
		if (type == "ActorControllerAsset") {
			const GdArray tracks = cur.array("Tracks");
			const GdStruct track = tracks.valid() && !tracks.empty() ? tracks.element(0) : GdStruct();
			const GdArray anims = track.valid() ? track.array("Anims") : GdArray();
			const GdStruct anim = anims.valid() && !anims.empty() ? anims.element(0) : GdStruct();
			GdGuid g;
			float in0 = 0.0f, in1 = 0.0f;
			if (!anim.valid() || !anim.read_guid("Asset", g) || !anim.read_float("StartInTime", in0) || !anim.read_float("EndInTime", in1) || !finite(in0) ||
					!finite(in1) || in0 < 0.0f || !(in1 > in0) || in1 > 1.0e6f) {
				return core::make_error(label(cur) + ": no Tracks[0].Anims[0] with Asset and a StartInTime < EndInTime window");
			}
			route.sequence = true;
			route.start_in_tick = in0;
			route.end_in_tick = in1;
			if (core::Status s = follow(g, "Anims[0].Asset"); !s.ok()) {
				return s.error();
			}
			continue;
		}
		return core::make_error(label(cur) + ": not a clip controller, chooser, loop, layer or sequence");
	}
}

float heading(float p_x, float p_z) {
	return std::atan2(p_x, p_z);
}

}

core::Result<AntClipRoute> route_ant_controller(const AntLibrary &p_library, const AntPackage &p_from, const GdGuid &p_controller, const ChooserIndices &p_indices) {
	core::gameplay::WorkBudget budget;
	const GdStruct start = p_library.resolve(p_from, p_controller);
	if (!start.valid()) {
		return core::make_error("controller " + hex(p_controller.index()) + " does not resolve");
	}
	return route_from(p_library, p_library.package_of(p_from, p_controller), start, p_indices, budget);
}

core::Result<AntClipRoute> route_ant_controller(const AntLibrary &p_library, std::string_view p_type, std::string_view p_name, const ChooserIndices &p_indices) {
	core::gameplay::WorkBudget budget;
	core::Result<GdStruct> start = find_static(p_library, p_type, p_name, budget);
	if (!start) {
		return start.error();
	}
	return route_from(p_library, *p_library.static_package(), *start, p_indices, budget);
}

core::Result<uint32_t> weapon_type_index(const AntLibrary &p_library, uint32_t p_ant_ref_asset_id) {
	const AntPackage *stat = p_library.static_package();
	const std::string where = "weapon AntRef " + std::to_string(p_ant_ref_asset_id);
	if (stat == nullptr) {
		return core::make_error(where + ": the library has no static package");
	}
	if ((p_ant_ref_asset_id & 0xFFFF0000u) != kAntRefHigh) {
		return core::make_error(where + ": not an s_basicassets AntRef");
	}
	core::gameplay::WorkBudget budget;
	GdStruct weapon;
	uint32_t hits = 0;
	for (size_t i = 0; i < stat->object_count(); ++i) {
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at(where, s.error());
		}
		const GdStruct o = stat->object(i);
		if ((o.guid().index() & 0xFFFFu) == (p_ant_ref_asset_id & 0xFFFFu) && o.type_name() == "WeaponAsset") {
			++hits;
			weapon = o;
		}
	}
	if (hits != 1) {
		return core::make_error(where + ": " + std::to_string(hits) + " WeaponAssets (must be exactly one)");
	}
	GdGuid g;
	const GdStruct pair = weapon.read_guid("WeaponClassEnum", g) && !g.is_null() ? p_library.resolve(*stat, g) : GdStruct();
	if (!pair.valid() || pair.type_name() != "EnumerationEnumeratorPair") {
		return core::make_error(where + ": no WeaponClassEnum pair");
	}
	const GdStruct value = pair.read_guid("Enumerator", g) && !g.is_null() ? p_library.resolve(*stat, g) : GdStruct();
	uint32_t index = 0;
	if (!value.valid() || value.type_name() != "EnumerationValueAsset" || !value.read_u32("Value", index)) {
		return core::make_error(where + ": the WeaponClassEnum pair has no EnumerationValueAsset Value");
	}
	return index;
}

core::Result<MoveGroupDef> read_move_group(const AntLibrary &p_library, std::string_view p_name, const ChooserIndices &p_indices) {
	core::gameplay::WorkBudget budget;
	const std::string where = "move group " + std::string(p_name);
	core::Result<GdStruct> group = find_static(p_library, "StreetMoveGroupControllerAsset", p_name, budget);
	if (!group) {
		return core::error_at(where, group.error());
	}
	const AntPackage &stat = *p_library.static_package();
	MoveGroupDef def;
	def.name = std::string(p_name);
	if (!group->read_float("OneToOneSpeed", def.one_to_one_speed) || !group->read_float("MinSpeed", def.min_speed) ||
			!group->read_float("MasterLength", def.master_length) || !group->read_float("DirectionTolerance", def.direction_tolerance) ||
			!finite(def.one_to_one_speed) || !finite(def.min_speed) || !finite(def.master_length) || !(def.master_length > 0.0f) || def.min_speed < 0.0f ||
			def.one_to_one_speed < 0.0f || def.one_to_one_speed > 16.0f) {
		return core::make_error(where + ": missing or invalid OneToOneSpeed / MinSpeed / MasterLength / DirectionTolerance");
	}
	GdGuid g;
	const GdStruct outer = group->read_guid("BlendArrayControllerAsset", g) && !g.is_null() ? p_library.resolve(stat, g) : GdStruct();
	if (!outer.valid() || outer.type_name() != "BlendArrayAsset") {
		return core::make_error(where + ": BlendArrayControllerAsset is not a BlendArrayAsset");
	}
	const AntPackage &opkg = p_library.package_of(stat, g);
	const GdArray items = outer.array("Items");
	if (!items.valid() || items.empty() || items.size() > kMaxMoveGroupDirections) {
		return core::make_error(where + ": " + std::to_string(items.size()) + " directions (1.." + std::to_string(kMaxMoveGroupDirections) + ")");
	}
	if (!outer.read_float("Min", def.x_min) || !outer.read_float("Max", def.x_max) || !finite(def.x_min) || !finite(def.x_max) || !(def.x_max >= def.x_min) ||
			std::fabs(def.x_min) > kMaxMoveGroupAngle || std::fabs(def.x_max) > kMaxMoveGroupAngle) {
		return core::make_error(where + ": the angle array has no finite Min <= Max within +-" + std::to_string(kMaxMoveGroupAngle) + " rad");
	}
	std::vector<uint32_t> item_guid;
	for (uint32_t d = 0; d < items.size(); ++d) {
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at(where, s.error());
		}
		const GdStruct di = items.element(d);
		MoveGroupDirection dir;
		GdGuid ig;
		if (!di.valid() || !di.read_float("X", dir.x) || !finite(dir.x) || std::fabs(dir.x) > kMaxMoveGroupAngle || !di.read_guid("Clip", ig) || ig.is_null()) {
			return core::make_error(where + ": direction " + std::to_string(d) + " has no X within +-" + std::to_string(kMaxMoveGroupAngle) + " rad or no Clip");
		}
		const GdStruct inner = p_library.resolve(opkg, ig);
		if (!inner.valid() || inner.type_name() != "BlendArrayAsset") {
			return core::make_error(where + ": direction " + std::to_string(d) + " is not a BlendArrayAsset");
		}
		const AntPackage &ipkg = p_library.package_of(opkg, ig);
		const GdArray entries = inner.array("Items");
		if (!entries.valid() || entries.empty() || entries.size() > kMaxMoveGroupEntries) {
			return core::make_error(where + ": direction " + std::to_string(d) + " has " + std::to_string(entries.size()) + " speed entries (1.." +
					std::to_string(kMaxMoveGroupEntries) + ")");
		}
		for (uint32_t e = 0; e < entries.size(); ++e) {
			const GdStruct ei = entries.element(e);
			MoveGroupEntry entry;
			GdGuid mg;
			if (!ei.valid() || !ei.read_float("X", entry.velocity) || !finite(entry.velocity) || entry.velocity < 0.0f || entry.velocity > 1.0f ||
					!ei.read_guid("Clip", mg) || mg.is_null()) {
				return core::make_error(where + ": direction " + std::to_string(d) + " entry " + std::to_string(e) + " has no X in [0, 1] m/tick or no Clip");
			}
			const GdStruct item = p_library.resolve(ipkg, mg);
			if (!item.valid() || item.type_name() != "StreetMoveGroupItemControllerAsset") {
				return core::make_error(where + ": direction " + std::to_string(d) + " entry " + std::to_string(e) + " is not a StreetMoveGroupItemControllerAsset");
			}
			const AntPackage &mpkg = p_library.package_of(ipkg, mg);
			GdGuid cg;
			if (!item.read_float("ItemSpeed", entry.item_speed) || !item.read_float("ItemStartPhase", entry.start_phase) ||
					!item.read_float("ItemAchievedAngle", entry.achieved_angle) || !item.read_guid("ControllerAsset", cg) || !finite(entry.item_speed) ||
					!finite(entry.start_phase) || !finite(entry.achieved_angle) || !(entry.item_speed > 0.0f) || entry.item_speed > 16.0f) {
				return core::make_error(where + ": " + label(item) + " has no ItemSpeed in (0, 16], ItemStartPhase, ItemAchievedAngle or ControllerAsset");
			}
			core::Result<AntClipRoute> route = route_ant_controller(p_library, mpkg, cg, p_indices);
			if (!route) {
				return core::error_at(where + " " + label(item), route.error());
			}
			uint32_t clip = 0;
			while (clip < def.clips.size() && def.clips[clip].clip_controller != route->clip_controller) {
				++clip;
			}
			if (clip == def.clips.size()) {
				if (def.clips.size() >= kMaxMoveGroupClips) {
					return core::make_error(where + ": more than " + std::to_string(kMaxMoveGroupClips) + " clips");
				}
				def.clips.push_back(std::move(*route));
			}
			entry.clip = clip;
			dir.entries.push_back(entry);
		}
		std::stable_sort(dir.entries.begin(), dir.entries.end(), [](const MoveGroupEntry &a, const MoveGroupEntry &b) { return a.velocity < b.velocity; });
		def.directions.push_back(std::move(dir));
	}
	std::stable_sort(def.directions.begin(), def.directions.end(), [](const MoveGroupDirection &a, const MoveGroupDirection &b) { return a.x < b.x; });
	return def;
}

float move_group_x(const Vec3 &p_local, float p_x_sign) {
	if (!std::isfinite(p_local.x) || !std::isfinite(p_local.z) || (p_local.x == 0.0f && p_local.z == 0.0f) || !std::isfinite(p_x_sign)) {
		return 0.0f;
	}
	return p_x_sign * heading(p_local.x, p_local.z);
}

MoveGroupWeights move_group_weights(const MoveGroupDef &p_def, std::span<const MoveGroupClipTiming> p_timing, float p_x, float p_speed,
		const MoveGroupOptions &p_options) {
	MoveGroupWeights out;
	if (p_def.directions.empty()) {
		return out;
	}
	float x = std::isfinite(p_x) ? p_x : 0.0f;
	float speed = std::isfinite(p_speed) && p_speed > 0.0f ? p_speed : 0.0f;
	speed = std::min(speed, 1.0e3f);
	const float v = speed / kAntTicksPerSecond;
	out.velocity = v;
	out.below_min_speed = v < p_def.min_speed;

	if (std::fabs(x) > 1.0e4f) {
		x = 0.0f;
	}

	const bool range_ok = std::isfinite(p_def.x_min) && std::isfinite(p_def.x_max) && p_def.x_max >= p_def.x_min;
	if (range_ok && p_def.x_max > p_def.x_min) {
		const float turns = std::floor((x - p_def.x_min) / kTwoPi);
		if (std::isfinite(turns)) {
			x -= turns * kTwoPi;
		}
	}
	x = range_ok ? std::clamp(x, p_def.x_min, p_def.x_max) : 0.0f;
	out.x = x;
	const std::vector<MoveGroupDirection> &dirs = p_def.directions;
	size_t lo = 0, hi = 0;
	float t = 0.0f;
	if (x <= dirs.front().x) {
		lo = hi = 0;
	} else if (x >= dirs.back().x) {
		lo = hi = dirs.size() - 1;
	} else {
		for (size_t k = 0; k + 1 < dirs.size(); ++k) {
			if (x >= dirs[k].x && x <= dirs[k + 1].x) {
				lo = k;
				hi = k + 1;
				const float span = dirs[hi].x - dirs[lo].x;
				t = span > 0.0f ? (x - dirs[lo].x) / span : 0.0f;
				break;
			}
		}
	}
	auto timing = [&](uint32_t p_clip) { return p_clip < p_timing.size() ? p_timing[p_clip] : MoveGroupClipTiming{}; };
	auto add = [&](uint32_t p_clip, float p_w, float p_rate, float p_start) {
		if (!(p_w > 0.0f)) {
			return;
		}
		for (uint32_t k = 0; k < out.count; ++k) {
			MoveGroupTerm &m = out.terms[k];
			if (m.clip == p_clip) {
				const float w = m.weight + p_w;
				m.rate = (m.rate * m.weight + p_rate * p_w) / w;
				m.weight = w;
				return;
			}
		}
		if (out.count < kMaxMoveGroupTerms) {
			out.terms[out.count++] = MoveGroupTerm{ p_clip, p_w, p_rate, p_start };
		}
	};
	auto inner = [&](const MoveGroupDirection &p_dir, float p_w) {
		if (!(p_w > 0.0f) || p_dir.entries.empty()) {
			return;
		}

		std::array<float, kMaxMoveGroupEntries> vel{};
		const size_t n = std::min<size_t>(p_dir.entries.size(), kMaxMoveGroupEntries);
		for (size_t e = 0; e < n; ++e) {
			const MoveGroupEntry &en = p_dir.entries[e];
			const MoveGroupClipTiming tm = timing(en.clip);
			vel[e] = en.velocity;
			if (p_options.velocity_from_root && en.velocity > 0.0f && tm.root_velocity > 0.0f && std::isfinite(tm.root_velocity)) {
				vel[e] = tm.root_velocity * en.item_speed;
			}
		}
		const float master = p_def.master_length > 0.0f ? p_def.master_length : 100.0f;
		auto start_of = [&](const MoveGroupEntry &p_e) {
			const float s = p_e.start_phase / master;
			return s - std::floor(s);
		};
		if (n == 1 || v <= vel[0]) {
			const MoveGroupEntry &e0 = p_dir.entries[0];
			float rate = e0.item_speed;
			if (n == 1 && vel[0] > 0.0f && v > vel[0]) {
				rate *= 1.0f + p_def.one_to_one_speed * (v / vel[0] - 1.0f);
			}
			add(e0.clip, p_w, rate, start_of(e0));
			return;
		}
		if (v >= vel[n - 1]) {
			const MoveGroupEntry &el = p_dir.entries[n - 1];
			float rate = el.item_speed;
			if (vel[n - 1] > 0.0f) {
				rate *= 1.0f + p_def.one_to_one_speed * (v / vel[n - 1] - 1.0f);
			}
			add(el.clip, p_w, rate, start_of(el));
			return;
		}
		for (size_t e = 0; e + 1 < n; ++e) {
			if (v >= vel[e] && v <= vel[e + 1]) {
				const float span = vel[e + 1] - vel[e];
				const float u = span > 0.0f ? (v - vel[e]) / span : 1.0f;
				add(p_dir.entries[e].clip, p_w * (1.0f - u), p_dir.entries[e].item_speed, start_of(p_dir.entries[e]));
				add(p_dir.entries[e + 1].clip, p_w * u, p_dir.entries[e + 1].item_speed, start_of(p_dir.entries[e + 1]));
				return;
			}
		}

		size_t best = 0;
		for (size_t e = 1; e < n; ++e) {
			if (std::fabs(v - vel[e]) < std::fabs(v - vel[best])) {
				best = e;
			}
		}
		add(p_dir.entries[best].clip, p_w, p_dir.entries[best].item_speed, start_of(p_dir.entries[best]));
	};
	inner(dirs[lo], lo == hi ? 1.0f : 1.0f - t);
	if (hi != lo) {
		inner(dirs[hi], t);
	}

	float wsum = 0.0f, cps = 0.0f;
	for (uint32_t k = 0; k < out.count; ++k) {
		const MoveGroupClipTiming tm = timing(out.terms[k].clip);
		if (tm.seconds > 0.0f && std::isfinite(tm.seconds)) {
			++out.moving;
			wsum += out.terms[k].weight;
			cps += out.terms[k].weight * out.terms[k].rate / tm.seconds;
		}
	}
	out.cycles_per_second = wsum > 0.0f && std::isfinite(cps) ? cps / wsum : 0.0f;
	return out;
}

}

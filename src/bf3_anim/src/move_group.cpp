#include "bf3/anim/move_group.h"

#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/util/finite.h"

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

using bf3::core::finite;

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

bool read_flag(const GdStruct &p_s, std::string_view p_name, bool &r_value) {
	if (p_s.read_bool(p_name, r_value)) {
		return true;
	}
	int64_t v = 0;
	if (!p_s.read_int(p_name, v)) {
		return false;
	}
	r_value = v != 0;
	return true;
}

int64_t driven_index(const GdStruct &p_driver, int64_t p_default, const ChooserIndices &p_indices) {
	if (p_driver.name() == "WeaponType_EnumGamestate") {
		return int64_t(p_indices.weapon_type);
	}
	if (p_driver.name() == "SpecificWeapon_EnumGamestate" && p_indices.specific_weapon >= 0) {
		return p_indices.specific_weapon;
	}
	return p_default;
}

core::Result<bool> chooser_step(
		const AntLibrary &p_library, const AntPackage *&r_pkg, GdStruct &r_cur, const ChooserIndices &p_indices) {
	const std::string_view type = r_cur.type_name();
	auto take = [&](const GdGuid &p_g, const char *p_what) -> core::Result<bool> {
		const GdStruct next = p_g.is_null() ? GdStruct() : p_library.resolve(*r_pkg, p_g);
		if (!next.valid()) {
			return core::make_error(label(r_cur) + ": " + p_what + " " + hex(p_g.index()) + " does not resolve");
		}
		r_pkg = &p_library.package_of(*r_pkg, p_g);
		r_cur = next;
		return true;
	};
	if (type == "IndexChooserControllerAsset") {
		GdGuid ia;
		if (!r_cur.read_guid("IndexAsset", ia) || ia.is_null()) {
			return core::make_error(label(r_cur) + ": no IndexAsset");
		}
		const GdStruct index_asset = p_library.resolve(*r_pkg, ia);
		int64_t index = 0;
		if (!index_asset.valid() || !index_asset.read_int("Default", index)) {
			return core::make_error(label(r_cur) + ": IndexAsset " + hex(ia.index()) + " has no Default");
		}
		index = driven_index(index_asset, index, p_indices);
		const GdArray list = r_cur.array("ChoiceAssetList");
		if (index < 0 || !list.valid() || uint64_t(index) >= list.size()) {
			return core::make_error(label(r_cur) + ": index " + std::to_string(index) + " outside ChoiceAssetList (" +
					std::to_string(list.size()) + ")");
		}
		GdGuid g;
		if (!list.read_guid(uint32_t(index), g)) {
			return core::make_error(label(r_cur) + ": unreadable ChoiceAssetList entry");
		}
		return take(g, "ChoiceAssetList entry");
	}
	if (type == "ContextDatabaseChooserWrapperAsset") {
		GdGuid g;
		if (!r_cur.read_guid("ContextDatabaseAsset", g)) {
			return core::make_error(label(r_cur) + ": no ContextDatabaseAsset");
		}
		return take(g, "ContextDatabaseAsset");
	}
	if (type == "ContextDatabaseAsset") {
		const GdArray fields = r_cur.array("FieldDescriptionAssets");
		const GdStruct field = fields.valid() && fields.size() == 1 ? fields.element(0) : GdStruct();
		GdGuid dg;
		int64_t value = 0;
		bool shift = false;
		if (!field.valid() || !field.read_int("IntDefault", value) ||
				!read_flag(field, "AutoShiftEnumsForBitmap", shift)) {
			return core::make_error(label(r_cur) + ": " + std::to_string(fields.size()) +
					" FieldDescriptionAssets (must be one with IntDefault and AutoShiftEnumsForBitmap)");
		}
		const GdStruct driver = field.read_guid("GameStateDriverAsset0", dg) && !dg.is_null()
				? p_library.resolve(*r_pkg, dg)
				: GdStruct();
		if (driver.valid()) {
			int64_t driver_default = value;
			if (driver.read_int("Default", driver_default)) {
				value = driver_default;
			}
			value = driven_index(driver, value, p_indices);
		}
		if (value < 0 || (shift && value >= 63)) {
			return core::make_error(label(r_cur) + ": field value " + std::to_string(value) + " out of range");
		}
		const GdArray entries = r_cur.array("EntryAssets");
		if (!entries.valid() || entries.empty() || entries.size() > kMaxChooserEntries) {
			return core::make_error(label(r_cur) + ": " + std::to_string(entries.size()) + " EntryAssets (1.." +
					std::to_string(kMaxChooserEntries) + ")");
		}
		for (uint32_t e = 0; e < entries.size(); ++e) {
			const GdStruct entry = entries.element(e);
			const GdArray cells = entry.valid() ? entry.array("Cells") : GdArray();
			const GdStruct cell = cells.valid() && cells.size() == 1 ? cells.element(0) : GdStruct();
			int64_t key = 0;
			GdGuid g;
			if (!cell.valid() || !cell.read_int("Int0", key) || !entry.read_guid("Asset", g)) {
				return core::make_error(
						label(r_cur) + ": entry " + std::to_string(e) + " has no single Cells[0].Int0 and Asset");
			}
			if (shift ? (uint64_t(key) & (uint64_t(1) << value)) != 0 : key == value) {
				return take(g, "EntryAssets Asset");
			}
		}
		return core::make_error(label(r_cur) + ": no entry holds field value " + std::to_string(value));
	}
	return false;
}

bool read_branch_tags(const AntLibrary &p_library, const AntPackage &p_pkg, const GdStruct &p_owner,
		std::vector<AntBranchTag> &r_tags) {
	GdGuid sg;
	if (!p_owner.read_guid("TagCollectionSet", sg) || sg.is_null()) {
		return true;
	}
	const GdStruct set = p_library.resolve(p_pkg, sg);
	if (!set.valid()) {
		return false;
	}
	const AntPackage &set_pkg = p_library.package_of(p_pkg, sg);
	const GdArray keys = set.array("TagCollectionKeys");
	for (uint32_t k = 0; keys.valid() && k < keys.size() && k < kMaxSignals; ++k) {
		GdGuid cg;
		const GdStruct coll = keys.read_guid(k, cg) && !cg.is_null() ? p_library.resolve(set_pkg, cg) : GdStruct();
		if (!coll.valid()) {
			return false;
		}
		const AntPackage &coll_pkg = p_library.package_of(set_pkg, cg);
		const GdArray tags = coll.array("TagKeys");
		for (uint32_t t = 0; tags.valid() && t < tags.size() && t < kMaxSignals; ++t) {
			GdGuid tg;
			const GdStruct tag = tags.read_guid(t, tg) && !tg.is_null() ? p_library.resolve(coll_pkg, tg) : GdStruct();
			if (!tag.valid()) {
				return false;
			}
			const AntPackage &tag_pkg = p_library.package_of(coll_pkg, tg);
			AntBranchTag b;
			GdGuid bt;
			if (tag.type_name() == "BranchOutPointTag") {
				if (!tag.read_float("Time", b.time) || !tag.read_float("Length", b.length) ||
						!tag.read_guid("BranchTypeAsset", bt)) {
					return false;
				}
			} else if (tag.type_name() == "BranchInWindowTag") {
				b.branch_in = true;
				if (!tag.read_float("Time", b.time) || !tag.read_guid("BranchType", bt)) {
					return false;
				}
			} else {
				continue;
			}
			const GdStruct type = bt.is_null() ? GdStruct() : p_library.resolve(tag_pkg, bt);
			if (!type.valid() || !finite(b.time) || !finite(b.length)) {
				return false;
			}
			b.type = std::string(type.name());
			r_tags.push_back(std::move(b));
		}
	}
	return true;
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
		if (core::Result<bool> chose = chooser_step(p_library, pkg, cur, p_indices); !chose) {
			return chose.error();
		} else if (*chose) {
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
			if (!cur.read_float("LoopEndTime", route.loop_end_tick) || !finite(route.loop_end_tick) ||
					route.loop_end_tick < 0.0f) {
				return core::make_error(label(cur) + ": no LoopEndTime");
			}
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
			if (!read_branch_tags(p_library, *pkg, cur, route.branches)) {
				return core::make_error(label(cur) + ": unreadable branch tags");
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

core::Result<AntHoldReleaseFlow> read_hold_release_flow(
		const AntLibrary &p_library, std::string_view p_wrapper, const ChooserIndices &p_indices) {
	core::gameplay::WorkBudget budget;
	const std::string where = "hold-and-release flow under " + std::string(p_wrapper);
	core::Result<GdStruct> start = find_static(p_library, "ContextDatabaseChooserWrapperAsset", p_wrapper, budget);
	if (!start) {
		return core::error_at(where, start.error());
	}
	GdStruct cur = *start;
	const AntPackage *pkg = p_library.static_package();
	for (uint32_t depth = 0; cur.type_name() != "StateFlowControllerAsset"; ++depth) {
		if (depth >= kMaxAntRouteDepth) {
			return core::make_error(
					where + ": no StateFlowControllerAsset within " + std::to_string(kMaxAntRouteDepth) + " choices");
		}
		core::Result<bool> chose = chooser_step(p_library, pkg, cur, p_indices);
		if (!chose) {
			return core::error_at(where, chose.error());
		}
		if (!*chose) {
			return core::make_error(where + ": " + label(cur) + " is neither a chooser nor a state flow");
		}
	}
	AntHoldReleaseFlow flow;
	flow.name = std::string(cur.name());
	const AntPackage &flow_pkg = *pkg;
	struct Node {
		GdStruct s;
		const AntPackage *pkg = nullptr;
	};
	auto node_at = [&](const AntPackage &p_from, const GdGuid &p_g) -> Node {
		Node n;
		n.s = p_g.is_null() ? GdStruct() : p_library.resolve(p_from, p_g);
		n.pkg = n.s.valid() ? &p_library.package_of(p_from, p_g) : nullptr;
		return n;
	};
	auto requires_signal = [&](const Node &p_n, const char *p_field) {
		const GdArray list = p_n.s.array(p_field);
		for (uint32_t i = 0; list.valid() && i < list.size() && i < kMaxSignals; ++i) {
			GdGuid g;
			const GdStruct b = list.read_guid(i, g) && !g.is_null() ? p_library.resolve(*p_n.pkg, g) : GdStruct();
			if (b.valid() && b.type_name() == "BoolAsset" && b.name() == kHoldAndReleaseSignal) {
				return true;
			}
		}
		return false;
	};
	struct Edge {
		Node to;
		std::string out_type, in_type;
		bool conditioned = false;
	};
	auto edges_of = [&](const Node &p_n, std::vector<Edge> &r_edges) -> bool {
		const GdArray list = p_n.s.array("Transitions");
		if (!list.valid() || list.size() > kMaxSignals) {
			return false;
		}
		for (uint32_t i = 0; i < list.size(); ++i) {
			GdGuid g;
			const Node t = list.read_guid(i, g) ? node_at(*p_n.pkg, g) : Node{};
			GdGuid to;
			if (!t.s.valid() || t.s.type_name() != "StateFlowTransitionAsset" || !t.s.read_guid("To", to)) {
				return false;
			}
			Edge e;
			e.to = node_at(*t.pkg, to);
			if (!e.to.s.valid() || e.to.s.type_name() != "StateFlowNodeControllerAsset") {
				return false;
			}
			for (const auto &[field, out] :
					{ std::pair<const char *, std::string *>{ "BranchWindowTypeAsset", &e.out_type },
							std::pair<const char *, std::string *>{ "BranchInWindowTypeAsset", &e.in_type } }) {
				GdGuid bt;
				const GdStruct type =
						t.s.read_guid(field, bt) && !bt.is_null() ? p_library.resolve(*t.pkg, bt) : GdStruct();
				if (type.valid()) {
					*out = std::string(type.name());
				}
			}
			e.conditioned = t.s.array("ConditionsRequiredTrue").size() > 0;
			r_edges.push_back(std::move(e));
		}
		return true;
	};
	auto route_of = [&](const Node &p_n, AntClipRoute &r_route) -> core::Status {
		GdGuid g;
		if (!p_n.s.read_guid("SubjectController", g) || g.is_null()) {
			return core::make_error(where + ": " + label(p_n.s) + " has no SubjectController");
		}
		const GdStruct subject = p_library.resolve(*p_n.pkg, g);
		if (!subject.valid()) {
			return core::make_error(where + ": " + label(p_n.s) + " subject does not resolve");
		}
		core::Result<AntClipRoute> r =
				route_from(p_library, p_library.package_of(*p_n.pkg, g), subject, p_indices, budget);
		if (!r) {
			return core::error_at(where + ": " + label(p_n.s), r.error());
		}
		r_route = std::move(*r);
		return core::Status::success();
	};
	auto window = [&](const AntClipRoute &p_route, const std::string &p_type, bool p_in, AntBranchTag &r_tag) {
		for (const AntBranchTag &b : p_route.branches) {
			if (b.branch_in == p_in && b.type == p_type) {
				r_tag = b;
				return true;
			}
		}
		return false;
	};

	GdGuid sg;
	const Node null_node = cur.read_guid("StartNode", sg) ? node_at(flow_pkg, sg) : Node{};
	if (!null_node.s.valid() || !requires_signal(null_node, "EntryConditionsRequiredFalse")) {
		return core::make_error(where + ": " + flow.name + " has no StartNode that requires " +
				std::string(kHoldAndReleaseSignal) + " false");
	}
	std::vector<Edge> from_null, from_start, from_release, from_deploy;
	if (!edges_of(null_node, from_null) || from_null.size() != 1 ||
			!requires_signal(from_null[0].to, "EntryConditionsRequiredTrue")) {
		return core::make_error(where + ": the StartNode has no single transition into a node that requires " +
				std::string(kHoldAndReleaseSignal));
	}
	const Node start_node = from_null[0].to;
	if (!edges_of(start_node, from_start)) {
		return core::make_error(where + ": unreadable transitions from " + label(start_node.s));
	}
	const Edge *to_release = nullptr, *to_hold = nullptr;
	for (const Edge &e : from_start) {
		if (requires_signal(e.to, "EntryConditionsRequiredFalse")) {
			to_release = &e;
		} else if (requires_signal(e.to, "EntryConditionsRequiredTrue")) {
			to_hold = &e;
		}
	}
	if (to_release == nullptr || to_hold == nullptr) {
		return core::make_error(where + ": " + label(start_node.s) + " lacks a release or a hold transition");
	}
	const Node release_node = to_release->to;
	const Node hold_node = to_hold->to;
	if (!edges_of(release_node, from_release)) {
		return core::make_error(where + ": unreadable transitions from " + label(release_node.s));
	}
	const Edge *to_deploy = nullptr;
	for (const Edge &e : from_release) {
		if (e.conditioned && e.to.s.guid().index() != null_node.s.guid().index()) {
			to_deploy = &e;
		}
	}
	if (to_deploy == nullptr) {
		return core::make_error(
				where + ": " + label(release_node.s) + " has no conditioned transition to a deploy node");
	}
	const Node deploy_node = to_deploy->to;
	if (!edges_of(deploy_node, from_deploy)) {
		return core::make_error(where + ": unreadable transitions from " + label(deploy_node.s));
	}
	const Edge *deploy_to_null = nullptr;
	for (const Edge &e : from_deploy) {
		if (e.to.s.guid().index() == null_node.s.guid().index()) {
			deploy_to_null = &e;
		}
	}
	if (deploy_to_null == nullptr) {
		return core::make_error(where + ": " + label(deploy_node.s) + " has no transition back to the StartNode");
	}
	for (const auto &[node, route] : { std::pair<const Node *, AntClipRoute *>{ &null_node, &flow.idle },
				 std::pair<const Node *, AntClipRoute *>{ &start_node, &flow.start },
				 std::pair<const Node *, AntClipRoute *>{ &hold_node, &flow.hold },
				 std::pair<const Node *, AntClipRoute *>{ &release_node, &flow.release },
				 std::pair<const Node *, AntClipRoute *>{ &deploy_node, &flow.deploy } }) {
		if (core::Status s = route_of(*node, *route); !s.ok()) {
			return s.error();
		}
	}
	if (!flow.hold.looping || !(flow.hold.loop_end_tick > 0.0f)) {
		return core::make_error(where + ": the hold node " + label(hold_node.s) + " is not a loop");
	}
	AntBranchTag tag;
	if (!window(flow.start, to_release->out_type, false, tag)) {
		return core::make_error(where + ": " + flow.start.controller + " has no " + to_release->out_type + " window");
	}
	flow.release_from_tick = tag.time;
	flow.release_until_tick = tag.time + tag.length;
	flow.release_in_tick = 0.0f;
	if (!to_release->in_type.empty()) {
		if (!window(flow.release, to_release->in_type, true, tag)) {
			return core::make_error(
					where + ": " + flow.release.controller + " has no " + to_release->in_type + " entry");
		}
		flow.release_in_tick = tag.time;
	}
	if (!window(flow.start, to_hold->out_type, false, tag)) {
		return core::make_error(where + ": " + flow.start.controller + " has no " + to_hold->out_type + " window");
	}
	flow.hold_from_tick = tag.time;
	if (!window(flow.release, to_deploy->out_type, false, tag)) {
		return core::make_error(where + ": " + flow.release.controller + " has no " + to_deploy->out_type + " window");
	}
	flow.release_end_tick = tag.time;
	if (!window(flow.deploy, deploy_to_null->out_type, false, tag)) {
		return core::make_error(
				where + ": " + flow.deploy.controller + " has no " + deploy_to_null->out_type + " window");
	}
	flow.deploy_end_tick = tag.time;
	return flow;
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

#include "bf3/anim/vault_anim.h"

#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/format.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace bf3::anim {

using core::fmt;
using core::ant::AntLibrary;
using core::ant::AntPackage;
using core::ant::GdArray;
using core::ant::GdGuid;
using core::ant::GdStruct;

namespace {

bool holds(const GdArray &p_list, uint32_t p_index) {
	if (!p_list.valid()) {
		return false;
	}
	for (uint32_t i = 0; i < p_list.size(); ++i) {
		GdGuid g;
		if (p_list.read_guid(i, g) && !g.is_null() && g.index() == p_index) {
			return true;
		}
	}
	return false;
}

float blend_ticks(const AntLibrary &p_library, const AntPackage &p_from, const GdStruct &p_transition) {
	GdGuid g;
	if (!p_transition.read_guid("OverrideBlend", g) || g.is_null()) {
		return 0.0f;
	}
	const GdStruct blend = p_library.resolve(p_from, g);
	float ticks = 0.0f;
	int64_t whole = 0;
	if (blend.valid() && blend.read_float("BlendTime", ticks) && core::finite(ticks) && ticks >= 0.0f) {
		return ticks;
	}
	if (blend.valid() && blend.read_int("BlendTime", whole) && whole >= 0) {
		return float(whole);
	}
	return 0.0f;
}

bool subject_of(const AntLibrary &p_library, const AntPackage &p_from, const GdStruct &p_node, VaultController &r_out) {
	GdGuid g;
	if (!p_node.valid() || !p_node.read_guid("SubjectController", g) || g.is_null()) {
		return false;
	}
	const GdStruct subject = p_library.resolve(p_from, g);
	if (!subject.valid()) {
		return false;
	}
	r_out.guid = g;
	r_out.package = &p_library.package_of(p_from, g);
	r_out.name = std::string(subject.name());
	r_out.node = std::string(p_node.name());
	return true;
}

}

core::Result<VaultControllers> find_vault_controllers(const AntLibrary &p_library) {
	const AntPackage *stat = p_library.static_package();
	if (stat == nullptr) {
		return core::make_error("vault controllers: the library has no static package");
	}
	core::gameplay::WorkBudget budget;
	VaultControllers out;
	std::array<GdGuid, kVaultAnimKinds> outs{};
	uint32_t owners = 0;
	for (size_t i = 0; i < stat->object_count(); ++i) {
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at("vault controllers", s.error());
		}
		const GdStruct o = stat->object(i);
		if (o.type_name() != kVaultSignalOwner) {
			continue;
		}
		++owners;
		for (size_t k = 0; k < kVaultAnimKinds; ++k) {
			if (!o.read_guid(kVaultTriggerFields[k], out.triggers[k]) || out.triggers[k].is_null() ||
					!o.read_guid(kVaultOutFields[k], outs[k]) || outs[k].is_null()) {
				return core::make_error(fmt("vault controllers: %s '%s' has no %s / %s",
						std::string(o.type_name()).c_str(), std::string(o.name()).c_str(),
						std::string(kVaultTriggerFields[k]).c_str(), std::string(kVaultOutFields[k]).c_str()));
			}
			const GdStruct signal = p_library.resolve(*stat, out.triggers[k]);
			out.trigger_names[k] = signal.valid() ? std::string(signal.name()) : std::string();
		}
	}
	if (owners != 1) {
		return core::make_error(fmt("vault controllers: %u %s objects in the static package (must be exactly one)",
				owners, std::string(kVaultSignalOwner).c_str()));
	}
	std::array<std::vector<uint32_t>, kVaultAnimKinds> first, third;
	for (size_t i = 0; i < stat->object_count(); ++i) {
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at("vault controllers", s.error());
		}
		const GdStruct o = stat->object(i);
		const std::string_view type = o.type_name();
		if (type == "StateFlowNodeControllerAsset") {
			const GdArray entry = o.array("EntryConditionsRequiredTrue");
			for (size_t k = 0; k < kVaultAnimKinds; ++k) {
				VaultController c;
				if (holds(entry, out.triggers[k].index()) && subject_of(p_library, *stat, o, c) &&
						std::find(first[k].begin(), first[k].end(), o.guid().index()) == first[k].end()) {
					first[k].push_back(o.guid().index());
					out.first_person[k] = std::move(c);
				}
			}
		} else if (type == "StateFlowTransitionAsset") {
			const GdArray conditions = o.array("ConditionsRequiredTrue");
			GdGuid to;
			if (!o.read_guid("To", to) || to.is_null()) {
				continue;
			}
			for (size_t k = 0; k < kVaultAnimKinds; ++k) {
				if (!holds(conditions, out.triggers[k].index()) && !holds(conditions, outs[k].index())) {
					continue;
				}
				const GdStruct node = p_library.resolve(*stat, to);
				VaultController c;
				if (!subject_of(p_library, p_library.package_of(*stat, to), node, c)) {
					continue;
				}
				c.blend_in_ticks = blend_ticks(p_library, *stat, o);
				if (std::find(third[k].begin(), third[k].end(), to.index()) == third[k].end()) {
					third[k].push_back(to.index());
					out.third_person[k] = std::move(c);
				}
			}
		}
	}
	for (size_t k = 0; k < kVaultAnimKinds; ++k) {
		if (first[k].size() != 1 || third[k].size() != 1) {
			return core::make_error(
					fmt("vault controllers: %s gates %zu first-person nodes and leads to %zu third-person "
						"nodes (must be one each)",
							out.trigger_names[k].c_str(), first[k].size(), third[k].size()));
		}
	}
	return out;
}

core::Result<VaultPlay> vault_play_of(const AntClipRoute &p_route) {
	VaultPlay p;
	p.frames_per_tick = core::finite(p_route.fps_scale) && p_route.fps_scale > 0.0f ? p_route.fps_scale : 0.5f;
	if (p_route.sequence) {
		p.start_tick = p_route.start_in_tick;
		p.end_tick = p_route.end_in_tick;
		p.clip_ticks_per_tick = p_route.sequence_scale;
	} else {
		p.start_tick = 0.0f;
		p.end_tick = float(p_route.num_ticks);
		p.clip_ticks_per_tick = 1.0f;
	}
	if (!core::finite(p.start_tick) || !core::finite(p.end_tick) || !(p.end_tick > p.start_tick) ||
			!core::finite(p.clip_ticks_per_tick) || !(p.clip_ticks_per_tick > 0.0f)) {
		return core::make_error(fmt("%s: no playable window (ticks %.2f .. %.2f, scale %.3f)",
				p_route.controller.c_str(), double(p.start_tick), double(p.end_tick), double(p.clip_ticks_per_tick)));
	}
	return p;
}

core::Result<AntClipRoute> route_vault(
		const AntLibrary &p_library, const VaultController &p_controller, const ChooserIndices &p_indices) {
	if (p_controller.package == nullptr) {
		return core::make_error("vault route: no controller");
	}
	core::Result<AntClipRoute> r = route_ant_controller(p_library, *p_controller.package, p_controller.guid, p_indices);
	if (!r) {
		return core::error_at("vault route " + p_controller.name, r.error());
	}
	return r;
}

core::Result<CrowdVaultClip> load_crowd_vault(const CrowdClipSet &p_set, const AntLibrary &p_library,
		const VaultControllers &p_controllers, size_t p_kind, const ChooserIndices &p_indices) {
	if (p_kind >= kVaultAnimKinds) {
		return core::make_error(fmt("crowd vault: kind %zu", p_kind));
	}
	const VaultController &c = p_controllers.third_person[p_kind];
	core::Result<AntClipRoute> route = route_vault(p_library, c, p_indices);
	if (!route) {
		return route.error();
	}
	core::Result<VaultPlay> play = vault_play_of(*route);
	if (!play) {
		return core::error_at("crowd vault " + c.name, play.error());
	}
	core::Result<CrowdClipData> clip = load_route_clip(p_set, p_library, *route, "crowd vault " + c.name);
	if (!clip) {
		return clip.error();
	}
	if (clip->tracks.end_frame == 0 || clip->mask.size() != p_set.rig.size()) {
		return core::make_error("crowd vault " + c.name + ": " + route->controller + " is not a rig-sized clip");
	}
	CrowdVaultClip out;
	if (p_set.root_joint >= 0) {
		LocalPose pose;
		core::ant::ClipPose scratch;
		if (sample_pose(p_set.rig, clip->clip, clip->tracks, play->frame_at(0.0f), pose, scratch)) {
			out.root_start = pose.joints[size_t(p_set.root_joint)].t;
		}
	}
	out.clip = std::move(*clip);
	out.play = *play;
	out.blend_in_seconds = c.blend_in_ticks / kAntTicksPerSecond;
	out.controller = route->controller;
	return out;
}

}

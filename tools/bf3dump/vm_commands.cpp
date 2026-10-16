// bf3dump M2a vm commands (docs/design/m2a-vm.md): `vm clips`, `vm skin`. See vm_commands.h.
#include "vm_commands.h"

#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/render/viewmodel_set.h"

#include <algorithm>
#include <cmath>

#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace bf3dump {

namespace {

using bf3::core::InstallSnapshot;
using bf3::core::SnapshotDiff;
using bf3::render::ViewmodelClip;
using bf3::render::ViewmodelClipRole;
using bf3::render::ViewmodelClipSource;
using bf3::render::ViewmodelItem;
using bf3::render::ViewmodelSet;
using bf3::render::ViewmodelSetSpec;

bool install_unchanged(const EbxContext &p_ctx, const InstallSnapshot &p_before) {
	if (p_ctx.reader == nullptr) {
		std::printf("install.unchanged unknown (no reader)\n");
		return true;
	}
	const SnapshotDiff diff = bf3::core::diff_snapshots(p_before, p_ctx.reader->snapshot());
	const bool clean = p_before.valid && diff.clean();
	std::printf("install.unchanged %s\n", clean ? ("yes (" + std::to_string(p_before.files.size()) + " files: sizes and mtimes equal)").c_str()
												: ("NO: " + diff.summary()).c_str());
	return clean;
}

void print_item(const ViewmodelItem &p_item) {
	const bf3::render::ViewmodelAdsData &a = p_item.ads;
	std::printf("item\t%s\tkind %s\tmesh %s\tpackage %s\tclass %s\n", p_item.unlock.c_str(), bf3::render::viewmodel_item_kind_name(p_item.kind),
			p_item.mesh.empty() ? "-" : p_item.mesh.c_str(), p_item.ant_package.empty() ? "-" : p_item.ant_package.c_str(),
			p_item.weapon_class.empty() ? "-" : p_item.weapon_class.c_str());
	std::printf("fov\t%s\tRenderFov %.1f\tZoomRenderFov %.1f (data %.1f)\tcontroller %s\tlevels %u\thip %.1f\tzoom %.1f\tlook %.2f/%.2f\t"
				"FovTransitionTime %.3f\tZoomTransitionTime %.3f\tin fov %.3f zoom %.3f\tout fov %.3f zoom %.3f\n",
			p_item.unlock.c_str(), double(p_item.render_fov), double(p_item.zoom_render_fov), double(p_item.zoom_render_fov_data),
			a.controller.empty() ? "-" : a.controller.c_str(), a.zoom_levels, double(a.hip_fov), double(a.zoom_fov), double(a.look_speed_hip),
			double(a.look_speed_zoom), double(a.fov_transition), double(a.zoom_transition), double(a.fov_in), double(a.zoom_in), double(a.fov_out),
			double(a.zoom_out));
	for (size_t r = 0; r < bf3::render::kViewmodelClipRoles; ++r) {
		const ViewmodelClip &c = p_item.clips[r];
		if (c.source == ViewmodelClipSource::Missing && c.name.empty()) {
			continue;
		}
		std::printf("role\t%s\t%-11s\t%-19s\t%-24s\t%-6s\tcontroller %-22s\t%.3f s\tframes %u\tbound %u unbound %u%s", p_item.unlock.c_str(),
				bf3::render::viewmodel_clip_role_name(ViewmodelClipRole(r)), bf3::render::viewmodel_clip_source_name(c.source), c.name.c_str(),
				c.codec.empty() ? "-" : c.codec.c_str(), c.controller.empty() ? "-" : c.controller.c_str(), double(c.seconds), c.tracks.end_frame,
				c.tracks.channels_bound, c.tracks.channels_unbound, c.blend_instead ? "\tblend instead" : "");
		if (ViewmodelClipRole(r) == ViewmodelClipRole::ZoomIn || ViewmodelClipRole(r) == ViewmodelClipRole::ZoomOut) {
			if (c.source == ViewmodelClipSource::Clip) {
				std::printf("\tendpoints %.2f deg", double(c.endpoint_deg));
			}
		}
		std::printf("\n");
	}
}

// vm skin: one item through a scripted run of the animator (deploy, idle, fire, idle, reload, idle at 30 fps), every
// surface of the weapon and the arms skinned each frame; the worst frame per surface. Returns the stretched surfaces.
uint32_t skin_item(const ViewmodelSet &p_set, const ViewmodelItem &p_item, const bf3::anim::ViewmodelTuning &p_tuning) {
	using bf3::anim::VmCue;
	const std::array<bf3::anim::VmClipRef, bf3::anim::kVmClips> refs = bf3::render::viewmodel_clip_refs(p_item);
	bf3::anim::ViewmodelAnimator animator(p_set.rig, refs, p_tuning);
	bf3::anim::LocalPose pose;
	std::vector<bf3::anim::Xform> model;
	bf3::anim::ViewmodelOutput out;
	struct Worst {
		bf3::render::ViewmodelSurfaceSkin s;
		double at = 0;
	};
	std::map<std::pair<int, size_t>, Worst> worst; // (0 weapon / 1 arms, subset)
	const double deploy = std::max(0.2, double(p_item.deploy_time));
	const double reload = std::max(0.5, double(p_item.reload_time));
	const double fire_at = deploy + 0.5, reload_at = fire_at + 1.5, end = reload_at + reload + 1.0;
	const float dt = 1.0f / 30.0f;
	std::vector<VmCue> cues;
	auto add = [&](VmCue::Kind p_kind, double p_t) {
		VmCue c;
		c.kind = p_kind;
		c.time_s = p_t;
		cues.push_back(c);
	};
	add(VmCue::Kind::DeployStarted, 0.0);
	add(VmCue::Kind::DeployFinished, deploy);
	add(p_item.kind == bf3::render::ViewmodelItemKind::Grenade ? VmCue::Kind::Thrown : VmCue::Kind::Shot, fire_at);
	if (p_item.kind == bf3::render::ViewmodelItemKind::Gun) {
		add(VmCue::Kind::ReloadStarted, reload_at);
		add(VmCue::Kind::ReloadFinished, reload_at + reload);
	}
	size_t next = 0;
	for (double t = 0; t <= end; t += dt) {
		bf3::anim::ViewmodelFrameInput in;
		in.time_s = t;
		in.dt = dt;
		const bool reloading = p_item.kind == bf3::render::ViewmodelItemKind::Gun && t >= reload_at && t < reload_at + reload;
		in.phase = t < deploy ? 1 : reloading ? 4 : 2; // Deploying / Reloading / Ready (sim WeaponPhase)
		in.phase_start_s = t < deploy ? 0.0 : reloading ? reload_at : (t < reload_at ? deploy : reload_at + reload);
		in.phase_end_s = t < deploy ? deploy : reloading ? reload_at + reload : 0.0;
		size_t first = next;
		while (next < cues.size() && cues[next].time_s <= t) {
			++next;
		}
		animator.step(in, std::span<const VmCue>(cues.data() + first, next - first), pose, out);
		bf3::anim::model_pose(p_set.rig, pose, model);
		for (int part = 0; part < 2; ++part) {
			const bf3::render::PreparedMesh &mesh = part == 0 ? p_item.weapon.prepared : p_set.arms.prepared;
			for (const bf3::render::ViewmodelSurfaceSkin &r : bf3::render::viewmodel_surface_skin(mesh, p_item.binds.binds(), model)) {
				Worst &w = worst[{ part, r.subset }];
				if (r.posed_edge >= w.s.posed_edge || w.s.vertices == 0) {
					w.s = r;
					w.at = t;
				}
			}
		}
	}
	// The idle pose at the end of the run: each surface's extent in camera space (CameraJoint, metres; the camera
	// looks along -Z of the joint turned about Y: reported as the joint's axes) and the bones its weights use.
	const int32_t cam = p_set.rig.find("CameraJoint");
	bf3::anim::Xform cam_inv = bf3::anim::xform_identity();
	if (cam >= 0 && size_t(cam) < model.size()) {
		cam_inv = bf3::anim::xform_inverse(model[size_t(cam)]);
	}
	for (int part = 0; part < 2; ++part) {
		const bf3::render::PreparedMesh &mesh = part == 0 ? p_item.weapon.prepared : p_set.arms.prepared;
		for (const bf3::render::ViewmodelSurfaceSkin &r : bf3::render::viewmodel_surface_skin(mesh, p_item.binds.binds(), model)) {
			float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
			for (int c = 0; c < 8; ++c) {
				const bf3::anim::Vec3 corner{ (c & 1) ? r.posed_max[0] : r.posed_min[0], (c & 2) ? r.posed_max[1] : r.posed_min[1],
					(c & 4) ? r.posed_max[2] : r.posed_min[2] };
				const bf3::anim::Vec3 q = bf3::anim::xform_point(cam_inv, corner);
				const float v[3] = { q.x, q.y, q.z };
				for (int k = 0; k < 3; ++k) {
					lo[k] = std::min(lo[k], v[k]);
					hi[k] = std::max(hi[k], v[k]);
				}
			}
			std::string bones;
			for (const int32_t b : r.binds) {
				const bf3::anim::SkinBind &sb = p_item.binds.binds()[size_t(b)];
				bones += " " + sb.name;
				if (part == 0 && sb.bone >= 0 && size_t(sb.bone) < model.size()) {
					// Driven by the idle clip? (a rotation or translation track on the bone)
					const bf3::anim::ClipTracks &tk = p_item.clips[size_t(ViewmodelClipRole::Idle)].tracks;
					const bool rot = std::find(tk.rotation_bone.begin(), tk.rotation_bone.end(), sb.bone) != tk.rotation_bone.end();
					const bool tr = std::find(tk.translation_bone.begin(), tk.translation_bone.end(), sb.bone) != tk.translation_bone.end();
					const bf3::anim::Vec3 q = bf3::anim::xform_point(cam_inv, model[size_t(sb.bone)].t);
					const int32_t parent = p_set.rig.bone(size_t(sb.bone)).parent;
					bones += std::string("[") + (rot ? "q" : "-") + (tr ? "t" : "-") + " " + std::to_string(q.x).substr(0, 6) + " " + std::to_string(q.y).substr(0, 6) +
							" " + std::to_string(q.z).substr(0, 6) + " parent " + (parent >= 0 ? p_set.rig.bone(size_t(parent)).name : std::string("-")) + "]";
				}
			}
			std::printf("skin.idle\t%s\t%s\tsubset %zu\t%s\tcamera-space box (%.3f %.3f %.3f)..(%.3f %.3f %.3f)\tbones%s\n", p_item.unlock.c_str(),
					part == 0 ? "weapon" : "arms", r.subset, r.material.c_str(), double(lo[0]), double(lo[1]), double(lo[2]), double(hi[0]), double(hi[1]),
					double(hi[2]), bones.c_str());
		}
	}
	// The weapon's material slots (shader, textures, vectors) per drawn surface.
	for (const bf3::render::PreparedSurface &sf : p_item.weapon.prepared.surfaces) {
		if (sf.material_index >= p_item.weapon.materials.slots.size()) {
			continue;
		}
		const bf3::render::MaterialSlot &m = p_item.weapon.materials.slots[sf.material_index];
		std::string l;
		for (const auto &[k, v] : m.textures) {
			l += " " + k + "=" + v;
		}
		for (const auto &[k, v] : m.vectors) {
			char b[128];
			std::snprintf(b, sizeof(b), " %s=(%.3g %.3g %.3g %.3g)", k.c_str(), double(v[0]), double(v[1]), double(v[2]), double(v[3]));
			l += b;
		}
		const bool alpha = sf.material_index < p_item.weapon_looks.size() && p_item.weapon_looks[sf.material_index].alpha_test;
		std::printf("skin.material\t%s\tsubset %zu\t%s\tshader %s\tvariation shader %s\t%s\t%s\n", p_item.unlock.c_str(), sf.subset,
				sf.material_name.c_str(), m.shader.c_str(), m.variation_shader.empty() ? "-" : m.variation_shader.c_str(), alpha ? "alpha-test" : "opaque",
				l.c_str());
	}
	// The Scale DOFs of the role clips (bf3_anim applies rotations and translations only): joint and value at the
	// first and last key, for the channels that are not 1.
	for (size_t role = 0; role < bf3::render::kViewmodelClipRoles; ++role) {
		const ViewmodelClip &c = p_item.clips[role];
		if (c.source != ViewmodelClipSource::Clip || c.clip.binding.rig == nullptr) {
			continue;
		}
		const bf3::core::ant::DecodedClip &d = c.clip.clip;
		const bf3::core::ant::ClipBinding &b = c.clip.binding;
		bf3::core::ant::ClipPose first, last;
		bf3::core::ant::sample_clip(d, 0.0f, first);
		bf3::core::ant::sample_clip(d, float(d.end_frame), last);
		uint32_t scales = 0;
		for (size_t ch = d.num_quats; ch < size_t(d.num_quats) + d.num_vec3s && ch < b.tracks.size(); ++ch) {
			const bf3::core::ant::ChannelTrack &tr = b.tracks[ch];
			if (tr.dof >= b.dofs.size() || b.dofs[tr.dof].kind != bf3::core::ant::DofKind::Scale) {
				continue;
			}
			++scales;
			const size_t v = ch - d.num_quats;
			if (v >= first.vec3s.size() || v >= last.vec3s.size()) {
				continue;
			}
			const bf3::anim::Vec3 a = first.vec3s[v], z = last.vec3s[v];
			if (std::fabs(a.x - 1) + std::fabs(a.y - 1) + std::fabs(a.z - 1) + std::fabs(z.x - 1) + std::fabs(z.y - 1) + std::fabs(z.z - 1) > 1e-4f) {
				std::printf("skin.scale\t%s\t%s\t%s\t%s\tfirst (%.3f %.3f %.3f) last (%.3f %.3f %.3f)\n", p_item.unlock.c_str(),
						bf3::render::viewmodel_clip_role_name(ViewmodelClipRole(role)), c.name.c_str(), std::string(b.dofs[tr.dof].slot).c_str(), double(a.x),
						double(a.y), double(a.z), double(z.x), double(z.y), double(z.z));
			}
		}
		std::printf("skin.clip\t%s\t%s\t%s\t%u quats, %u vec3s (%u scale DOFs), %u floats\n", p_item.unlock.c_str(),
				bf3::render::viewmodel_clip_role_name(ViewmodelClipRole(role)), c.name.c_str(), d.num_quats, d.num_vec3s, scales, d.num_floats);
	}
	uint32_t stretched = 0;
	for (const auto &[key, w] : worst) {
		const bf3::render::ViewmodelSurfaceSkin &r = w.s;
		stretched += r.stretched() ? 1 : 0;
		std::printf("skin\t%s\t%s\tsubset %zu\t%s\t%zu vertices\t%zu triangles\tbind edge %.3f m\tposed edge %.3f m (t %.2f s, bone %s)\tbad weights %zu\t%s\n",
				p_item.unlock.c_str(), key.first == 0 ? "weapon" : "arms", r.subset, r.material.c_str(), r.vertices, r.triangles, double(r.bind_edge),
				double(r.posed_edge), w.at, r.worst_bone.empty() ? "-" : r.worst_bone.c_str(), r.bad_weights, r.stretched() ? "STRETCHED" : "ok");
	}
	return stretched;
}

} // namespace

const char *vm_usage() {
	return "  vm clips [<unlock>...] [--team us|ru|all] [--level <sb>]   1p viewmodel clip roles, E5 reload rows, E11 coverage\n"
		   "  vm skin [<unlock>...] [--team us|ru|all] [--level <sb>]    skinned surfaces through a scripted run (stretched geometry)\n";
}

int cmd_vm(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	if (p_args.empty() || (p_args[0] != "clips" && p_args[0] != "skin")) {
		std::fprintf(stderr, "%s", vm_usage());
		return 2;
	}
	std::string level = "Win32/Levels/MP_Subway/MP_Subway";
	std::string team = "all";
	std::vector<std::string> unlocks;
	for (size_t i = 1; i < p_args.size(); ++i) {
		if (p_args[i] == "--team" && i + 1 < p_args.size()) {
			team = p_args[++i];
		} else if (p_args[i] == "--level" && i + 1 < p_args.size()) {
			level = p_args[++i];
		} else if (p_args[i].rfind("--", 0) == 0) {
			std::fprintf(stderr, "vm clips: unknown option %s\n%s", p_args[i].c_str(), vm_usage());
			return 2;
		} else {
			unlocks.push_back(p_args[i]);
		}
	}
	if (team != "us" && team != "ru" && team != "all") {
		std::fprintf(stderr, "vm clips: --team us|ru|all\n");
		return 2;
	}
	if (p_ctx.reader == nullptr) {
		std::fprintf(stderr, "vm clips: no install reader\n");
		return 77;
	}
	const InstallSnapshot before = p_ctx.reader->snapshot();
	const auto t0 = std::chrono::steady_clock::now();
	bf3::core::Result<std::unique_ptr<bf3::render::ViewmodelSources>> sources = bf3::render::open_viewmodel_sources(*p_ctx.reader, level, nullptr);
	if (!sources) {
		std::fprintf(stderr, "vm clips: %s\n", sources.error().message.c_str());
		install_unchanged(p_ctx, before);
		return 1;
	}
	std::fprintf(stderr, "vm clips: Ant library in %.2f s, %s in %.2f s\n", (*sources)->seconds_ant, level.c_str(), (*sources)->seconds_ebx);

	// The 8 loadouts from the data (the mode's kits, KitDef::default_unlock), cross-checked with the documented table.
	bf3::core::Result<std::vector<bf3::render::ViewmodelKit>> read_kits =
			bf3::render::read_viewmodel_kits(*(*sources)->ebx, bf3::render::viewmodel_level_id(level));
	if (!read_kits) {
		std::fprintf(stderr, "vm clips: %s\n", read_kits.error().message.c_str());
		install_unchanged(p_ctx, before);
		return 1;
	}
	const std::vector<bf3::render::ViewmodelKit> &kits = *read_kits;
	for (const bf3::render::ViewmodelKit &k : kits) {
		std::printf("kit\t%s\t%s\t%s\t%s\t%s\t%s\n", k.team.c_str(), k.kit.c_str(), k.unlocks[0].c_str(), k.unlocks[1].c_str(), k.unlocks[2].c_str(),
				k.unlocks[3].c_str());
	}
	// The documented table is Metro's (another --level only prints the difference).
	const bool documented_level = bf3::render::viewmodel_level_id(level) == "MP_Subway";
	const std::string kits_diff = bf3::render::viewmodel_kits_difference(kits, bf3::render::documented_viewmodel_kits());
	std::printf("kits: %zu loadouts read from %s %s; %s\n", kits.size(), bf3::render::viewmodel_level_id(level).c_str(),
			std::string(bf3::render::kViewmodelKitsMode).c_str(),
			kits_diff.empty() ? "equal to the documented table (m2a.md 5.6)" : ("DIFFER from the documented table: " + kits_diff).c_str());

	std::vector<ViewmodelSetSpec> specs;
	if (!unlocks.empty()) {
		ViewmodelSetSpec spec = bf3::render::team_viewmodel_set(kits, team == "ru" ? 2 : 1);
		spec.items.clear();
		for (const std::string &u : unlocks) {
			bf3::render::ViewmodelItemSpec item;
			item.unlock = u;
			spec.items.push_back(item);
		}
		specs.push_back(spec);
	} else {
		if (team != "ru") {
			specs.push_back(bf3::render::team_viewmodel_set(kits, 1));
		}
		if (team != "us") {
			specs.push_back(bf3::render::team_viewmodel_set(kits, 2));
		}
	}
	std::map<std::string, const ViewmodelItem *> by_unlock;
	std::vector<std::unique_ptr<ViewmodelSet>> sets;
	bool ok = true;
	const bool skin = p_args[0] == "skin";
	uint32_t stretched = 0, skinned_items = 0;
	for (ViewmodelSetSpec &spec : specs) {
		spec.superbundle = level;
		spec.clips_only = !skin;
		bf3::core::Result<std::unique_ptr<ViewmodelSet>> set = bf3::render::load_viewmodel_set(**sources, spec, nullptr);
		if (!set) {
			std::printf("vm clips: FAIL %s\n", set.error().message.c_str());
			ok = false;
			continue;
		}
		for (const std::string &n : (*set)->notes) {
			std::printf("note\t%s\n", n.c_str());
		}
		for (const ViewmodelItem &item : (*set)->items) {
			if (skin) {
				stretched += skin_item(**set, item, bf3::anim::ViewmodelTuning{});
				++skinned_items;
				continue;
			}
			print_item(item);
			by_unlock.emplace(item.unlock, &item);
		}
		sets.push_back(std::move(*set));
	}

	if (skin) {
		std::printf("VM SKIN: %s (%u items, %u stretched surfaces)\n", stretched == 0 && ok ? "PASS" : "FAIL", skinned_items, stretched);
		const bool clean = install_unchanged(p_ctx, before);
		return ok && stretched == 0 && clean ? 0 : 1;
	}

	// E5: the authored reload clip lengths next to ReloadTime (§16 Q4: the clip plays at its authored rate).
	for (const auto &[unlock, item] : by_unlock) {
		if (item->kind != bf3::render::ViewmodelItemKind::Gun) {
			continue;
		}
		const ViewmodelClip &tac = item->clips[size_t(ViewmodelClipRole::Reload)];
		const ViewmodelClip &emp = item->clips[size_t(ViewmodelClipRole::ReloadEmpty)];
		auto len = [](const ViewmodelClip &p_c) {
			char b[64];
			if (p_c.source == ViewmodelClipSource::Clip) {
				std::snprintf(b, sizeof(b), "%.3f s", double(p_c.seconds));
			} else {
				std::snprintf(b, sizeof(b), "%s", bf3::render::viewmodel_clip_source_name(p_c.source));
			}
			return std::string(b);
		};
		std::printf("E5 %-26s tactical: ReloadTimeBulletsLeft %.3f s, clip %s (%+.3f s) | empty: ReloadTime %.3f s, clip %s (%+.3f s) | DeployTime %.3f s, "
					"deploy clip %s\n",
				unlock.c_str(), double(item->reload_time_bullets_left), len(tac).c_str(),
				tac.source == ViewmodelClipSource::Clip ? double(tac.seconds - item->reload_time_bullets_left) : 0.0, double(item->reload_time),
				len(emp).c_str(), emp.source == ViewmodelClipSource::Clip ? double(emp.seconds - item->reload_time) : 0.0, double(item->deploy_time),
				len(item->clips[size_t(ViewmodelClipRole::Deploy)]).c_str());
	}

	// E11: deploy, fire and reload of every item of the 8 loadouts.
	if (unlocks.empty() && team == "all") {
		uint32_t covered = 0, total = 0, clips = 0, stand_ins = 0, na = 0;
		for (const bf3::render::ViewmodelKit &kit : kits) {
			for (const std::string &u : kit.unlocks) {
				++total;
				const auto it = by_unlock.find(u);
				if (it == by_unlock.end()) {
					std::printf("E11 %s %s %s: FAIL (not loaded)\n", kit.team.c_str(), kit.kit.c_str(), u.c_str());
					continue;
				}
				const bf3::render::ViewmodelCoverage c = bf3::render::viewmodel_coverage(*it->second);
				std::printf("E11 %s %-24s %-26s %s:", kit.team.c_str(), kit.kit.c_str(), u.c_str(), c.covered ? "PASS" : "FAIL");
				for (const bf3::render::ViewmodelCoverageEntry &e : c.entries) {
					std::printf("  %s=%s", e.what,
							!e.applicable ? "n/a" : (e.source == ViewmodelClipSource::Clip ? "clip" : bf3::render::viewmodel_clip_source_name(e.source)));
					if (e.applicable && !e.name.empty()) {
						std::printf(" (%s %s)", bf3::render::viewmodel_clip_role_name(e.role), e.name.c_str());
					}
				}
				std::printf("\n");
				covered += c.covered ? 1 : 0;
				clips += c.clips;
				stand_ins += c.stand_ins;
				na += c.not_applicable;
			}
		}
		const bool all_items = kits.size() == 2 * bf3::render::kViewmodelKitsPerTeam && total == kits.size() * 4;
		std::printf("E11 VIEWMODEL COVERAGE: %s (%u/%u items of %zu loadouts read from the data; %u clips, %u counted stand-ins, %u not applicable)\n",
				covered == total && all_items ? "PASS" : "FAIL", covered, total, kits.size(), clips, stand_ins, na);
		ok = ok && covered == total && all_items;
	}
	std::fprintf(stderr, "vm clips: %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	const bool clean = install_unchanged(p_ctx, before);
	return ok && (kits_diff.empty() || !documented_level) && clean ? 0 : 1;
}

} // namespace bf3dump

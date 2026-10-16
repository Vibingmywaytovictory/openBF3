// bf3dump M2a vm commands (docs/design/m2a-vm.md): `vm clips`. See vm_commands.h.
#include "vm_commands.h"

#include "bf3/core/install_snapshot.h"
#include "bf3/render/viewmodel_set.h"

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

} // namespace

const char *vm_usage() {
	return "  vm clips [<unlock>...] [--team us|ru|all] [--level <sb>]   1p viewmodel clip roles, E5 reload rows, E11 coverage\n";
}

int cmd_vm(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	if (p_args.empty() || p_args[0] != "clips") {
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

	std::vector<ViewmodelSetSpec> specs;
	if (!unlocks.empty()) {
		ViewmodelSetSpec spec = bf3::render::team_viewmodel_set(team == "ru" ? 2 : 1);
		spec.items.clear();
		for (const std::string &u : unlocks) {
			bf3::render::ViewmodelItemSpec item;
			item.unlock = u;
			spec.items.push_back(item);
		}
		specs.push_back(spec);
	} else {
		if (team != "ru") {
			specs.push_back(bf3::render::team_viewmodel_set(1));
		}
		if (team != "us") {
			specs.push_back(bf3::render::team_viewmodel_set(2));
		}
	}
	std::map<std::string, const ViewmodelItem *> by_unlock;
	std::vector<std::unique_ptr<ViewmodelSet>> sets;
	bool ok = true;
	for (ViewmodelSetSpec &spec : specs) {
		spec.superbundle = level;
		spec.clips_only = true;
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
			print_item(item);
			by_unlock.emplace(item.unlock, &item);
		}
		sets.push_back(std::move(*set));
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
		for (const bf3::render::ViewmodelKit &kit : bf3::render::default_viewmodel_kits()) {
			for (const char *u : kit.unlocks) {
				++total;
				const auto it = by_unlock.find(u);
				if (it == by_unlock.end()) {
					std::printf("E11 %s %s %s: FAIL (not loaded)\n", kit.team, kit.kit, u);
					continue;
				}
				const bf3::render::ViewmodelCoverage c = bf3::render::viewmodel_coverage(*it->second);
				std::printf("E11 %s %-24s %-26s %s:", kit.team, kit.kit, u, c.covered ? "PASS" : "FAIL");
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
		std::printf("E11 VIEWMODEL COVERAGE: %s (%u/%u items; %u clips, %u counted stand-ins, %u not applicable)\n", covered == total ? "PASS" : "FAIL",
				covered, total, clips, stand_ins, na);
		ok = ok && covered == total;
	}
	std::fprintf(stderr, "vm clips: %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	const bool clean = install_unchanged(p_ctx, before);
	return ok && clean ? 0 : 1;
}

} // namespace bf3dump

// Bf3M2aViewmodelDemo (docs/design/m2a-vm.md). See m2a_viewmodel_demo.h.
#include "m2a_viewmodel_demo.h"

#include "godot_cache.h"
#include "godot_util.h"
#include "viewmodel.h"

#include "bf3/core/install.h"
#include "bf3/sim/weapons/weapon_install.h"

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace bf3::godot_side {

using namespace godot;
namespace sw = bf3::sim::weapons;

namespace {

constexpr float kShotsDt = 1.0f / 60.0f;

std::string kit_short(const bf3::render::ViewmodelKit &p_kit) {
	std::string k = p_kit.kit;
	return k.substr(k.rfind('/') + 1);
}

std::string item_short(const std::string &p_unlock) {
	std::string u = p_unlock.substr(p_unlock.rfind('/') + 1);
	if (u.rfind("u_", 0) == 0) {
		u = u.substr(2);
	}
	return u;
}

} // namespace

Bf3M2aViewmodelDemo::Bf3M2aViewmodelDemo() = default;

Bf3M2aViewmodelDemo::~Bf3M2aViewmodelDemo() {
	join_loader();
}

void Bf3M2aViewmodelDemo::_bind_methods() {}

void Bf3M2aViewmodelDemo::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		join_loader();
	}
}

void Bf3M2aViewmodelDemo::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

Bf3Viewmodel *Bf3M2aViewmodelDemo::viewmodel(int p_team) const {
	return p_team == 2 ? vm_ru_ : vm_us_;
}

void Bf3M2aViewmodelDemo::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	bf3::core::InstallLocateOptions opts;
	opts.cmdline_path = user_arg_value("bf3-install");
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps->has_setting("openbf3/install/path")) {
		opts.setting_path = to_std(String(ps->get_setting("openbf3/install/path")));
	}
	install_.locate(opts);
	if (install_.is_mounted()) {
		install_before_ = install_.snapshot();
	}
	UtilityFunctions::print(to_godot("[vm] engine " + engine_summary()));
	UtilityFunctions::print(to_godot("[vm] install " + install_.describe()));
	check_mode_ = has_user_arg("vm-check");
	knife_dog_tags_ = has_user_arg("vm-dogtags");
	shots_dir_ = user_arg_value("vm-shots");
	const std::string quit_after = user_arg_value("quit-after");
	quit_after_ = quit_after.empty() ? 0.0 : std::atof(quit_after.c_str());
	{
		std::stringstream ss(user_arg_value("kits"));
		std::string item;
		while (std::getline(ss, item, ',')) {
			if (!item.empty()) {
				kit_filter_.push_back(size_t(std::atoi(item.c_str())));
			}
		}
	}
	// The tuning (res://config/viewmodel_tuning.cfg; FileAccess is main-thread work).
	tuning_source_ = "res://config/viewmodel_tuning.cfg";
	if (FileAccess::file_exists(to_godot(tuning_source_))) {
		const std::string text = to_std(FileAccess::get_file_as_string(to_godot(tuning_source_)));
		bf3::core::Result<bf3::anim::ViewmodelTuning> t = bf3::anim::parse_viewmodel_tuning(text);
		if (t) {
			tuning_ = *t;
		} else {
			UtilityFunctions::push_error(to_godot("[vm] " + t.error().message + ": built-in defaults"));
			tuning_source_ += " (invalid: built-in defaults)";
			failed_ = check_mode_;
		}
	} else {
		tuning_source_ = "(missing: built-in defaults)";
	}
	UtilityFunctions::print(to_godot("[vm] tuning " + tuning_source_));

	CanvasLayer *ui = memnew(CanvasLayer);
	ui->set_name("VmUI");
	label_ = memnew(Label);
	label_->set_position(Vector2(12, 8));
	label_->add_theme_color_override("font_color", Color(1, 1, 1));
	label_->add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.9f));
	label_->add_theme_constant_override("shadow_offset_x", 1);
	label_->add_theme_constant_override("shadow_offset_y", 1);
	label_->add_theme_font_size_override("font_size", 14);
	ui->add_child(label_);
	add_child(ui);

	if (!has_user_arg("no-cache")) {
		const std::string root = user_arg_value("bf3-cache");
		cache_root_ = root.empty() ? default_cache_root() : std::filesystem::path(std::u8string(root.begin(), root.end()));
	}
	if (!install_.is_mounted()) {
		load_error_ = "no usable install: " + install_.describe();
		load_done_ = true;
	} else {
		cache_guard_ = install_.write_guard();
		load_start_usec_ = Time::get_singleton()->get_ticks_usec();
		// Heavy work off the main thread (plan §2.3): no Godot API on this thread.
		loader_ = std::thread([this]() {
			std::shared_ptr<bf3::core::Cache> cache;
			if (!cache_root_.empty()) {
				progress_.set("opening the derived-data cache", 0);
				AcquiredCache acquired = acquire_cache(cache_root_, cache_guard_);
				if (acquired.status == bf3::core::CacheStatus::Ok) {
					cache = acquired.cache;
				} else {
					load_notes_.push_back(std::string("cache not used: ") + bf3::core::cache_status_name(acquired.status) + " " + acquired.detail);
				}
			}
			const std::string level = "Win32/Levels/MP_Subway/MP_Subway";
			bf3::core::Result<std::unique_ptr<bf3::render::ViewmodelSources>> sources = bf3::render::open_viewmodel_sources(install_, level, &progress_);
			if (!sources) {
				load_error_ = sources.error().message;
				load_done_ = true;
				return;
			}
			sources_ = std::move(*sources);
			// The 8 loadouts from the data (E11's item set), cross-checked with the documented table.
			bf3::core::Result<std::vector<bf3::render::ViewmodelKit>> kit_defs =
					bf3::render::read_viewmodel_kits(*sources_->ebx, bf3::render::viewmodel_level_id(level));
			if (!kit_defs) {
				load_error_ = kit_defs.error().message;
				load_done_ = true;
				return;
			}
			kit_defs_ = std::move(*kit_defs);
			kit_defs_difference_ = bf3::render::viewmodel_kits_difference(kit_defs_, bf3::render::documented_viewmodel_kits());
			for (const int team : { 1, 2 }) {
				bf3::render::ViewmodelSetSpec spec = bf3::render::team_viewmodel_set(kit_defs_, team);
				spec.knife_dog_tags = knife_dog_tags_;
				spec.superbundle = level;
				bf3::core::Result<std::unique_ptr<bf3::render::ViewmodelSet>> set = bf3::render::load_viewmodel_set(*sources_, spec, &progress_, cache.get());
				if (!set) {
					load_error_ = std::string(team == 1 ? "US" : "RU") + " set: " + set.error().message;
					load_done_ = true;
					return;
				}
				(team == 1 ? set_us_ : set_ru_) = std::move(*set);
			}
			cache.reset();
			// The weapon table of the kits' variants, and every kit's scripted run.
			progress_.set("weapon table and scripted timelines", 7);
			bf3::core::Result<sw::InstallWeapons> weapons = sw::open_install_weapons(sources_->install, level);
			if (!weapons) {
				load_error_ = "weapons: " + weapons.error().message;
				load_done_ = true;
				return;
			}
			std::vector<std::string> variants;
			for (const bf3::render::ViewmodelKit &k : kit_defs_) {
				for (const std::string &u : k.unlocks) {
					if (std::find(variants.begin(), variants.end(), u) == variants.end()) {
						variants.push_back(u);
					}
				}
			}
			std::vector<std::string_view> views(variants.begin(), variants.end());
			bf3::core::Result<std::shared_ptr<const sw::WeaponTable>> table = sw::install_weapon_table(*weapons, views, sw::install_table_options(*weapons));
			if (!table) {
				load_error_ = "weapon table: " + table.error().message;
				load_done_ = true;
				return;
			}
			table_ = *table;
			const std::vector<bf3::render::ViewmodelKit> &kits = kit_defs_;
			for (size_t k = 0; k < kits.size(); ++k) {
				if (!kit_filter_.empty() && std::find(kit_filter_.begin(), kit_filter_.end(), k) == kit_filter_.end()) {
					continue;
				}
				Kit kit;
				kit.index = k;
				kit.team = kits[k].team == "RU" ? 2 : 1;
				const bf3::render::ViewmodelSet &set = kit.team == 2 ? *set_ru_ : *set_us_;
				std::array<uint16_t, vm_script::kKitItems> defs{};
				for (size_t s = 0; s < vm_script::kKitItems; ++s) {
					const std::string u = kits[k].unlocks[s];
					defs[s] = uint16_t(std::find(variants.begin(), variants.end(), u) - variants.begin());
					kit.item[s] = -1;
					for (size_t i = 0; i < set.items.size(); ++i) {
						if (set.items[i].unlock == u) {
							kit.item[s] = int(i);
						}
					}
				}
				kit.run = vm_script::run_kit(*table_, k, defs);
				if (!kit.run.error.empty()) {
					load_error_ = std::string("kit ") + kits[k].kit + ": " + kit.run.error;
					load_done_ = true;
					return;
				}
				kits_.push_back(std::move(kit));
			}
			load_done_ = true;
		});
	}
	set_process(true);
}

void Bf3M2aViewmodelDemo::build_props() {
	// A floor and boxes ahead of the eye (sim +Z = Godot +Z here: the camera looks along the sim view).
	Ref<StandardMaterial3D> floor_mat;
	floor_mat.instantiate();
	floor_mat->set_albedo(Color(0.36f, 0.37f, 0.34f));
	Ref<PlaneMesh> plane;
	plane.instantiate();
	plane->set_size(Vector2(80, 80));
	plane->set_material(floor_mat);
	MeshInstance3D *floor = memnew(MeshInstance3D);
	floor->set_name("Floor");
	floor->set_mesh(plane);
	add_child(floor);
	const float spots[][3] = { { 3.0f, -1.6f, 1.2f }, { 4.5f, 1.8f, 2.0f }, { 7.0f, -0.4f, 3.0f }, { 10.0f, 3.0f, 1.5f }, { 2.2f, 2.2f, 0.8f },
		{ 14.0f, -3.0f, 2.5f } };
	for (int i = 0; i < 6; ++i) {
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color::from_hsv(0.08f + 0.15f * float(i), 0.45f, 0.75f));
		Ref<BoxMesh> box;
		box.instantiate();
		box->set_size(Vector3(0.8f, spots[i][2], 0.8f));
		box->set_material(m);
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name(to_godot("RefBox" + std::to_string(i)));
		mi->set_mesh(box);
		add_child(mi);
		// Sim yaw 0 looks down +Z, + yaw = left (+X).
		mi->set_global_position(Vector3(spots[i][1], spots[i][2] * 0.5f, spots[i][0]));
	}
}

void Bf3M2aViewmodelDemo::build_scene() {
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	for (const std::string &n : load_notes_) {
		UtilityFunctions::print(to_godot("[vm] " + n));
	}
	for (const bf3::render::ViewmodelSet *set : { set_us_.get(), set_ru_.get() }) {
		for (const std::string &n : set->notes) {
			if (n.rfind("texture ", 0) != 0 && n.rfind("mesh ", 0) != 0) {
				UtilityFunctions::print(to_godot("[vm] " + n));
			}
		}
		for (const bf3::render::ViewmodelItem &it : set->items) {
			if (has_user_arg("vm-verbose")) {
				// Every drawn surface of the item's weapon mesh: material, shader, size, extent (look debugging).
				for (const bf3::render::PreparedSurface &sf : it.weapon.prepared.surfaces) {
					const std::string shader = sf.material_index < it.weapon.materials.slots.size() ? it.weapon.materials.slots[sf.material_index].shader : "?";
					char b[512];
					std::snprintf(b, sizeof(b), "[vm] %s surface %zu material %s shader %s: %zu vertices, %zu triangles, aabb (%.3f %.3f %.3f)..(%.3f %.3f %.3f)",
							it.unlock.c_str(), sf.subset, sf.material_name.c_str(), shader.c_str(), sf.vertex_count(), sf.indices.size() / 3, double(sf.aabb_min[0]),
							double(sf.aabb_min[1]), double(sf.aabb_min[2]), double(sf.aabb_max[0]), double(sf.aabb_max[1]), double(sf.aabb_max[2]));
					UtilityFunctions::print(b);
				}
			}
			std::string l;
			for (const bf3::render::ViewmodelAttachment &a : it.attachments) {
				l += " " + a.part.mesh.substr(a.part.mesh.rfind('/') + 1) + (a.hip && a.zoom ? "" : a.hip ? " (hip)" : " (aimed)") + " on " + a.bone;
			}
			if (!l.empty()) {
				UtilityFunctions::print(to_godot("[vm] " + it.unlock + ": accessories (" + it.attachments.front().socket_unlock + "):" + l));
			}
		}
	}
	camera_ = memnew(Camera3D);
	camera_->set_name("ViewCamera");
	camera_->set_near(0.05f);
	camera_->set_far(500.0f);
	add_child(camera_);
	camera_->make_current();
	vm_us_ = memnew(Bf3Viewmodel);
	vm_us_->set_name("ViewmodelUS");
	add_child(vm_us_);
	vm_ru_ = memnew(Bf3Viewmodel);
	vm_ru_->set_name("ViewmodelRU");
	add_child(vm_ru_);
	if (!vm_us_->build(*set_us_, tuning_) || !vm_ru_->build(*set_ru_, tuning_)) {
		load_error_ = "a viewmodel set has no items or no CameraJoint";
	}
	build_props();
	built_ = load_error_.empty();
	char line[256];
	std::snprintf(line, sizeof(line), "[vm] built in %.3f s: US %zu items, %zu textures; RU %zu items, %zu textures; %zu kits scripted",
			double(Time::get_singleton()->get_ticks_usec() - t0) / 1e6, set_us_->items.size(), set_us_->textures.size(), set_ru_->items.size(),
			set_ru_->textures.size(), kits_.size());
	UtilityFunctions::print(line);
}

void Bf3M2aViewmodelDemo::set_tuning(const bf3::anim::ViewmodelTuning &p_tuning) {
	tuning_ = p_tuning;
	if (vm_us_) {
		vm_us_->set_tuning(p_tuning);
	}
	if (vm_ru_) {
		vm_ru_->set_tuning(p_tuning);
	}
}

void Bf3M2aViewmodelDemo::present_kit(size_t p_kit, double p_prev_t, double p_t, float p_dt) {
	const Kit &kit = kits_[p_kit];
	Bf3Viewmodel *vm = viewmodel(kit.team);
	Bf3Viewmodel *other = viewmodel(kit.team == 2 ? 1 : 2);
	if (shown_team_ != kit.team) {
		other->select_item(-1);
		shown_team_ = kit.team;
	}
	last_view_ = vm_script::view_at(kit.run, *table_, p_t);
	const uint8_t slot = last_view_.weapon.active_slot;
	const int item = slot < vm_script::kKitItems ? kit.item[slot] : -1;
	if (item != vm->item()) {
		vm->select_item(item);
	}
	std::vector<bf3::host::PresentEvent> events;
	vm_script::events_between(kit.run, p_prev_t, p_t, events);
	std::vector<bf3::anim::VmCue> cues;
	viewmodel_cues_from(events, last_view_.weapon.magazine, cues);
	const ViewmodelFrame frame = viewmodel_frame_from(last_view_.weapon, Vector3(0, 1.6f, 0), last_view_.yaw, last_view_.pitch, p_dt, last_view_.sprinting,
			last_view_.speed, last_view_.yaw_rate, last_view_.pitch_rate);
	vm->present(frame, cues, camera_);
}

void Bf3M2aViewmodelDemo::update_label(const std::string &p_extra) {
	if (label_ == nullptr) {
		return;
	}
	char buf[768];
	if (!built_) {
		if (!load_error_.empty()) {
			label_->set_text(to_godot("openBF3 viewmodel: load failed: " + load_error_));
			return;
		}
		std::snprintf(buf, sizeof(buf), "openBF3 viewmodel: loading (%d/%d) %s...", progress_.step(), bf3::render::LoadProgress::kSteps,
				progress_.stage().c_str());
		label_->set_text(to_godot(buf));
		return;
	}
	const Kit &kit = kits_[std::min(kit_cursor_, kits_.size() - 1)];
	const Bf3Viewmodel *vm = viewmodel(kit.team);
	const bf3::render::ViewmodelItem *item = vm->current_item();
	const bf3::anim::ViewmodelOutput &o = vm->output();
	std::snprintf(buf, sizeof(buf),
			"openBF3 vm | %s %s | %s | t %.2f s | phase %u mag %d | FOV world %.1f vm %.1f | ADS %.2f | base %s action %s %.2f s w %.2f%s | %.0f fps",
			kit_defs_[kit.index].team.c_str(), kit_short(kit_defs_[kit.index]).c_str(),
			item ? item_short(item->unlock).c_str() : "-", script_t_ - kit.run.start_s(), unsigned(last_view_.weapon.phase), last_view_.weapon.magazine,
			double(vm->ads().world_fov), double(vm->ads().viewmodel_fov), double(vm->ads().blend), bf3::anim::vm_clip_name(bf3::anim::VmClip(o.base_clip)),
			bf3::anim::vm_clip_name(bf3::anim::VmClip(o.action_clip)), double(o.action_time_s), double(o.action_weight), o.action_procedural ? " (stand-in)" : "",
			Engine::get_singleton()->get_frames_per_second());
	label_->set_text(to_godot(std::string(buf) + p_extra));
}

bool Bf3M2aViewmodelDemo::save_png(const std::string &p_path) {
	const std::filesystem::path target = std::filesystem::path(std::u8string(p_path.begin(), p_path.end()));
	const bf3::core::PathGuard guard = install_.write_guard();
	if (!guard.allows_write(target)) {
		UtilityFunctions::push_error(to_godot("[vm] screenshot refused (" + std::string(bf3::core::PathGuard::verdict_name(guard.check_write_target(target))) +
				"): " + p_path));
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);
	Ref<Image> img = get_viewport()->get_texture()->get_image();
	if (img.is_valid() && img->save_png(to_godot(p_path)) == OK) {
		UtilityFunctions::print(to_godot("[vm] screenshot " + p_path + " (" + std::to_string(img->get_width()) + "x" + std::to_string(img->get_height()) + ")"));
		return true;
	}
	UtilityFunctions::push_error(to_godot("[vm] screenshot failed: " + p_path));
	return false;
}

void Bf3M2aViewmodelDemo::begin_kit(size_t p_kit) {
	kit_cursor_ = p_kit;
	const Kit &kit = kits_[p_kit];
	script_t_ = kit.run.start_s();
	captures_.clear();
	next_capture_ = 0;
	if (shots_dir_.empty()) {
		return;
	}
	const bf3::render::ViewmodelKit &k = kit_defs_[kit.index];
	const bf3::render::ViewmodelSet &s = set(kit.team);
	auto add = [&](uint8_t p_slot, const char *p_label, double p_at) {
		if (kit.item[p_slot] < 0) {
			return;
		}
		const std::string name = std::string("vm_") + k.team + "_" + kit_short(k) + "_" + item_short(s.items[size_t(kit.item[p_slot])].unlock) + "_" + p_label;
		captures_.push_back({ p_at, name });
	};
	auto clip_mid = [&](uint8_t p_slot, bf3::render::ViewmodelClipRole p_role) {
		const bf3::render::ViewmodelItem &it = s.items[size_t(kit.item[p_slot])];
		const bf3::render::ViewmodelClip &c = it.clips[size_t(p_role)];
		return 0.5 * double(c.seconds > 0 ? c.seconds : it.reload_time_bullets_left);
	};
	const bool full = kit.index == 0 || kit.index == 4; // assault kits: every item
	for (uint8_t slot = 0; slot < (full ? 4 : 1); ++slot) {
		if (const vm_script::Mark *m = kit.run.mark(slot, "hip")) {
			add(slot, "hip", m->time_s);
		}
		if (const vm_script::Mark *m = kit.run.mark(slot, "ads")) {
			add(slot, "ads", m->time_s);
		}
		if (const vm_script::Mark *m = kit.run.mark(slot, "reload_start")) {
			add(slot, "reload_mid", m->time_s + clip_mid(slot, bf3::render::ViewmodelClipRole::Reload));
		}
		if (const vm_script::Mark *m = kit.run.mark(slot, "throw")) {
			add(slot, "throw", m->time_s + 0.1);
		}
		if (const vm_script::Mark *m = kit.run.mark(slot, "knife")) {
			add(slot, "knife", m->time_s);
		}
	}
	std::sort(captures_.begin(), captures_.end(), [](const Capture &p_a, const Capture &p_b) { return p_a.time_s < p_b.time_s; });
}

void Bf3M2aViewmodelDemo::shots_step() {
	if (hold_frames_ > 0) {
		present_kit(kit_cursor_, script_t_, script_t_, 0.0f);
		update_label();
		if (--hold_frames_ == 0) {
			const std::string path = shots_dir_ + "/" + captures_[next_capture_].name + ".png";
			if (save_png(path)) {
				++shots_saved_;
			} else {
				failed_ = true;
			}
			++next_capture_;
		}
		return;
	}
	double next_t = script_t_ + kShotsDt;
	if (next_capture_ < captures_.size() && next_t >= captures_[next_capture_].time_s) {
		next_t = std::max(script_t_, captures_[next_capture_].time_s);
		hold_frames_ = 4;
	}
	present_kit(kit_cursor_, script_t_, next_t, float(next_t - script_t_));
	script_t_ = next_t;
	update_label();
	if (hold_frames_ == 0 && next_capture_ >= captures_.size()) {
		// The rest of this kit is not captured: on to the next.
		if (kit_cursor_ + 1 < kits_.size()) {
			begin_kit(kit_cursor_ + 1);
		} else {
			UtilityFunctions::print(to_godot("[vm] " + std::to_string(shots_saved_) + " captures saved to " + shots_dir_));
			finish(failed_ ? 1 : 0);
		}
	}
}

void Bf3M2aViewmodelDemo::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || quitting_) {
		return;
	}
	if (!built_) {
		if (load_done_) {
			join_loader();
			if (load_error_.empty()) {
				load_seconds_ = double(Time::get_singleton()->get_ticks_usec() - load_start_usec_) / 1e6;
				UtilityFunctions::print(to_godot("[vm] loaded in " + std::to_string(load_seconds_) + " s"));
				build_scene();
			}
			if (!load_error_.empty() || kits_.empty()) {
				UtilityFunctions::push_error(to_godot("[vm] load failed: " + (load_error_.empty() ? std::string("no kits") : load_error_)));
				update_label();
				finish(1); // "no usable install" is a skip for ctest (godot_m2a_viewmodel.cmake matches the text)
				return;
			}
			if (check_mode_) {
				finish(failed_ ? 1 : run_m2a_viewmodel_check(*this));
				return;
			}
			begin_kit(0);
		}
		update_label();
		return;
	}
	if (!shots_dir_.empty()) {
		shots_step();
		return;
	}
	// Play: the kits in a loop at the display's frame time.
	const float dt = float(std::min(p_delta, 0.1));
	const Kit &kit = kits_[kit_cursor_];
	double next_t = script_t_ + dt;
	if (next_t > kit.run.end_s()) {
		begin_kit((kit_cursor_ + 1) % kits_.size());
		next_t = script_t_ + dt;
	}
	present_kit(kit_cursor_, script_t_, next_t, dt);
	script_t_ = next_t;
	update_label();
	played_ += p_delta;
	if (quit_after_ > 0 && played_ >= quit_after_) {
		finish(failed_ ? 1 : 0);
	}
}

void Bf3M2aViewmodelDemo::finish(int p_code) {
	quitting_ = true;
	if (install_before_.valid) {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(install_before_, install_.snapshot());
		UtilityFunctions::print(to_godot(std::string("[vm] install.unchanged ") + (d.clean() ? "yes (" + std::to_string(install_before_.files.size()) +
																							   " files: sizes and mtimes equal)"
																						 : "NO: " + d.summary())));
		if (!d.clean()) {
			p_code = 1;
		}
	}
	join_loader();
	get_tree()->quit(p_code);
}

} // namespace bf3::godot_side

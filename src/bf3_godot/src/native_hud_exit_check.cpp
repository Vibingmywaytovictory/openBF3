#include "native_hud_exit_check.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "native_hud.h"
#include "swf_view.h"

#include "bf3/core/install_snapshot.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace bf3::godot_side {

void Bf3NativeHudExitCheck::_bind_methods() {}

void Bf3NativeHudExitCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_process(true);
}

void Bf3NativeHudExitCheck::add(std::string p_name, bool p_pass, std::string p_detail) {
	rows_.push_back(Row{ std::move(p_name), p_pass, std::move(p_detail) });
}

Bf3NativeHud *Bf3NativeHudExitCheck::open_hud() {
	Bf3NativeHud *hud = memnew(Bf3NativeHud);
	hud->set_name("NativeHud");
	add_child(hud);
	return hud;
}

void Bf3NativeHudExitCheck::free_hud() {
	const uint64_t before = Bf3SwfView::counters().scenes_freed_while_driven;
	remove_child(hud_);
	memdelete(hud_);
	hud_ = nullptr;
	const uint64_t freed = Bf3SwfView::counters().scenes_freed_while_driven - before;
	add("free.no_driven_scene", freed == 0,
			bf3::core::fmt("%llu views freed while a HudScreen still drove their scene", (unsigned long long)freed));
}

void Bf3NativeHudExitCheck::finish() {
	Bf3Localisation *loc = Bf3Localisation::current();
	if (loc == nullptr || !loc->install_snapshot_at_start().valid) {
		add("install.unchanged", false, "no snapshot at start");
	} else {
		const bf3::core::SnapshotDiff d =
				bf3::core::diff_snapshots(loc->install_snapshot_at_start(), loc->reader().snapshot());
		add("install.unchanged", d.clean(), d.clean() ? "sizes and mtimes equal" : d.summary());
	}
	bool ok = !rows_.empty();
	for (const Row &r : rows_) {
		ok = ok && r.pass;
		UtilityFunctions::print(to_godot(
				bf3::core::fmt("  %-4s %-28s %s", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str())));
	}
	UtilityFunctions::print(ok ? "NATIVE HUD EXIT CHECK: PASS" : "NATIVE HUD EXIT CHECK: FAIL");
	set_process(false);
	get_tree()->quit(ok ? 0 : 1);
}

void Bf3NativeHudExitCheck::_process(double) {
	if (phase_ == 0) {
		Bf3Localisation *loc = Bf3Localisation::current();
		if (loc == nullptr || loc->has_failed() || (loc->is_loaded() && !loc->install())) {
			UtilityFunctions::print(to_godot("NATIVE HUD EXIT CHECK: no usable install (" +
					(loc != nullptr ? to_std(loc->get_error()) : std::string("no Bf3Localisation")) + ")"));
			set_process(false);
			get_tree()->quit(1);
			return;
		}
		if (!loc->is_loaded()) {
			return;
		}
		hud_ = open_hud();
		phase_ = 1;
		frames_ = 0;
		return;
	}
	if (phase_ == 1 || phase_ == 2) {
		const bool built = hud_->is_ready();
		if (!built && ++frames_ < kNativeHudExitMaxWaitFrames) {
			return;
		}
		std::string detail = "four views and the commo rose";
		if (!built) {
			detail = bf3::core::fmt("not built after %u frames", unsigned(frames_));
		} else if (hud_->has_failed()) {
			detail = hud_->error();
		} else if (hud_->rose() == nullptr) {
			detail = "no commo rose: " + hud_->rose_error();
		}
		add(std::string(phase_ == 1 ? "first" : "second") + ".built",
				built && !hud_->has_failed() && hud_->rose() != nullptr, detail);
		if (phase_ == 1) {
			free_hud();
			hud_ = open_hud();
			phase_ = 2;
			frames_ = 0;
			return;
		}
		finish();
		phase_ = 3;
	}
}

}

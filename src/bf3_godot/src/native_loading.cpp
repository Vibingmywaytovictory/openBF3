#include "native_loading.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "native_screens.h"
#include "ui_assets.h"
#include "ui_textures.h"

#include "bf3/core/level/level.h"
#include "bf3/core/ui/hud_screen.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

std::string clip_text(ui::HudScreen &r_screen, std::string_view p_path) {
	const ui::As2Clip c = r_screen.clip(p_path);
	return c.exists() ? c.text() : std::string();
}

}

NativeLoading::~NativeLoading() {
	if (Object *o = layer_ != nullptr ? ObjectDB::get_instance(layer_id_) : nullptr) {
		memdelete(Object::cast_to<CanvasLayer>(o));
	}
	layer_ = nullptr;
	if (textures_ != nullptr && prepared_ != nullptr) {
		textures_->remove_mount(prepared_->mount.get());
	}
	join();
}

void NativeLoading::join() {
	if (worker_.joinable()) {
		worker_.join();
	}
}

Bf3UiScreens *NativeLoading::screens() const {
	return screens_id_ != 0 ? Object::cast_to<Bf3UiScreens>(ObjectDB::get_instance(screens_id_)) : nullptr;
}

std::string NativeLoading::movie() const {
	return worker_done_.load(std::memory_order_acquire) && prepared_ != nullptr ? prepared_->movie : std::string();
}

void NativeLoading::attach(Node *p_parent, std::string p_level, std::string p_mode, uint64_t p_seed) {
	if (layer_ != nullptr || p_parent == nullptr) {
		return;
	}
	level_ = std::move(p_level);
	mode_ = std::move(p_mode);
	seed_ = p_seed;
	layer_ = memnew(CanvasLayer);
	layer_->set_name("NativeLoading");
	layer_->set_layer(kNativeLoadingLayer);
	layer_id_ = layer_->get_instance_id();
	Bf3UiScreens *s = memnew(Bf3UiScreens);
	s->set_name("LoadingScreens");
	s->set_manual(true);
	layer_->add_child(s);
	screens_id_ = s->get_instance_id();
	p_parent->add_child(layer_);
	opened_ = false;
}

void NativeLoading::start_worker(std::shared_ptr<const bf3::core::Install> p_install) {
	worker_ = std::thread([this, install = std::move(p_install)]() {
		auto p = std::make_unique<Prepared>();
		bf3::core::Result<bf3::core::LevelMount> mount = bf3::core::level::mount_level_description(*install, level_);
		if (!mount) {
			p->error = mount.error().message;
		} else {
			bf3::core::Result<bf3::core::level::LevelInfo> info = bf3::core::level::level_info(*mount, level_);
			if (!info) {
				p->error = info.error().message;
			} else if (info->loading.movie.empty()) {
				p->error = level_ + " names no MPLoadingAssetPath";
			} else if (bf3::core::Result<std::string> mode = bf3::core::level::resolve_level_mode(info->modes, mode_);
					!mode) {
				p->error = level_ + ": " + mode.error().message;
			} else if (bf3::core::Result<std::vector<std::string>> hints =
							   bf3::core::level::level_hints(*mount, info->loading.hint_asset);
					!hints) {
				p->error = hints.error().message;
			} else {
				p->movie = ui::loading_screen_movie(info->loading.movie);
				p->source = ui::loading_screen_source(
						*info, *mode, std::move(*hints), std::string(kNativeLoadingPlatform), seed_);
				p->mount = std::make_shared<const bf3::core::LevelMount>(std::move(*mount));
			}
		}
		prepared_ = std::move(p);
		worker_done_.store(true, std::memory_order_release);
	});
}

void NativeLoading::try_open(double p_now_s, const bf3::host::LoadProgress &p_progress) {
	Bf3Localisation *loc = Bf3Localisation::current();
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (loc == nullptr || assets == nullptr || !error_.empty()) {
		return;
	}
	if (!worker_.joinable() && !worker_done_.load(std::memory_order_acquire)) {
		if (!loc->is_loaded() || loc->install() == nullptr) {
			return;
		}
		start_worker(loc->install());
	}
	if (!worker_done_.load(std::memory_order_acquire) || !assets->is_loaded()) {
		return;
	}
	join();
	Bf3UiScreens *s = screens();
	if (s == nullptr) {
		return;
	}
	if (!prepared_->error.empty()) {
		error_ = prepared_->error;
		UtilityFunctions::print(to_godot("[loading] no native loading screen: " + error_));
		return;
	}
	textures_ = Bf3UiTextures::shared();
	textures_->add_mount(prepared_->mount);
	s->set_client(ui::hud_text_client(loc->database()));
	s->set_drawn(prepared_->movie, true);
	ui::LoadingScreenSource source = prepared_->source;
	source.seed += facts_.opens;
	data_.push_back(std::make_unique<ui::LoadingScreenData>(std::move(source)));
	if (const bf3::core::Status st = s->open_movie_screen(prepared_->movie, data_.back().get()); !st) {
		error_ = st.error().message;
		UtilityFunctions::print(to_godot("[loading] " + prepared_->movie + " not opened: " + error_));
		return;
	}
	opened_ = true;
	stepped_s_ = p_now_s;
	++facts_.opens;
	facts_.open_update = facts_.updates;
	facts_.open_s = p_now_s;
	facts_.drawn_updates = 0;
	facts_.removed_update = 0;
	facts_.removed_s = -1.0;
	facts_.decode_done_at_open =
			p_progress.end_us[size_t(bf3::host::LoadStage::Meshes)].load(std::memory_order_acquire) != 0 &&
			p_progress.end_us[size_t(bf3::host::LoadStage::Textures)].load(std::memory_order_acquire) != 0;
	facts_.stage_at_open =
			bf3::host::load_stage_name(bf3::host::LoadStage(p_progress.stage.load(std::memory_order_acquire)));
}

void NativeLoading::read_facts() {
	Bf3UiScreens *s = screens();
	ui::HudScreen *hud = s != nullptr ? s->hud_screen(prepared_->movie) : nullptr;
	if (hud == nullptr) {
		return;
	}
	const std::string root(ui::kHudScreenInstance);
	facts_.header = clip_text(*hud, root + ".header");
	facts_.subheader = clip_text(*hud, root + ".subheader");
	facts_.hint = hud->clip(root + ".hint").visible() ? clip_text(*hud, root + ".hint") : std::string();
	const std::string message = ui::loading_message_path(*hud);
	facts_.message = message.empty() ? std::string() : clip_text(*hud, message + ".text.msg.txt");
	facts_.hints_served = data_.back()->hints_served();
}

void NativeLoading::update(double p_now_s, const bf3::host::LoadProgress &p_progress) {
	if (layer_ == nullptr) {
		return;
	}
	++facts_.updates;
	if (!opened_) {
		try_open(p_now_s, p_progress);
	}
	if (!opened_) {
		return;
	}
	Bf3UiScreens *s = screens();
	if (s == nullptr || s->hud_screen(prepared_->movie) == nullptr) {
		return;
	}
	if (layer_->is_visible()) {
		++facts_.drawn_updates;
	}
	if (facts_.open_update != facts_.updates && p_now_s - stepped_s_ < kNativeLoadingStepSeconds) {
		return;
	}
	s->step(std::clamp(p_now_s - stepped_s_, 0.0, kNativeLoadingMaxStepSeconds));
	stepped_s_ = p_now_s;
	read_facts();
}

void NativeLoading::remove(double p_now_s) {
	if (layer_ == nullptr) {
		return;
	}
	if (Object *o = ObjectDB::get_instance(layer_id_)) {
		CanvasLayer *layer = Object::cast_to<CanvasLayer>(o);
		if (layer->get_parent() != nullptr) {
			layer->get_parent()->remove_child(layer);
		}
		layer->queue_free();
	}
	layer_ = nullptr;
	layer_id_ = 0;
	screens_id_ = 0;
	if (textures_ != nullptr && prepared_ != nullptr) {
		textures_->remove_mount(prepared_->mount.get());
	}
	textures_.reset();
	facts_.removed_update = facts_.updates;
	facts_.removed_s = p_now_s;
	opened_ = false;
}

}

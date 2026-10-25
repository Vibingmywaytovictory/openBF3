#include "ui_textures.h"

#include "bf3_localisation.h"
#include "bf3/core/res/res_loader.h"
#include "render_bridge.h"

#include <godot_cpp/variant/utility_functions.hpp>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

std::shared_ptr<Bf3UiTextures> Bf3UiTextures::shared() {
	static std::mutex m;
	static std::weak_ptr<Bf3UiTextures> current;
	std::lock_guard lock(m);
	std::shared_ptr<Bf3UiTextures> t = current.lock();
	if (t == nullptr) {
		Bf3Localisation *loc = Bf3Localisation::current();
		t = std::make_shared<Bf3UiTextures>(loc != nullptr ? loc->install() : nullptr);
		current = t;
	}
	return t;
}

Bf3UiTextures::Bf3UiTextures(std::shared_ptr<const bf3::core::Install> p_install) :
		install_(std::move(p_install)) {}

Bf3UiTextures::~Bf3UiTextures() {
	{
		std::lock_guard lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
}

ui::SwfImageInfo Bf3UiTextures::request(std::string_view p_res_name) {
	std::unique_lock lock(mutex_);
	auto it = entries_.find(p_res_name);
	if (it == entries_.end()) {
		Entry e;
		if (install_ == nullptr) {
			e.info.status = ui::SwfImageStatus::Missing;
			e.info.error = "no install";
			++failed_;
		} else {
			queue_.emplace_back(p_res_name);
			if (!worker_.joinable()) {
				worker_ = std::thread([this]() { run(); });
			}
		}
		it = entries_.emplace(std::string(p_res_name), std::move(e)).first;
		lock.unlock();
		wake_.notify_one();
		lock.lock();
		it = entries_.find(p_res_name);
	}
	Entry &e = it->second;
	if (e.decoded && e.prepared && e.texture.is_null()) {
		Ref<ImageTexture> tex = make_texture(*e.prepared);
		e.prepared.reset();
		if (tex.is_null()) {
			e.info.status = ui::SwfImageStatus::Missing;
			e.info.error = "texture not created";
			++failed_;
		} else {
			e.texture = tex;
			e.info.status = ui::SwfImageStatus::Ready;
			e.info.width = uint32_t(tex->get_width());
			e.info.height = uint32_t(tex->get_height());
		}
	}
	return e.info;
}

Ref<Texture2D> Bf3UiTextures::texture(std::string_view p_res_name) const {
	std::lock_guard lock(mutex_);
	const auto it = entries_.find(p_res_name);
	return it != entries_.end() ? it->second.texture : Ref<Texture2D>();
}

uint32_t Bf3UiTextures::decoded() const {
	std::lock_guard lock(mutex_);
	return decoded_;
}

uint32_t Bf3UiTextures::failed() const {
	std::lock_guard lock(mutex_);
	return failed_;
}

void Bf3UiTextures::run() {
	bf3::core::Result<bf3::core::LevelMount> mount = ui::mount_swf_images(*install_);
	for (;;) {
		std::string name;
		{
			std::unique_lock lock(mutex_);
			wake_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
			if (stop_) {
				return;
			}
			name = std::move(queue_.front());
			queue_.pop_front();
		}
		std::optional<bf3::render::PreparedTexture> prepared;
		std::string error;
		if (!mount) {
			error = mount.error().message;
		} else {
			const ui::SwfImageInfo info = ui::read_swf_image_info(*mount, name);
			if (info.status != ui::SwfImageStatus::Ready) {
				error = info.error;
			} else {
				bf3::core::Result<bf3::core::res::LoadedTexture> loaded = bf3::core::res::load_texture(*mount, name);
				if (!loaded) {
					error = loaded.error().message;
				} else {
					bf3::core::Result<bf3::render::PreparedTexture> prep = bf3::render::prepare_texture(*loaded, bf3::render::TextureRole::Color);
					if (!prep) {
						error = prep.error().message;
					} else {
						prepared = std::move(*prep);
					}
				}
			}
		}
		std::lock_guard lock(mutex_);
		Entry &e = entries_[name];
		e.decoded = true;
		if (prepared) {
			e.prepared = std::move(prepared);
			++decoded_;
		} else {
			e.info.status = ui::SwfImageStatus::Missing;
			e.info.error = error;
			++failed_;
		}
	}
}

}

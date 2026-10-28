#include "hud_items.h"

#include "bf3_localisation.h"
#include "godot_util.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/ui/text_db.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

PackedByteArray byte_array(const std::vector<uint8_t> &p_data) {
	PackedByteArray out;
	out.resize(int64_t(p_data.size()));
	if (!p_data.empty()) {
		std::copy(p_data.begin(), p_data.end(), out.ptrw());
	}
	return out;
}

bool image_format(bf3::core::res::TextureFormat p_format, Image::Format &r_format) {
	using bf3::core::res::TextureFormat;
	switch (p_format) {
		case TextureFormat::Dxt1:
			r_format = Image::FORMAT_DXT1;
			return true;
		case TextureFormat::Dxt3:
			r_format = Image::FORMAT_DXT3;
			return true;
		case TextureFormat::Dxt5:
			r_format = Image::FORMAT_DXT5;
			return true;
		case TextureFormat::Argb8888:
			r_format = Image::FORMAT_RGBA8;
			return true;
		default:
			return false;
	}
}

}

struct Bf3HudItems::Loaded {
	std::unique_ptr<ui::ItemIconIndex> index;
	std::string icon_error;
	struct Icon {
		std::vector<uint32_t> items;
		ui::ItemIconImage image;
	};
	std::vector<Icon> icons;
	size_t icons_failed = 0;
	std::vector<std::string> log;
	double seconds = 0;
};

Bf3HudItems::Bf3HudItems() = default;

Bf3HudItems::~Bf3HudItems() {
	join_loader();
}

void Bf3HudItems::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_loaded"), &Bf3HudItems::is_loaded);
	ADD_SIGNAL(MethodInfo("items_loaded", PropertyInfo(Variant::BOOL, "ok")));
}

void Bf3HudItems::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_process(true);
}

void Bf3HudItems::_exit_tree() {
	join_loader();
}

void Bf3HudItems::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Bf3HudItems::_process(double p_delta) {
	(void)p_delta;
	if (finished_) {
		return;
	}
	if (!loader_.joinable()) {
		Bf3Localisation *loc = Bf3Localisation::current();
		if (loc == nullptr) {
			error_ = "no Bf3Localisation in the scene";
			finish_load();
			return;
		}
		if (!loc->is_loaded() && !loc->has_failed()) {
			return;
		}
		if (!loc->install()) {
			error_ = "no usable install (" + to_std(loc->get_error()) + ")";
			finish_load();
			return;
		}
		start_loader();
		return;
	}
	if (loader_done_.load(std::memory_order_acquire)) {
		join_loader();
		finish_load();
	}
}

void Bf3HudItems::start_loader() {
	std::shared_ptr<const bf3::core::Install> install = Bf3Localisation::current()->install();
	loaded_.reset();
	loader_done_.store(false, std::memory_order_relaxed);
	loader_ = std::thread([this, install] {
		const auto t0 = std::chrono::steady_clock::now();
		auto out = std::make_unique<Loaded>();
		bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> ebx = bf3::core::EbxLibrary::open(install, { "Win32/UI" });
		if (!ebx) {
			out->icon_error = "Win32/UI: " + ebx.error().message;
		} else {
			bf3::core::Result<ui::ItemIconIndex> items = ui::ItemIconIndex::read(**ebx);
			if (!items) {
				out->icon_error = "item index: " + items.error().message;
			} else {
				out->index = std::make_unique<ui::ItemIconIndex>(std::move(*items));
				std::map<std::string, size_t> by_res;
				for (const ui::ItemIcon &icon : out->index->icons()) {
					if ((icon.source != ui::ItemIconSource::Weapon && icon.source != ui::ItemIconSource::KitItem) ||
							icon.res_name.empty()) {
						continue;
					}
					auto it = by_res.find(icon.res_name);
					if (it != by_res.end()) {
						if (it->second != SIZE_MAX) {
							out->icons[it->second].items.push_back(icon.item_id);
						}
						continue;
					}
					bf3::core::Result<ui::ItemIconImage> img = ui::load_item_icon((*ebx)->mount(0), icon);
					if (!img) {
						by_res.emplace(icon.res_name, SIZE_MAX);
						++out->icons_failed;
						if (out->log.size() < 8) {
							out->log.push_back(icon.res_name + ": " + img.error().message);
						}
						continue;
					}
					by_res.emplace(icon.res_name, out->icons.size());
					Loaded::Icon li;
					li.items.push_back(icon.item_id);
					li.image = std::move(*img);
					out->icons.push_back(std::move(li));
				}
			}
		}
		out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		loaded_ = std::move(out);
		loader_done_.store(true, std::memory_order_release);
	});
}

void Bf3HudItems::finish_load() {
	finished_ = true;
	set_process(false);
	bool ok = error_.empty();
	if (loaded_) {
		Loaded &l = *loaded_;
		if (!l.icon_error.empty()) {
			UtilityFunctions::push_warning(to_godot("Bf3HudItems: no item icons (" + l.icon_error + ")"));
			error_ = l.icon_error;
			ok = false;
		}
		for (const std::string &m : l.log) {
			UtilityFunctions::push_warning(to_godot("Bf3HudItems: icon " + m));
		}
		icons_failed_ = l.icons_failed;
		for (Loaded::Icon &icon : l.icons) {
			Image::Format format = Image::FORMAT_RGBA8;
			Ref<ImageTexture> tex;
			if (image_format(icon.image.format, format)) {
				const Ref<Image> img = Image::create_from_data(int32_t(icon.image.width), int32_t(icon.image.height), false, format, byte_array(icon.image.data));
				if (img.is_valid() && !img->is_empty()) {
					tex = ImageTexture::create_from_image(img);
				}
			}
			if (tex.is_null()) {
				++icons_failed_;
				continue;
			}
			for (uint32_t id : icon.items) {
				icons_.emplace(id, tex);
			}
			icon.image.data.clear();
		}
		UtilityFunctions::print(to_godot("Bf3HudItems: loaded in " + std::to_string(l.seconds) + " s: " + std::to_string(icons_.size()) + " item ids with an icon, " +
				std::to_string(icons_failed_) + " icons failed"));
	} else if (!error_.empty()) {
		UtilityFunctions::push_warning(to_godot("Bf3HudItems: " + error_));
	}
	emit_signal("items_loaded", ok);
}

const ui::ItemIconIndex *Bf3HudItems::item_index() const {
	return finished_ && loaded_ ? loaded_->index.get() : nullptr;
}

Ref<Texture2D> Bf3HudItems::icon(uint32_t p_item_id) const {
	auto it = icons_.find(p_item_id);
	return it != icons_.end() ? it->second : Ref<Texture2D>();
}

const std::string &Bf3HudItems::localised(uint32_t p_sid_hash) {
	static const std::string kEmpty;
	if (p_sid_hash == 0) {
		return kEmpty;
	}
	auto it = names_.find(p_sid_hash);
	if (it != names_.end()) {
		return it->second;
	}
	Bf3Localisation *loc = Bf3Localisation::current();
	const std::shared_ptr<const ui::TextDatabase> db = loc != nullptr ? loc->database() : nullptr;
	if (db == nullptr) {
		return kEmpty;
	}
	const std::optional<std::string_view> text = db->find(p_sid_hash);
	return names_.emplace(p_sid_hash, text ? std::string(*text) : std::string()).first->second;
}

}

void openbf3_register_hud() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3HudItems);
}

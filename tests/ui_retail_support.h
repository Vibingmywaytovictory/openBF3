#pragma once

#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_image.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/ui/ui_fonts.h"
#include "test_support.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ui_retail {

struct Retail {
	bf3::test::Mounted *mount = nullptr;
	std::unique_ptr<bf3::core::ui::SwfLibrary> lib;
	std::unique_ptr<bf3::core::ui::SwfFontSet> fonts;
	std::unique_ptr<bf3::core::EbxLibrary> level;
	std::unique_ptr<bf3::core::EbxLibrary> ui;
	std::shared_ptr<bf3::core::ui::SwfMountImages> images;
	std::string error;
};

inline Retail *retail_or_skip() {
	using namespace bf3::core;
	using namespace bf3::core::ui;
	static Retail r;
	static bool tried = false;
	if (!tried) {
		tried = true;
		r.mount = bf3::test::shared_mount_or_skip();
		if (r.mount == nullptr) {
			return nullptr;
		}
		Result<SwfLibrary> lib = SwfLibrary::open(r.mount->install);
		if (!lib) {
			r.error = lib.error().message;
			return &r;
		}
		r.lib = std::make_unique<SwfLibrary>(std::move(*lib));
		Result<std::unique_ptr<EbxLibrary>> ui = EbxLibrary::open(r.mount->install, { "Win32/UI" });
		if (!ui) {
			r.error = ui.error().message;
			return &r;
		}
		r.ui = std::move(*ui);
		Result<FontMappingCollection> mapping = font_mapping_for_text_database(*r.ui, "localization/us_loc");
		if (!mapping) {
			r.error = mapping.error().message;
			return &r;
		}
		WorkBudget fb(kUiFontConvertBudgetUnits);
		Result<SwfFontSet> fonts = SwfFontSet::load(*r.lib, *mapping, fb);
		if (!fonts) {
			r.error = fonts.error().message;
			return &r;
		}
		r.fonts = std::make_unique<SwfFontSet>(std::move(*fonts));
		Result<std::unique_ptr<EbxLibrary>> level =
				EbxLibrary::open(r.mount->install, { "Win32/Levels/MP_Subway/MP_Subway" });
		if (!level) {
			r.error = level.error().message;
			return &r;
		}
		r.level = std::move(*level);
	} else if (r.mount == nullptr) {
		bf3::test::shared_mount_or_skip();
		return nullptr;
	}
	REQUIRE_MESSAGE(r.error.empty(), r.error);
	return &r;
}

inline const std::shared_ptr<bf3::core::ui::SwfMountImages> &retail_images(Retail &r_r) {
	if (r_r.images == nullptr) {
		bf3::core::Result<bf3::core::LevelMount> images = bf3::core::ui::mount_swf_images(*r_r.mount->install);
		REQUIRE_MESSAGE(images.ok(), (images.ok() ? std::string() : images.error().message));
		r_r.images = std::make_shared<bf3::core::ui::SwfMountImages>(std::move(*images));
	}
	return r_r.images;
}

inline const bf3::core::ui::TextDatabase &retail_text_db(Retail &r_r) {
	static std::unique_ptr<bf3::core::ui::TextDatabase> db;
	if (db == nullptr) {
		bf3::core::Result<bf3::core::ui::TextDatabase> t = bf3::core::ui::open_text_database(r_r.mount->install);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		db = std::make_unique<bf3::core::ui::TextDatabase>(std::move(*t));
	}
	return *db;
}

inline bf3::core::ui::HudClientState retail_text_client(
		const bf3::core::ui::TextDatabase &p_db, bf3::core::ui::HudClientState p_client = {}) {
	p_client.localise_hash = [&p_db](uint32_t p_hash) {
		const std::optional<std::string_view> t = p_db.find(p_hash);
		return t ? std::string(*t) : std::string();
	};
	p_client.translate = [&p_db](std::string_view p_sid) -> std::optional<std::string> {
		const std::optional<std::string_view> t = p_db.find_sid(p_sid);
		return t ? std::optional<std::string>(std::string(*t)) : std::nullopt;
	};
	return p_client;
}

struct RetailScreen {
	std::unique_ptr<bf3::core::ui::SwfScene> scene;
	std::unique_ptr<bf3::core::ui::HudScreen> hud;
	bf3::core::ui::HudScreenAsset asset;

	const bf3::core::ui::SwfSceneNode &at(std::string_view p_path) const {
		const bf3::core::ui::SwfSceneNode *n = scene->find_node(p_path);
		REQUIRE_MESSAGE(n != nullptr, std::string(p_path));
		return *n;
	}
};

inline RetailScreen open_retail_screen(
		Retail &r_r, std::string_view p_partition, bf3::core::ui::HudClientState p_client = {}) {
	using namespace bf3::core;
	using namespace bf3::core::ui;
	RetailScreen s;
	Result<HudScreenAsset> asset = read_hud_screen_asset(*r_r.level, p_partition);
	REQUIRE_MESSAGE(asset.ok(), (asset.ok() ? std::string() : asset.error().message));
	s.asset = *asset;
	Result<SwfScene> scene = SwfScene::open(*r_r.lib, asset->movie);
	REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? std::string() : scene.error().message));
	s.scene = std::make_unique<SwfScene>(std::move(*scene));
	s.scene->set_image_source(retail_images(r_r));
	Result<std::unique_ptr<HudScreen>> hud =
			HudScreen::open(*s.scene, *r_r.lib, s.asset, r_r.fonts.get(), std::move(p_client));
	REQUIRE_MESSAGE(hud.ok(), (hud.ok() ? std::string() : hud.error().message));
	s.hud = std::move(*hud);
	return s;
}

}

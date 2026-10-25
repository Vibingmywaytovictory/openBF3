#pragma once

#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_image.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/ui/ui_fonts.h"
#include "test_support.h"

#include <memory>
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
		Result<LevelMount> images = mount_swf_images(*r.mount->install);
		if (!images) {
			r.error = images.error().message;
			return &r;
		}
		r.images = std::make_shared<SwfMountImages>(std::move(*images));
	} else if (r.mount == nullptr) {
		bf3::test::shared_mount_or_skip();
		return nullptr;
	}
	REQUIRE_MESSAGE(r.error.empty(), r.error);
	return &r;
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
	s.scene->set_image_source(r_r.images);
	Result<std::unique_ptr<HudScreen>> hud =
			HudScreen::open(*s.scene, *r_r.lib, s.asset, r_r.fonts.get(), std::move(p_client));
	REQUIRE_MESSAGE(hud.ok(), (hud.ok() ? std::string() : hud.error().message));
	s.hud = std::move(*hud);
	return s;
}

}

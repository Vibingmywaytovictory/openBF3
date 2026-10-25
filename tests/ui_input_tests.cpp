#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/ui_graph.h"
#include "bf3/core/ui/ui_input.h"
#include "test_support.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace {

using Keys = std::vector<std::string>;

TEST_SUITE("ui-input-unit") {
	TEST_CASE("ui input: concept ids are fb.Input.InputConceptActions") {
		CHECK(int32_t(UiInputConcept::NavigateUp) == 0);
		CHECK(int32_t(UiInputConcept::Activate) == 6);
		CHECK(int32_t(UiInputConcept::Deactivate) == 7);
		CHECK(int32_t(UiInputConcept::Menu) == 8);
		CHECK(int32_t(UiInputConcept::Com) == 22);
		for (int32_t i = 0; i < kUiInputConceptCount; ++i) {
			const UiInputConcept c = UiInputConcept(i);
			CHECK(ui_input_concept_of_action("UIInputAction_" + std::string(ui_input_concept_name(c))) == c);
		}
		CHECK_FALSE(ui_input_concept_of_action("UIInputAction_ToggleMinimapType").has_value());
		CHECK_FALSE(ui_input_concept_of_action("Activate").has_value());
	}

	TEST_CASE("ui input: Flash key codes of the IDK names") {
		CHECK(ui_flash_key_code("IDK_Enter") == 13);
		CHECK(ui_flash_key_code("IDK_NumpadEnter") == 13);
		CHECK(ui_flash_key_code("IDK_Escape") == 27);
		CHECK(ui_flash_key_code("IDK_Space") == 32);
		CHECK(ui_flash_key_code("IDK_Tab") == 9);
		CHECK(ui_flash_key_code("IDK_A") == 65);
		CHECK(ui_flash_key_code("IDK_Z") == 90);
		CHECK(ui_flash_key_code("IDK_0") == 48);
		CHECK(ui_flash_key_code("IDK_F1") == 112);
		CHECK(ui_flash_key_code("IDK_F12") == 123);
		CHECK(ui_flash_key_code("IDK_Numpad5") == 101);
		CHECK(ui_flash_key_code("IDK_ArrowUp") == 38);
		CHECK(ui_flash_key_code("IDK_Insert") == 45);
		CHECK(ui_flash_key_code("IDK_Home") == 36);
		CHECK(ui_flash_key_code("IDK_Undefined") == 0);
		CHECK(ui_flash_key_code("Enter") == 0);
	}

	TEST_CASE("ui input: a bindings table swaps in one call") {
		UiInputBindings a;
		a.bindings = { { "ConceptActivate", UiInputDevice::Keyboard, "IDK_Enter" },
			{ "ConceptMenu", UiInputDevice::Keyboard, "IDK_Escape" },
			{ "ConceptDeactivate", UiInputDevice::Keyboard, "IDK_Escape" } };
		UiInputRouter router(a);
		CHECK(router.concepts_of_key("IDK_Enter") == std::vector<UiInputConcept>{ UiInputConcept::Activate });
		CHECK(router.concepts_of_key("IDK_Escape") ==
				std::vector<UiInputConcept>{ UiInputConcept::Deactivate, UiInputConcept::Menu });
		CHECK(router.concepts_of_key("IDK_P").empty());
		UiInputBindings b;
		b.bindings = { { "ConceptActivate", UiInputDevice::Keyboard, "IDK_P" },
			{ "ConceptActivate", UiInputDevice::Mouse, "IDB_Button_0" } };
		router.set_bindings(b);
		CHECK(router.concepts_of_key("IDK_Enter").empty());
		CHECK(router.concepts_of_key("IDK_P") == std::vector<UiInputConcept>{ UiInputConcept::Activate });
		CHECK(router.concepts_of_mouse("IDB_Button_0") == std::vector<UiInputConcept>{ UiInputConcept::Activate });
		CHECK(router.bindings().keys_of("ConceptActivate") == Keys{ "IDK_P", "IDB_Button_0" });
	}
}

struct RetailInput {
	bf3::test::Mounted *mount = nullptr;
	std::unique_ptr<EbxLibrary> level;
	std::string error;
};

RetailInput *retail_input_or_skip() {
	static RetailInput r;
	static bool tried = false;
	if (!tried) {
		tried = true;
		r.mount = bf3::test::shared_mount_or_skip();
		if (r.mount == nullptr) {
			return nullptr;
		}
		Result<std::unique_ptr<EbxLibrary>> level =
				EbxLibrary::open(r.mount->install, { "Win32/Levels/MP_Subway/MP_Subway" });
		if (!level) {
			r.error = level.error().message;
		} else {
			r.level = std::move(*level);
		}
	} else if (r.mount == nullptr) {
		bf3::test::skip_install_case("no install");
		return nullptr;
	}
	REQUIRE_MESSAGE(r.error.empty(), r.error);
	return &r;
}

TEST_SUITE("ui-input") {
	TEST_CASE("ui input: the PC keys of each UI concept from input/defaultinputconcepts") {
		RetailInput *r = retail_input_or_skip();
		if (r == nullptr) {
			return;
		}
		Result<UiInputBindings> b = read_ui_input_bindings(*r->level);
		REQUIRE_MESSAGE(b.ok(), (b.ok() ? std::string() : b.error().message));
		std::printf("ui.input bindings %zu\n", b->bindings.size());
		const std::map<UiInputConcept, Keys> expected{
			{ UiInputConcept::NavigateUp, { "IDK_W", "IDK_ArrowUp" } },
			{ UiInputConcept::NavigateDown, { "IDK_S", "IDK_ArrowDown" } },
			{ UiInputConcept::NavigateLeft, { "IDK_A", "IDK_ArrowLeft" } },
			{ UiInputConcept::NavigateRight, { "IDK_D", "IDK_ArrowRight" } },
			{ UiInputConcept::TabLeft, {} },
			{ UiInputConcept::TabRight, {} },
			{ UiInputConcept::Activate, { "IDK_Enter", "IDK_NumpadEnter", "IDK_Space" } },
			{ UiInputConcept::Deactivate, { "IDK_Escape" } },
			{ UiInputConcept::Menu, { "IDK_Escape" } },
			{ UiInputConcept::Cancel, { "IDK_X" } },
			{ UiInputConcept::OK, {} },
			{ UiInputConcept::Back, {} },
			{ UiInputConcept::Tab, { "IDK_Tab" } },
			{ UiInputConcept::Edit, { "IDK_Insert" } },
			{ UiInputConcept::View, { "IDK_Home" } },
			{ UiInputConcept::LThumb, {} },
			{ UiInputConcept::RThumb, {} },
			{ UiInputConcept::MapZoom, { "IDK_N" } },
			{ UiInputConcept::MapSize, { "IDK_M" } },
			{ UiInputConcept::SayAllChat, { "IDK_J" } },
			{ UiInputConcept::TeamChat, { "IDK_K" } },
			{ UiInputConcept::SquadChat, { "IDK_L" } },
			{ UiInputConcept::Com, { "IDK_Q" } },
		};
		for (const auto &[ui_concept, keys] : expected) {
			const std::string_view id = ui_input_concept_identifier(ui_concept);
			const Keys got = id.empty() ? Keys{} : b->keys_of(id);
			std::string joined;
			for (const std::string &k : got) {
				joined += " " + k;
			}
			std::printf("ui.input %s %s:%s\n", std::string(ui_input_concept_name(ui_concept)).c_str(),
					id.empty() ? "-" : std::string(id).c_str(), joined.c_str());
			CHECK_MESSAGE(got == keys, std::string(ui_input_concept_name(ui_concept)));
		}
		const UiInputRouter router(std::move(*b));
		CHECK(router.concepts_of_key("IDK_Escape") ==
				std::vector<UiInputConcept>{ UiInputConcept::Deactivate, UiInputConcept::Menu });
		CHECK(router.concepts_of_key("IDK_Space") == std::vector<UiInputConcept>{ UiInputConcept::Activate });
		CHECK(router.concepts_of_key("IDK_Tab") == std::vector<UiInputConcept>{ UiInputConcept::Tab });
	}

	TEST_CASE("ui input: the graphs' UIInputAction values are the AS2 concept ids") {
		RetailInput *r = retail_input_or_skip();
		if (r == nullptr) {
			return;
		}
		std::map<std::string, int32_t> seen;
		for (const std::string_view partition :
				{ "ui/flow/graph/spawn/spawngraphpc", "ui/flow/graph/endofround/endofroundgraph",
						"ui/flow/graph/ingame/mp/ingamemenumpgraph", "ui/flow/graph/ingame/tdm/playingtdmgraph",
						"ui/flow/graph/options/optionsgraph", "ui/flow/graph/spawn/customizationgraph" }) {
			Result<UiGraphAsset> g = read_ui_graph_asset(*r->level, partition);
			REQUIRE_MESSAGE(g.ok(), std::string(partition));
			for (const UiGraphNode &n : g->nodes) {
				for (const UiGraphPort &p : n.ports) {
					if (!p.input_event.empty()) {
						seen[p.input_event] = p.input_action;
					}
				}
			}
		}
		size_t matched = 0;
		for (const auto &[name, value] : seen) {
			const std::optional<UiInputConcept> c = ui_input_concept_of_action(name);
			std::printf("ui.input enum %s = %d%s\n", name.c_str(), value, c ? "" : " (no AS2 concept)");
			if (c) {
				++matched;
				CHECK_MESSAGE(int32_t(*c) == value, name);
			}
		}
		CHECK(matched >= 10);
	}

	TEST_CASE("ui input: install unchanged") {
		RetailInput *r = retail_input_or_skip();
		if (r == nullptr) {
			return;
		}
		const SnapshotDiff diff = diff_snapshots(r->mount->before, r->mount->reader.snapshot());
		std::printf("ui.input install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}

}

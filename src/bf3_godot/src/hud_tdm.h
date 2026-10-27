#pragma once

#include "ui_screen_records.h"

#include "bf3/core/ui/hud_model.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace godot {
class Node;
}

namespace bf3::godot_side {

class Bf3Localisation;
class Bf3UiAssets;

struct TdmHudRecords {
	uint64_t model_serial = 0;
	uint64_t draws = 0;


	uint64_t missing_fonts = 0;
	uint64_t missing_icons = 0;
	uint64_t missing_strings = 0;
};
static_assert(std::is_trivially_copyable_v<TdmHudRecords>);

bool tdm_hud_reports(std::string_view p_partition);
std::optional<UiScreenRecord> tdm_hud_screen_record(const TdmHudRecords &p_records, std::string_view p_partition);
UiLookupRecord tdm_hud_lookup_record(const TdmHudRecords &p_records);

class Bf3TdmHud final : public UiScreenRecords {
public:
	Bf3TdmHud();
	~Bf3TdmHud() override;
	Bf3TdmHud(const Bf3TdmHud &) = delete;
	Bf3TdmHud &operator=(const Bf3TdmHud &) = delete;

	bool build(godot::Node *p_parent, Bf3Localisation *p_localisation, Bf3UiAssets *p_ui_assets, std::string *r_error);
	bool built() const;

	void set_model(const core::ui::HudModel &p_model);

	TdmHudRecords records() const;
	bool reports(std::string_view p_partition) const override;
	std::optional<UiScreenRecord> screen_record(std::string_view p_partition) const override;
	std::optional<UiLookupRecord> lookup_record() const override;
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}

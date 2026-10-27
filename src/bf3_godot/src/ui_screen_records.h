#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {
class HudScreen;
}

namespace bf3::godot_side {

inline constexpr std::string_view kUiFactRows = "rows";
inline constexpr std::string_view kUiFactTeam1Kills = "team1_kills";
inline constexpr std::string_view kUiFactTeam2Kills = "team2_kills";
inline constexpr std::string_view kUiFactKiller = "KillCam_01.Name";
inline constexpr std::string_view kUiFactKillerWeapon = "KillCam_01.Weapon";
inline constexpr std::string_view kUiFactKillerWeaponImage = "KillCam_01.WeaponImage";

struct UiScreenRecord {
	bool visible = false;
	uint64_t shown = 0;
	std::map<std::string, double, std::less<>> numbers;
	std::map<std::string, std::string, std::less<>> texts;

	double number(std::string_view p_fact) const;
	std::string text(std::string_view p_fact) const;
};

struct UiLookupRecord {
	uint64_t model_serial = 0;
	uint64_t draws = 0;
	uint64_t missing_fonts = 0;
	uint64_t missing_icons = 0;
	uint64_t missing_strings = 0;
};

void add_screen_widget_facts(const bf3::core::ui::HudScreen &p_screen, UiScreenRecord &r_record);

class UiScreenRecordSet;

class UiScreenRecords {
public:
	UiScreenRecords() = default;
	UiScreenRecords(const UiScreenRecords &) = delete;
	UiScreenRecords &operator=(const UiScreenRecords &) = delete;
	virtual ~UiScreenRecords();
	virtual bool reports(std::string_view p_partition) const = 0;
	virtual std::optional<UiScreenRecord> screen_record(std::string_view p_partition) const = 0;
	virtual std::optional<UiLookupRecord> lookup_record() const { return std::nullopt; }

private:
	friend class UiScreenRecordSet;
	mutable std::vector<UiScreenRecordSet *> sets_;
};

class UiScreenRecordSet final : public UiScreenRecords {
public:
	UiScreenRecordSet() = default;
	~UiScreenRecordSet() override;
	void add(const UiScreenRecords *p_source);
	void remove(const UiScreenRecords *p_source);
	size_t sources() const { return sources_.size(); }

	bool reports(std::string_view p_partition) const override;
	std::optional<UiScreenRecord> screen_record(std::string_view p_partition) const override;
	std::optional<UiLookupRecord> lookup_record() const override;

private:
	friend class UiScreenRecords;
	std::vector<const UiScreenRecords *> sources_;
};

}

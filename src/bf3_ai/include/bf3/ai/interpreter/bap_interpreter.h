#pragma once

#include "bf3/ai/actionplan/statement.h"
#include "bf3/ai/main/ai_settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace bf3::ai {

class BAPEntry {
public:
	virtual ~BAPEntry() = default;
	virtual bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const = 0;
	virtual BAPStatementType target_id() const = 0;
};

inline constexpr size_t kBotLodLevelCount = 3;

class BAPInterpreter final : public PlanInterpreter {
public:
	bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const override;
	bool add_entry(const BAPEntry &p_entry, BotLodLevel p_lod = BotLodLevel::Full);
	bool remove_entry(BotLodLevel p_lod, const BAPEntry &p_entry);
	bool activate_entry(BAPStatementType p_type, BotLodLevel p_lod = BotLodLevel::Full);
	void activate_defaults();
	bool is_active(BAPStatementType p_type, BotLodLevel p_lod) const;
	const BAPEntry *entry(BAPStatementType p_type, BotLodLevel p_lod) const;

private:
	std::array<std::array<const BAPEntry *, kBotLodLevelCount>, kBAPStatementTypeCount> entries_{};
	std::array<uint64_t, kBotLodLevelCount> active_{};
};

struct PlanEntryKind {
	std::string_view name;
	BAPStatementType target = BAPStatementType::MoveTo;
	AiDomain domain = AiDomain::Any;
	bool has_body = false;
};

std::span<const PlanEntryKind> plan_entry_kinds();
const PlanEntryKind *find_plan_entry_kind(std::string_view p_name);
std::unique_ptr<BAPEntry> create_plan_entry(std::string_view p_name, const AiSettings &p_settings);
const BAPInterpreter &infantry_interpreter();

}

#include "bf3/ai/plan/plan_entries.h"

namespace bf3::ai {

void BAPContext::add_variable(std::unique_ptr<BotScriptVariable> p_variable) {
	variables_.push_back(std::move(p_variable));
}

void BAPContext::clear() {
	variables_.clear();
}

}

#include "bf3/ai/actionplan/infantry_actions.h"

namespace bf3::ai {

BAPATrigger::BAPATrigger(TriggerKind p_input, std::unique_ptr<BAPCondition> p_start,
		std::unique_ptr<BAPCondition> p_stop, uint64_t p_controls, int p_pattern_id, double p_now, double p_duration,
		bool p_release_after_press) :
		BAPAction(p_pattern_id, p_controls), input_(p_input), start_(std::move(p_start)), stop_(std::move(p_stop)),
		end_time_(p_now + p_duration), release_after_press_(p_release_after_press) {}

BAPASense::BAPASense(SenseFocus p_focus, std::unique_ptr<BAPCondition> p_first, std::unique_ptr<BAPCondition> p_second,
		int p_pattern_id) :
		BAPAction(p_pattern_id), focus_(p_focus), first_(std::move(p_first)), second_(std::move(p_second)) {}

}

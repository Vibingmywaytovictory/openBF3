#include "bf3/host/prediction_tally.h"

#include "bf3/core/util/format.h"
#include "bf3/sim/sim_types.h"

#include <algorithm>

namespace bf3::host {

namespace {

std::string per_minute(uint64_t p_count, double p_minutes) {
	if (p_minutes <= 0.0) {
		return "-";
	}
	return core::fmt("%.2f", double(p_count) / p_minutes);
}

}

void tally_level(PredictionTally &r_tally, const ClientStats &p_level_end) {
	const uint64_t sent = p_level_end.commands_sent;
	r_tally.predicted_ticks += sent >= r_tally.commands_seen ? sent - r_tally.commands_seen : sent;
	r_tally.commands_seen = sent;
	r_tally.corrections += p_level_end.reconciliations;
	r_tally.corrections_over_1mm += p_level_end.corrections_over_1mm;
	r_tally.motor_hash_mismatch += p_level_end.motor_hash_mismatch;
	r_tally.max_correction_m = std::max(r_tally.max_correction_m, p_level_end.max_correction_m);
	++r_tally.levels;
}

double predicted_minutes(const PredictionTally &p_tally) {
	return double(p_tally.predicted_ticks) / (double(sim::kSimHz) * 60.0);
}

std::string prediction_tally_text(const PredictionTally &p_tally) {
	const double minutes = predicted_minutes(p_tally);
	return core::fmt(
			"prediction over %u level(s), %.2f predicted min: corrections %llu (%s per minute), over 1 mm %llu "
			"(%s per minute, largest %.4f m), motor_hash_mismatch %llu",
			p_tally.levels, minutes, static_cast<unsigned long long>(p_tally.corrections),
			per_minute(p_tally.corrections, minutes).c_str(),
			static_cast<unsigned long long>(p_tally.corrections_over_1mm),
			per_minute(p_tally.corrections_over_1mm, minutes).c_str(), double(p_tally.max_correction_m),
			static_cast<unsigned long long>(p_tally.motor_hash_mismatch));
}

}

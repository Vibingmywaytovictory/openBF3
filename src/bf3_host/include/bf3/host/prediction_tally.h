#pragma once

#include "bf3/host/client_session.h"

#include <cstdint>
#include <string>

namespace bf3::host {

struct PredictionTally {
	uint32_t levels = 0;
	uint64_t predicted_ticks = 0;
	uint64_t corrections = 0;
	uint64_t corrections_over_1mm = 0;
	uint64_t motor_hash_mismatch = 0;
	uint64_t ordered_moves = 0;
	float max_correction_m = 0.0f;
	uint64_t commands_seen = 0;
};

void tally_level(PredictionTally &r_tally, const ClientStats &p_level_end);
double predicted_minutes(const PredictionTally &p_tally);
std::string prediction_tally_text(const PredictionTally &p_tally);

}

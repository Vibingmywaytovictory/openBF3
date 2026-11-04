#include <doctest/doctest.h>

#include "bf3/host/prediction_tally.h"

#include <string>

namespace {

using bf3::host::ClientStats;
using bf3::host::PredictionTally;

ClientStats level_end(uint64_t p_commands_sent, uint64_t p_reconciliations, uint64_t p_over_1mm, float p_max_m,
		uint64_t p_motor_mismatch) {
	ClientStats s;
	s.commands_sent = p_commands_sent;
	s.reconciliations = p_reconciliations;
	s.corrections_over_1mm = p_over_1mm;
	s.max_correction_m = p_max_m;
	s.motor_hash_mismatch = p_motor_mismatch;
	return s;
}

}

TEST_SUITE("host-prediction-tally") {
	TEST_CASE("the exit tally adds each level's corrections and counts minutes by the commands sent over the "
			  "connection") {
		PredictionTally t;
		bf3::host::tally_level(t, level_end(3600, 3, 2, 0.004f, 0));
		bf3::host::tally_level(t, level_end(10800, 1, 1, 0.02f, 1));
		CHECK(t.levels == 2);
		CHECK(t.predicted_ticks == 10800);
		CHECK(bf3::host::predicted_minutes(t) == doctest::Approx(3.0));
		CHECK(t.corrections == 4);
		CHECK(t.corrections_over_1mm == 3);
		CHECK(t.motor_hash_mismatch == 1);
		CHECK(t.max_correction_m == doctest::Approx(0.02f));
		CHECK(bf3::host::prediction_tally_text(t) ==
				"prediction over 2 level(s), 3.00 predicted min: corrections 4 (1.33 per minute), over 1 mm 3 "
				"(1.00 per minute, largest 0.0200 m), motor_hash_mismatch 1");
	}

	TEST_CASE("a new connection's command count starts again, and no predicted minute gives no rate") {
		PredictionTally t;
		bf3::host::tally_level(t, level_end(7200, 0, 0, 0.0f, 0));
		bf3::host::tally_level(t, level_end(1800, 2, 0, 0.0005f, 0));
		CHECK(t.predicted_ticks == 9000);
		PredictionTally none;
		bf3::host::tally_level(none, level_end(0, 0, 0, 0.0f, 0));
		CHECK(bf3::host::prediction_tally_text(none) ==
				"prediction over 1 level(s), 0.00 predicted min: corrections 0 (- per minute), over 1 mm 0 "
				"(- per minute, largest 0.0000 m), motor_hash_mismatch 0");
	}
}

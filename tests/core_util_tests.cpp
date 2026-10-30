#include <doctest/doctest.h>

#include "bf3/core/math.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/key_values.h"
#include "bf3/core/util/percentile.h"
#include "bf3/core/util/splitmix64.h"
#include "bf3/core/util/text.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace bf3::core;

namespace {

struct Point {
	float x = 0, y = 0, z = 0;
};

enum class Shape : uint8_t { Round, Square, Flat };

struct Sample {
	float speed = 1.5f;
	uint32_t count = 3;
	int32_t offset = -2;
	bool enabled = true;
	Shape shape = Shape::Square;
	bool operator==(const Sample &) const = default;
};

constexpr std::string_view kShapes[] = { "round", "square", "flat" };

#define BF3_SAMPLE_FIELD(f) [](Sample &s) -> void * { return &s.f; }

const KeyDef<Sample> kSampleKeys[] = {
	{ "speed", KeyKind::Float, 0.25f, 10.0f, BF3_SAMPLE_FIELD(speed) },
	{ "count", KeyKind::U32, 1, 64, BF3_SAMPLE_FIELD(count) },
	{ "offset", KeyKind::I32, -100, 100, BF3_SAMPLE_FIELD(offset) },
	{ "enabled", KeyKind::Bool, 0, 0, BF3_SAMPLE_FIELD(enabled) },
	{ "shape", KeyKind::Choice, 0, 0, BF3_SAMPLE_FIELD(shape), kShapes },
};

#undef BF3_SAMPLE_FIELD

const KeyValueGrammar<Sample> kSample{ "sample", kSampleKeys, 256, 8, 40 };

std::string fault_of(std::string_view p_text, const KeyValueGrammar<Sample> &p_grammar = kSample) {
	const Result<Sample> r = parse_key_values(p_text, p_grammar);
	return r ? std::string() : r.error().message;
}

}

TEST_SUITE("core-util-unit") {
	TEST_CASE("core.util: horizontal_distance measures in the ground plane and ignores height") {
		const Vec3 a{ 3.0f, 100.0f, 4.0f };
		const Vec3 b{ 0.0f, -5.0f, 0.0f };
		CHECK(vec::horizontal_distance(a, b) == 5.0f);
		CHECK(vec::horizontal_distance(b, a) == 5.0f);
		CHECK(vec::horizontal_distance(a, a) == 0.0f);
		CHECK(vec::horizontal_distance(a, Vec3{ 3.0f, -7.0f, 4.0f }) == 0.0f);
	}

	TEST_CASE("core.util: one degrees-to-radians constant in double and float") {
		CHECK(kDegToRad * 180.0 == doctest::Approx(3.14159265358979323846).epsilon(1e-15));
		CHECK(kDegToRad == 3.14159265358979323846 / 180.0);
		CHECK(kDegToRadF == 0.0174532925f);
		CHECK(kDegToRadF == 0.017453292519943295f);
		CHECK(double(kDegToRadF) == doctest::Approx(kDegToRad).epsilon(1e-7));
	}

	TEST_CASE("core.util: fmt is never truncated") {
		CHECK(fmt("%d-%s", 7, "x") == "7-x");
		const std::string wide(3000, 'a');
		CHECK(fmt("[%s]", wide.c_str()) == "[" + wide + "]");
		CHECK(fmt("%.3f", 1.0 / 3.0) == "0.333");
	}

	TEST_CASE("core.util: seconds_since and ms_since agree") {
		const auto start = std::chrono::steady_clock::now() - std::chrono::milliseconds(50);
		const double s = seconds_since(start);
		const double ms = ms_since(start);
		CHECK(s >= 0.05);
		CHECK(ms >= 50.0);
		CHECK(ms >= s * 1000.0);
	}

	TEST_CASE("core.util: thread_cpu_seconds counts this thread's work, not its sleep") {
		const double before_sleep = thread_cpu_seconds();
		const auto slept_from = std::chrono::steady_clock::now();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		CHECK(seconds_since(slept_from) >= 0.2);
		CHECK(thread_cpu_seconds() - before_sleep < 0.05);

		const double before_work = thread_cpu_seconds();
		const auto worked_from = std::chrono::steady_clock::now();
		uint64_t state = 1;
		while (thread_cpu_seconds() - before_work < 0.1 && seconds_since(worked_from) < 10.0) {
			for (int i = 0; i < 100000; ++i) {
				state = splitmix64(state);
			}
		}
		CHECK(state != 0);
		CHECK(thread_cpu_seconds() - before_work >= 0.1);
	}

	TEST_CASE("core.util: finite over scalars, spans and xyz types") {
		const float inf = std::numeric_limits<float>::infinity();
		const double nan = std::numeric_limits<double>::quiet_NaN();
		CHECK(finite(1.0f));
		CHECK_FALSE(finite(inf));
		CHECK_FALSE(finite(nan));
		const std::array<float, 3> good{ 1, 2, 3 };
		const std::array<float, 3> bad{ 1, inf, 3 };
		CHECK(finite(std::span<const float>(good)));
		CHECK_FALSE(finite(std::span<const float>(bad)));
		const std::vector<double> doubles{ 0.0, nan };
		CHECK_FALSE(finite(std::span<const double>(doubles)));
		CHECK(finite(Point{ 1, 2, 3 }));
		CHECK_FALSE(finite(Point{ 1, 2, inf }));
	}

	TEST_CASE("core.util: ascii lower, upper, iequals and trim") {
		CHECK(to_lower_ascii('Q') == 'q');
		CHECK(to_upper_ascii('q') == 'Q');
		CHECK(to_upper_ascii("ConceptJump_7") == "CONCEPTJUMP_7");
		CHECK(to_upper_ascii("\xE4x") == "\xE4X");
		CHECK(to_lower_ascii('7') == '7');
		CHECK(to_lower_ascii("Levels/MP_Subway") == "levels/mp_subway");
		CHECK(to_lower_ascii("\xC4X") == "\xC4x");
		CHECK(iequals_ascii("SoldierBody", "soldierBODY"));
		CHECK_FALSE(iequals_ascii("Soldier", "Soldiers"));
		CHECK(icontains_ascii("sound/Bullet_Craft/BULLET_IMPACTS/soldier", "bullet_impacts"));
		CHECK(icontains_ascii("abc", ""));
		CHECK_FALSE(icontains_ascii("bullet_impact", "bullet_impacts"));
		CHECK_FALSE(icontains_ascii("", "a"));
		CHECK(trim(" \t\r\n\f\vkey = 1 \t\r\n") == "key = 1");
		CHECK(trim(" \t ").empty());
		CHECK(trim("") == "");
	}

	TEST_CASE("core.util: splitmix64 golden values; next equals the pure form over a gamma walk") {
		static_assert(splitmix64(0) == 0xE220A8397B1DCDAFull);
		uint64_t state = 0;
		CHECK(splitmix64_next(state) == 0xE220A8397B1DCDAFull);
		CHECK(splitmix64_next(state) == 0x6E789E6AA1B965F4ull);
		CHECK(splitmix64_next(state) == 0x06C45D188009454Full);
		uint64_t walk = 12345;
		for (uint64_t i = 0; i < 1000; ++i) {
			const uint64_t expected = splitmix64(12345 + i * kSplitmix64Gamma);
			CHECK(splitmix64_next(walk) == expected);
		}
	}

	TEST_CASE("core.util: the four percentile flavours keep their ranks") {
		const std::vector<double> v{ 10, 1, 9, 2, 8, 3, 7, 4, 6, 5 };
		CHECK(nearest_rank(v, 0.5) == 5);
		CHECK(nearest_rank(v, 0.99) == 10);
		CHECK(nearest_rank(v, 0.0) == 1);
		CHECK(rank_above(v, 0.5) == 6);
		CHECK(rank_above(v, 1.0) == 10);
		CHECK(rounded_rank(v, 0.5) == 6);
		CHECK(rounded_rank(v, 0.95) == 10);
		CHECK(interpolated_percentile(v, 0.5) == doctest::Approx(5.5));
		CHECK(interpolated_percentile(v, 1.0) == 10);
		CHECK(nearest_rank(std::vector<int>{}, 0.5) == 0);
		CHECK(interpolated_percentile({}, 0.5) == 0.0);
	}

	TEST_CASE("core.util: key=value format / parse round trip and defaults") {
		const Sample defaults;
		const std::string text = format_key_values(defaults, kSampleKeys);
		CHECK(text == "speed = 1.5\ncount = 3\noffset = -2\nenabled = true\nshape = square\n");
		const Result<Sample> back = parse_key_values(text, kSample);
		REQUIRE(back);
		CHECK(*back == defaults);
		Sample changed;
		changed.speed = 0.3f;
		changed.count = 64;
		changed.offset = 100;
		changed.enabled = false;
		changed.shape = Shape::Flat;
		const Result<Sample> again = parse_key_values(format_key_values(changed, kSampleKeys), kSample);
		REQUIRE(again);
		CHECK(*again == changed);
		const Result<Sample> sparse = parse_key_values("# only one\n\n  count = 9  # trailing\n", kSample);
		REQUIRE(sparse);
		CHECK(sparse->count == 9);
		CHECK(sparse->speed == defaults.speed);
	}

	TEST_CASE("core.util: key=value faults name the line and the limit") {
		CHECK(fault_of(std::string(257, ' ')) == "sample: 257 bytes (limit 256)");
		CHECK(fault_of("\n\n\n\n\n\n\n\n") == "sample: 9 lines (limit 8)");
		CHECK(fault_of("\ncount = " + std::string(40, '1')) == "sample line 2: longer than 40 bytes");
		CHECK(fault_of("count") == "sample line 1: expected `key = value`, got `count`");
		CHECK(fault_of("colour = red") == "sample line 1: unknown key `colour`");
		CHECK(fault_of("count = 1\ncount = 2") == "sample line 2: `count` given twice");
		CHECK(fault_of("speed = fast") == "sample line 1: `speed` needs a number, got `fast`");
		CHECK(fault_of("speed = inf") == "sample line 1: `speed` needs a number, got `inf`");
		CHECK(fault_of("speed = 0.2") == "sample line 1: `speed` = 0.2 is outside 0.25..10");
		CHECK(fault_of("count = 2.5") == "sample line 1: `count` needs a whole number, got `2.5`");
		CHECK(fault_of("count = 65") == "sample line 1: `count` = 65 is outside 1..64");
		CHECK(fault_of("offset = -101") == "sample line 1: `offset` = -101 is outside -100..100");
		CHECK(fault_of("offset = 99999999999") == "sample line 1: `offset` = 99999999999 is outside -100..100");
		CHECK(fault_of("enabled = yes") == "sample line 1: `enabled` needs true or false, got `yes`");
		CHECK(fault_of("shape = oval") == "sample line 1: `shape` needs round, square or flat, got `oval`");
	}

	TEST_CASE("core.util: separator, empty items, repeated keys and bare items follow the grammar") {
		KeyValueGrammar<Sample> list = kSample;
		list.separator = ',';
		list.comments = false;
		list.skip_empty = false;
		list.unique_keys = false;
		const Result<Sample> r = parse_key_values("count=2, count=4 ,enabled=false", list);
		REQUIRE(r);
		CHECK(r->count == 4);
		CHECK_FALSE(r->enabled);
		CHECK(fault_of("count=2,,count=3", list) == "sample line 2: empty item");
		list.bare = [](std::string_view p_item, size_t p_index, Sample &r_value) -> KeyOutcome {
			if (p_index != 0 || p_item != "fast") {
				return { KeyFault::BadValue, 0.0, "needs fast first" };
			}
			r_value.speed = 9.0f;
			return {};
		};
		const Result<Sample> bare = parse_key_values("fast,count=5", list);
		REQUIRE(bare);
		CHECK(bare->speed == 9.0f);
		CHECK(bare->count == 5);
		CHECK(fault_of("count=5,fast", list) == "sample line 2: needs fast first");
	}

	TEST_CASE("core.util: an unknown hook takes keys the table lacks") {
		CHECK(fault_of("colour = red") == "sample line 1: unknown key `colour`");
		KeyValueGrammar<Sample> open = kSample;
		open.unknown = [](std::string_view p_key, std::string_view p_value, Sample &r_value) -> KeyOutcome {
			if (p_key == "bad") {
				return { KeyFault::BadValue, 0.0, "bad is refused" };
			}
			r_value.offset = int32_t(p_key.size() * 10 + p_value.size());
			return {};
		};
		const Result<Sample> r = parse_key_values("count = 4\ncolour = red\n", open);
		REQUIRE(r);
		CHECK(r->count == 4);
		CHECK(r->offset == 63);
		CHECK(fault_of("bad = 1", open) == "sample line 1: `bad` bad is refused");
	}

	TEST_CASE("core.util: a failed parse leaves the caller's start value alone") {
		Sample start;
		start.count = 11;
		const Result<Sample> kept = parse_key_values("offset = 5", kSample, start);
		REQUIRE(kept);
		CHECK(kept->count == 11);
		CHECK(kept->offset == 5);
		CHECK(start.offset == -2);
	}
}

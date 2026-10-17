// weapon_mechanics.cfg: the unmeasured weapon mechanics as a tuning file (docs/design/m2a.md §5.5; owner sv;
// mechanics_tuning.h has the format). The motor_tuning.cfg format (motor_tuning.cpp): "key = value" lines, '#'
// comments, every key a Mechanics field name, enums by snake_case name, booleans true / false, floats finite and in
// range. format_mechanics_tuning writes every key with the shortest round-trip number text, so
// parse(format(m)) == m field for field.
#include "bf3/sim/weapons/mechanics_tuning.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>

namespace bf3::sim::weapons {

namespace {

enum class KeyType : uint8_t { Enum, Bool, Float };

struct KeyDef {
	const char *name;
	KeyType type;
	// Enum: the names of the values 0..n-1 (nullptr-terminated, at most 4).
	std::array<const char *, 5> names;
	float min;
	float max;
	void *(*field)(Mechanics &);
};

#define BF3_MECH_FIELD(f) [](Mechanics &m) -> void * { return &m.f; }

// Ranges keep a hostile or mistyped file from making the damage arithmetic meaningless; every default and every
// plausible measured value lies well inside them. The enum names follow mechanics.h's enumerators in order.
const KeyDef kKeys[] = {
	{ "disc", KeyType::Enum, { "uniform_area", "uniform_radius", nullptr }, 0, 0, BF3_MECH_FIELD(disc) },
	{ "recovery", KeyType::Enum, { "linear", "exponential", nullptr }, 0, 0, BF3_MECH_FIELD(recovery) },
	{ "first_shot_rearm", KeyType::Enum, { "on_trigger_release", "when_recovered", nullptr }, 0, 0, BF3_MECH_FIELD(first_shot_rearm) },
	{ "min_spread_rise", KeyType::Enum, { "immediate", "at_decrease_rate", nullptr }, 0, 0, BF3_MECH_FIELD(min_spread_rise) },
	{ "falloff", KeyType::Enum, { "linear", nullptr }, 0, 0, BF3_MECH_FIELD(falloff) },
	{ "falloff_basis", KeyType::Enum, { "straight_line", "path_length", nullptr }, 0, 0, BF3_MECH_FIELD(falloff_basis) },
	{ "blast_falloff", KeyType::Enum, { "linear", nullptr }, 0, 0, BF3_MECH_FIELD(blast_falloff) },
	{ "stance_transition_penalty", KeyType::Bool, {}, 0, 0, BF3_MECH_FIELD(stance_transition_penalty) },
	{ "suppression_threshold", KeyType::Float, {}, 0.0f, 1.0f, BF3_MECH_FIELD(suppression_threshold) },
	{ "apply_protection_threshold", KeyType::Bool, {}, 0, 0, BF3_MECH_FIELD(apply_protection_threshold) },
	{ "bullets_pass_soldiers", KeyType::Bool, {}, 0, 0, BF3_MECH_FIELD(bullets_pass_soldiers) },
	{ "blast_occlusion", KeyType::Bool, {}, 0, 0, BF3_MECH_FIELD(blast_occlusion) },
	{ "empty_reload_unchambered", KeyType::Bool, {}, 0, 0, BF3_MECH_FIELD(empty_reload_unchambered) },
	{ "sprint_blocks_fire", KeyType::Bool, {}, 0, 0, BF3_MECH_FIELD(sprint_blocks_fire) },
	{ "spawn_protection_blocks", KeyType::Bool, {}, 0, 0, BF3_MECH_FIELD(spawn_protection_blocks) },
	{ "bullet_damage_modifier", KeyType::Float, {}, 0.0f, 100.0f, BF3_MECH_FIELD(bullet_damage_modifier) },
	{ "measured", KeyType::Bool, {}, 0, 0, BF3_MECH_FIELD(measured) },
};
constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef BF3_MECH_FIELD

// Every enum of Mechanics is a uint8_t enumeration, so the field is written as its byte.
static_assert(sizeof(DiscDistribution) == 1 && sizeof(RecoveryShape) == 1 && sizeof(FirstShotRearm) == 1 && sizeof(MinSpreadRise) == 1 &&
		sizeof(FalloffShape) == 1 && sizeof(FalloffBasis) == 1 && sizeof(BlastFalloff) == 1);

std::string_view trim(std::string_view p_text) {
	while (!p_text.empty() && (p_text.front() == ' ' || p_text.front() == '\t' || p_text.front() == '\r')) {
		p_text.remove_prefix(1);
	}
	while (!p_text.empty() && (p_text.back() == ' ' || p_text.back() == '\t' || p_text.back() == '\r')) {
		p_text.remove_suffix(1);
	}
	return p_text;
}

std::string shown(std::string_view p_text) {
	std::string out;
	for (size_t i = 0; i < p_text.size() && i < 40; ++i) {
		const char c = p_text[i];
		out += (c >= 0x20 && c < 0x7F) ? c : '?';
	}
	if (p_text.size() > 40) {
		out += "...";
	}
	return out;
}

std::string number_text(float p_value) {
	char buffer[64];
	const std::to_chars_result r = std::to_chars(buffer, buffer + sizeof(buffer), p_value);
	return std::string(buffer, r.ptr);
}

size_t enum_count(const KeyDef &p_key) {
	size_t n = 0;
	while (n < p_key.names.size() && p_key.names[n] != nullptr) {
		++n;
	}
	return n;
}

} // namespace

core::Result<Mechanics> parse_mechanics_tuning(std::string_view p_text) {
	if (p_text.size() > kMaxMechanicsTuningBytes) {
		return core::make_error("weapon mechanics: " + std::to_string(p_text.size()) + " bytes (limit " + std::to_string(kMaxMechanicsTuningBytes) + ")");
	}
	size_t lines = 1;
	for (const char c : p_text) {
		lines += c == '\n' ? 1 : 0;
	}
	if (lines > kMaxMechanicsTuningLines) {
		return core::make_error("weapon mechanics: " + std::to_string(lines) + " lines (limit " + std::to_string(kMaxMechanicsTuningLines) + ")");
	}
	Mechanics m;
	bool seen[kKeyCount] = {};
	size_t line_no = 0;
	size_t at = 0;
	while (at <= p_text.size()) {
		const size_t end = std::min(p_text.find('\n', at), p_text.size());
		std::string_view line = p_text.substr(at, end - at);
		at = end + 1;
		++line_no;
		if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
			line = line.substr(0, hash);
		}
		line = trim(line);
		if (line.empty()) {
			if (end == p_text.size()) {
				break;
			}
			continue;
		}
		const std::string where = "weapon mechanics line " + std::to_string(line_no);
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos) {
			return core::make_error(where + ": expected `key = value`, got `" + shown(line) + "`");
		}
		const std::string_view key = trim(line.substr(0, eq));
		const std::string_view value = trim(line.substr(eq + 1));
		size_t index = kKeyCount;
		for (size_t k = 0; k < kKeyCount; ++k) {
			if (key == kKeys[k].name) {
				index = k;
				break;
			}
		}
		if (index == kKeyCount) {
			return core::make_error(where + ": unknown key `" + shown(key) + "`");
		}
		if (seen[index]) {
			return core::make_error(where + ": `" + std::string(key) + "` given twice");
		}
		seen[index] = true;
		const KeyDef &k = kKeys[index];
		void *field = k.field(m);
		switch (k.type) {
			case KeyType::Enum: {
				const size_t n = enum_count(k);
				size_t v = n;
				for (size_t i = 0; i < n; ++i) {
					if (value == k.names[i]) {
						v = i;
						break;
					}
				}
				if (v == n) {
					std::string options;
					for (size_t i = 0; i < n; ++i) {
						options += (i == 0 ? "" : " | ") + std::string(k.names[i]);
					}
					return core::make_error(where + ": `" + std::string(key) + "` must be " + options + ", got `" + shown(value) + "`");
				}
				*static_cast<uint8_t *>(field) = uint8_t(v);
				break;
			}
			case KeyType::Bool:
				if (value == "true") {
					*static_cast<bool *>(field) = true;
				} else if (value == "false") {
					*static_cast<bool *>(field) = false;
				} else {
					return core::make_error(where + ": `" + std::string(key) + "` must be true or false, got `" + shown(value) + "`");
				}
				break;
			case KeyType::Float: {
				float v = 0;
				const std::from_chars_result r = std::from_chars(value.data(), value.data() + value.size(), v);
				if (value.empty() || r.ec != std::errc() || r.ptr != value.data() + value.size() || !std::isfinite(v)) {
					return core::make_error(where + ": `" + std::string(key) + "` needs a number, got `" + shown(value) + "`");
				}
				if (!(v >= k.min && v <= k.max)) {
					return core::make_error(
							where + ": `" + std::string(key) + "` = " + shown(value) + " is outside " + number_text(k.min) + ".." + number_text(k.max));
				}
				*static_cast<float *>(field) = v;
				break;
			}
		}
		if (end == p_text.size()) {
			break;
		}
	}
	return m;
}

std::string format_mechanics_tuning(const Mechanics &p_mechanics) {
	Mechanics m = p_mechanics;
	std::string out;
	for (const KeyDef &k : kKeys) {
		void *field = k.field(m);
		out += k.name;
		out += " = ";
		switch (k.type) {
			case KeyType::Enum: {
				const uint8_t v = *static_cast<uint8_t *>(field);
				out += v < enum_count(k) ? k.names[v] : "?";
				break;
			}
			case KeyType::Bool:
				out += *static_cast<bool *>(field) ? "true" : "false";
				break;
			case KeyType::Float:
				out += number_text(*static_cast<float *>(field));
				break;
		}
		out += '\n';
	}
	return out;
}

} // namespace bf3::sim::weapons

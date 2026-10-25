#include "bf3/core/util/key_values.h"

#include <charconv>
#include <cmath>
#include <system_error>

namespace bf3::core {

namespace {

template <class T> std::string number_text(T p_value) {
	char buffer[64];
	const std::to_chars_result r = std::to_chars(buffer, buffer + sizeof(buffer), p_value);
	return std::string(buffer, r.ptr);
}

template <class T> bool parse_finite_number(std::string_view p_text, T &r_out) {
	const std::from_chars_result r = std::from_chars(p_text.data(), p_text.data() + p_text.size(), r_out);
	return !p_text.empty() && r.ec == std::errc() && r.ptr == p_text.data() + p_text.size() && std::isfinite(r_out);
}

}

std::string shown_text(std::string_view p_text) {
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

std::string float_text(float p_value) {
	return number_text(p_value);
}

std::string float_text(double p_value) {
	return number_text(p_value);
}

bool parse_finite(std::string_view p_text, double &r_out) {
	return parse_finite_number(p_text, r_out);
}

std::string choice_list(
		std::span<const std::string_view> p_choices, std::string_view p_comma, std::string_view p_last) {
	std::string out;
	for (size_t i = 0; i < p_choices.size(); ++i) {
		if (i > 0) {
			out += i + 1 == p_choices.size() ? p_last : p_comma;
		}
		out += p_choices[i];
	}
	return out;
}

std::string key_fault_where(const KeyFaultInfo &p_info) {
	return std::string(p_info.what) + " line " + std::to_string(p_info.line);
}

std::string key_fault_message(const KeyFaultInfo &p_info) {
	const std::string what(p_info.what);
	const std::string where = key_fault_where(p_info);
	const std::string key(p_info.key);
	switch (p_info.fault) {
		case KeyFault::None:
			break;
		case KeyFault::TooLarge:
			return what + ": " + std::to_string(p_info.count) + " bytes (limit " + std::to_string(p_info.limit) + ")";
		case KeyFault::TooManyLines:
			return what + ": " + std::to_string(p_info.count) + " lines (limit " + std::to_string(p_info.limit) + ")";
		case KeyFault::LineTooLong:
			return where + ": longer than " + std::to_string(p_info.limit) + " bytes";
		case KeyFault::EmptyItem:
			return where + ": empty item";
		case KeyFault::NoEquals:
			return where + ": expected `key = value`, got `" + shown_text(p_info.item) + "`";
		case KeyFault::UnknownKey:
			return where + ": unknown key `" + shown_text(p_info.key) + "`";
		case KeyFault::DuplicateKey:
			return where + ": `" + key + "` given twice";
		case KeyFault::NotANumber:
			return where + ": `" + key + "` needs a number, got `" + shown_text(p_info.value) + "`";
		case KeyFault::NotAWholeNumber:
			return where + ": `" + key + "` needs a whole number, got `" + shown_text(p_info.value) + "`";
		case KeyFault::OutOfRange:
			return where + ": `" + key + "` = " + shown_text(p_info.value) + " is outside " +
					float_text(float(p_info.min)) + ".." + float_text(float(p_info.max));
		case KeyFault::NotABool:
			return where + ": `" + key + "` needs true or false, got `" + shown_text(p_info.value) + "`";
		case KeyFault::NotAChoice:
			return where + ": `" + key + "` needs " + choice_list(p_info.choices, ", ", " or ") + ", got `" +
					shown_text(p_info.value) + "`";
		case KeyFault::BadValue:
			return key.empty() ? where + ": " + std::string(p_info.detail)
							   : where + ": `" + key + "` " + std::string(p_info.detail);
	}
	return where + ": unreadable";
}

KeyOutcome parse_float_value(std::string_view p_value, double p_min, double p_max, float &r_out) {
	float v = 0;
	if (!parse_finite_number(p_value, v)) {
		return { KeyFault::NotANumber };
	}
	if (!(double(v) >= p_min && double(v) <= p_max)) {
		return { KeyFault::OutOfRange, double(v) };
	}
	r_out = v;
	return {};
}

KeyOutcome parse_u32_value(std::string_view p_value, double p_min, double p_max, uint32_t &r_out) {
	uint32_t v = 0;
	const std::from_chars_result r = std::from_chars(p_value.data(), p_value.data() + p_value.size(), v);
	if (p_value.empty() || r.ec != std::errc() || r.ptr != p_value.data() + p_value.size()) {
		return { KeyFault::NotAWholeNumber };
	}
	if (double(float(v)) < p_min || double(float(v)) > p_max) {
		return { KeyFault::OutOfRange, double(v) };
	}
	r_out = v;
	return {};
}

KeyOutcome parse_i32_value(std::string_view p_value, double p_min, double p_max, int32_t &r_out) {
	int64_t v = 0;
	const std::from_chars_result r = std::from_chars(p_value.data(), p_value.data() + p_value.size(), v);
	if (p_value.empty() || r.ec != std::errc() || r.ptr != p_value.data() + p_value.size()) {
		return { KeyFault::NotAWholeNumber };
	}
	if (v < int64_t(p_min) || v > int64_t(p_max)) {
		return { KeyFault::OutOfRange, double(v) };
	}
	r_out = int32_t(v);
	return {};
}

KeyOutcome parse_bool_value(std::string_view p_value, bool &r_out) {
	if (p_value == "true") {
		r_out = true;
		return {};
	}
	if (p_value == "false") {
		r_out = false;
		return {};
	}
	return { KeyFault::NotABool };
}

KeyOutcome parse_choice_value(std::string_view p_value, std::span<const std::string_view> p_choices, uint8_t &r_out) {
	for (size_t i = 0; i < p_choices.size(); ++i) {
		if (p_value == p_choices[i]) {
			r_out = uint8_t(i);
			return {};
		}
	}
	return { KeyFault::NotAChoice };
}

}

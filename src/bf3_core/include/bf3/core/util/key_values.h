#pragma once

#include "bf3/core/result.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::core {

enum class KeyKind : uint8_t { Float, U32, I32, Bool, Choice, Custom };

enum class KeyFault : uint8_t {
	None,
	TooLarge,
	TooManyLines,
	LineTooLong,
	EmptyItem,
	NoEquals,
	UnknownKey,
	DuplicateKey,
	NotANumber,
	NotAWholeNumber,
	OutOfRange,
	NotABool,
	NotAChoice,
	BadValue,
};

struct KeyFaultInfo {
	KeyFault fault = KeyFault::None;
	std::string_view what;
	std::string_view text;
	size_t line = 0;
	size_t count = 0;
	size_t limit = 0;
	std::string_view item;
	std::string_view key;
	std::string_view value;
	double number = 0.0;
	double min = 0.0;
	double max = 0.0;
	std::span<const std::string_view> choices;
	std::string_view detail;
};

struct KeyOutcome {
	KeyFault fault = KeyFault::None;
	double number = 0.0;
	std::string_view detail;
};

using KeyFaultMessage = std::string (*)(const KeyFaultInfo &);

std::string key_fault_message(const KeyFaultInfo &p_info);
std::string key_fault_where(const KeyFaultInfo &p_info);
std::string shown_text(std::string_view p_text);
std::string float_text(float p_value);
std::string choice_list(std::span<const std::string_view> p_choices, std::string_view p_comma, std::string_view p_last);

KeyOutcome parse_float_value(std::string_view p_value, double p_min, double p_max, float &r_out);
KeyOutcome parse_u32_value(std::string_view p_value, double p_min, double p_max, uint32_t &r_out);
KeyOutcome parse_i32_value(std::string_view p_value, double p_min, double p_max, int32_t &r_out);
KeyOutcome parse_bool_value(std::string_view p_value, bool &r_out);
KeyOutcome parse_choice_value(std::string_view p_value, std::span<const std::string_view> p_choices, uint8_t &r_out);

template <class T> struct KeyDef {
	std::string_view name;
	KeyKind kind = KeyKind::Float;
	double min = 0.0;
	double max = 0.0;
	void *(*field)(T &) = nullptr;
	std::span<const std::string_view> choices{};
	KeyOutcome (*parse)(const KeyDef &, std::string_view, T &) = nullptr;
	void (*format)(const KeyDef &, const T &, std::string &) = nullptr;
};

template <class T> struct KeyValueGrammar {
	std::string_view what;
	std::span<const KeyDef<T>> keys;
	size_t max_bytes = 0;
	size_t max_lines = 0;
	size_t max_line_bytes = SIZE_MAX;
	bool unique_keys = true;
	char separator = '\n';
	bool comments = true;
	bool skip_empty = true;
	KeyFaultMessage message = key_fault_message;
	KeyOutcome (*bare)(std::string_view p_item, size_t p_index, T &r_value) = nullptr;
};

template <class T> KeyOutcome parse_key_value(const KeyDef<T> &p_key, std::string_view p_value, T &r_value) {
	switch (p_key.kind) {
		case KeyKind::Float:
			return parse_float_value(p_value, p_key.min, p_key.max, *static_cast<float *>(p_key.field(r_value)));
		case KeyKind::U32:
			return parse_u32_value(p_value, p_key.min, p_key.max, *static_cast<uint32_t *>(p_key.field(r_value)));
		case KeyKind::I32:
			return parse_i32_value(p_value, p_key.min, p_key.max, *static_cast<int32_t *>(p_key.field(r_value)));
		case KeyKind::Bool:
			return parse_bool_value(p_value, *static_cast<bool *>(p_key.field(r_value)));
		case KeyKind::Choice:
			return parse_choice_value(p_value, p_key.choices, *static_cast<uint8_t *>(p_key.field(r_value)));
		case KeyKind::Custom:
			assert(p_key.parse != nullptr);
			return p_key.parse(p_key, p_value, r_value);
	}
	return { KeyFault::BadValue };
}

template <class T>
Result<T> parse_key_values(std::string_view p_text, const KeyValueGrammar<T> &p_grammar, T p_value = T{}) {
	KeyFaultInfo info;
	info.what = p_grammar.what;
	info.text = p_text;
	const auto fail = [&](KeyFault p_fault) -> Error {
		info.fault = p_fault;
		return make_error(p_grammar.message(info));
	};
	if (p_text.size() > p_grammar.max_bytes) {
		info.count = p_text.size();
		info.limit = p_grammar.max_bytes;
		return fail(KeyFault::TooLarge);
	}
	const size_t lines = 1 + size_t(std::count(p_text.begin(), p_text.end(), p_grammar.separator));
	if (lines > p_grammar.max_lines) {
		info.count = lines;
		info.limit = p_grammar.max_lines;
		return fail(KeyFault::TooManyLines);
	}
	std::vector<bool> seen(p_grammar.keys.size(), false);
	size_t index = 0;
	size_t at = 0;
	while (at <= p_text.size()) {
		const size_t end = std::min(p_text.find(p_grammar.separator, at), p_text.size());
		std::string_view item = p_text.substr(at, end - at);
		at = end + 1;
		++info.line;
		if (item.size() > p_grammar.max_line_bytes) {
			info.count = item.size();
			info.limit = p_grammar.max_line_bytes;
			return fail(KeyFault::LineTooLong);
		}
		if (p_grammar.comments) {
			item = item.substr(0, item.find('#'));
		}
		item = trim(item);
		info.item = item;
		info.key = {};
		info.value = {};
		if (item.empty()) {
			if (p_grammar.skip_empty) {
				continue;
			}
			return fail(KeyFault::EmptyItem);
		}
		const size_t eq = item.find('=');
		if (eq == std::string_view::npos) {
			if (p_grammar.bare == nullptr) {
				return fail(KeyFault::NoEquals);
			}
			const KeyOutcome outcome = p_grammar.bare(item, index++, p_value);
			if (outcome.fault != KeyFault::None) {
				info.detail = outcome.detail;
				return fail(outcome.fault);
			}
			continue;
		}
		info.key = trim(item.substr(0, eq));
		info.value = trim(item.substr(eq + 1));
		const auto key = std::find_if(p_grammar.keys.begin(), p_grammar.keys.end(),
				[&](const KeyDef<T> &p_key) { return p_key.name == info.key; });
		if (key == p_grammar.keys.end()) {
			return fail(KeyFault::UnknownKey);
		}
		const size_t k = size_t(key - p_grammar.keys.begin());
		if (p_grammar.unique_keys && seen[k]) {
			return fail(KeyFault::DuplicateKey);
		}
		seen[k] = true;
		const KeyOutcome outcome = parse_key_value(*key, info.value, p_value);
		if (outcome.fault != KeyFault::None) {
			info.number = outcome.number;
			info.min = key->min;
			info.max = key->max;
			info.choices = key->choices;
			info.detail = outcome.detail;
			return fail(outcome.fault);
		}
		++index;
	}
	return p_value;
}

template <class T> void format_key_value(const KeyDef<T> &p_key, T &r_value, std::string &r_out) {
	switch (p_key.kind) {
		case KeyKind::Float:
			r_out += float_text(*static_cast<const float *>(p_key.field(r_value)));
			return;
		case KeyKind::U32:
			r_out += std::to_string(*static_cast<const uint32_t *>(p_key.field(r_value)));
			return;
		case KeyKind::I32:
			r_out += std::to_string(*static_cast<const int32_t *>(p_key.field(r_value)));
			return;
		case KeyKind::Bool:
			r_out += *static_cast<const bool *>(p_key.field(r_value)) ? "true" : "false";
			return;
		case KeyKind::Choice: {
			const uint8_t v = *static_cast<const uint8_t *>(p_key.field(r_value));
			r_out += v < p_key.choices.size() ? p_key.choices[v] : std::string_view("?");
			return;
		}
		case KeyKind::Custom:
			assert(p_key.format != nullptr);
			p_key.format(p_key, r_value, r_out);
			return;
	}
}

template <class T>
std::string format_key_values(const T &p_value, std::span<const KeyDef<std::type_identity_t<T>>> p_keys) {
	T value = p_value;
	std::string out;
	for (const KeyDef<T> &k : p_keys) {
		out += k.name;
		out += " = ";
		format_key_value(k, value, out);
		out += '\n';
	}
	return out;
}

}

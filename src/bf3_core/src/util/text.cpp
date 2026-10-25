#include "bf3/core/util/text.h"

namespace bf3::core {

namespace {

bool blank(char p_c) {
	return p_c == ' ' || p_c == '\t' || p_c == '\n' || p_c == '\r' || p_c == '\f' || p_c == '\v';
}

}

std::string to_lower_ascii(std::string_view p_text) {
	std::string out(p_text);
	for (char &c : out) {
		c = to_lower_ascii(c);
	}
	return out;
}

std::string to_upper_ascii(std::string_view p_text) {
	std::string out(p_text);
	for (char &c : out) {
		c = to_upper_ascii(c);
	}
	return out;
}

bool iequals_ascii(std::string_view p_a, std::string_view p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (size_t i = 0; i < p_a.size(); ++i) {
		if (to_lower_ascii(p_a[i]) != to_lower_ascii(p_b[i])) {
			return false;
		}
	}
	return true;
}

std::string_view trim(std::string_view p_text) {
	while (!p_text.empty() && blank(p_text.front())) {
		p_text.remove_prefix(1);
	}
	while (!p_text.empty() && blank(p_text.back())) {
		p_text.remove_suffix(1);
	}
	return p_text;
}

}

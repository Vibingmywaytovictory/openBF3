#include "json_number.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace bf3dump {

namespace {

struct SciParts {
	bool negative = false;
	std::string digits;
	int exp10 = 0;
};

SciParts split_scientific(std::string_view p_sci) {
	SciParts out;
	size_t i = 0;
	if (p_sci[0] == '-') {
		out.negative = true;
		i = 1;
	}
	const size_t e = p_sci.find('e');
	for (size_t j = i; j < e; ++j) {
		if (p_sci[j] != '.') {
			out.digits += p_sci[j];
		}
	}
	const size_t exp_at = e + 1 + (p_sci[e + 1] == '+' ? 1 : 0);
	std::from_chars(p_sci.data() + exp_at, p_sci.data() + p_sci.size(), out.exp10);
	return out;
}

std::string join_scientific(const SciParts &p_parts) {
	std::string out;
	if (p_parts.negative) {
		out += '-';
	}
	out += p_parts.digits[0];
	if (p_parts.digits.size() > 1) {
		out += '.';
		out += std::string_view(p_parts.digits).substr(1);
	}
	out += 'e';
	out += std::to_string(p_parts.exp10);
	return out;
}

}

std::string rust_f32_digits(float p_value) {

	char buf[64];
	const auto res = std::to_chars(buf, buf + sizeof(buf), p_value, std::chars_format::scientific);
	const std::string shortest(buf, res.ptr);
	const SciParts parts = split_scientific(shortest);

	char exact_buf[192];
	const auto exact_res = std::to_chars(exact_buf, exact_buf + sizeof(exact_buf), static_cast<double>(p_value), std::chars_format::scientific, 150);
	const SciParts exact = split_scientific(std::string_view(exact_buf, static_cast<size_t>(exact_res.ptr - exact_buf)));
	const size_t n = parts.digits.size();
	if (exact.exp10 != parts.exp10 || exact.digits.size() <= n || exact.digits[n] != '5' ||
			exact.digits.find_first_not_of('0', n + 1) != std::string::npos) {
		return shortest;
	}

	SciParts up = exact;
	up.digits.resize(n);
	size_t k = n;
	while (k > 0 && up.digits[k - 1] == '9') {
		up.digits[--k] = '0';
	}
	if (k == 0) {
		up.digits.insert(up.digits.begin(), '1');
		up.digits.pop_back();
		++up.exp10;
	} else {
		++up.digits[k - 1];
	}
	if (up.digits == parts.digits && up.exp10 == parts.exp10) {
		return shortest;
	}
	const std::string candidate = join_scientific(up);
	float back = 0;
	const auto back_res = std::from_chars(candidate.data(), candidate.data() + candidate.size(), back);
	if (back_res.ec != std::errc() || std::memcmp(&back, &p_value, sizeof(float)) != 0) {
		return shortest;
	}
	return candidate;
}

std::string json_f64_text(double p_value) {
	if (p_value == 0.0) {
		return std::signbit(p_value) ? "-0.0" : "0.0";
	}
	char buf[64];
	const auto res = std::to_chars(buf, buf + sizeof(buf), p_value, std::chars_format::scientific);
	const SciParts parts = split_scientific(std::string_view(buf, static_cast<size_t>(res.ptr - buf)));
	const std::string &digits = parts.digits;
	std::string out = parts.negative ? "-" : "";
	const int length = static_cast<int>(digits.size());
	const int k = parts.exp10 - (length - 1);
	const int kk = length + k;
	if (0 <= k && kk <= 16) {
		out += digits;
		out.append(static_cast<size_t>(kk - length), '0');
		out += ".0";
	} else if (0 < kk && kk <= 16) {
		out += digits.substr(0, static_cast<size_t>(kk));
		out += '.';
		out += digits.substr(static_cast<size_t>(kk));
	} else if (-5 < kk && kk <= 0) {
		out += "0.";
		out.append(static_cast<size_t>(-kk), '0');
		out += digits;
	} else {
		out += digits[0];
		if (length > 1) {
			out += '.';
			out += digits.substr(1);
		}
		out += kk - 1 >= 0 ? "e+" : "e";
		out += std::to_string(kk - 1);
	}
	return out;
}

std::string json_f32_text(float p_value) {
	const std::string digits = rust_f32_digits(p_value);
	double d = 0;
	std::from_chars(digits.data(), digits.data() + digits.size(), d);
	return json_f64_text(d);
}

int json_number_selfcheck() {
	struct F64Case {
		double value;
		const char *text;
	};

	static const F64Case kF64[] = {
		{ 0.0, "0.0" },
		{ -0.0, "-0.0" },
		{ 1.0, "1.0" },
		{ 123.0, "123.0" },
		{ 43210.1, "43210.1" },
		{ 1e15, "1000000000000000.0" },
		{ -5942736479622170.0, "-5942736479622170.0" },
		{ 5.444310685350916e+14, "544431068535091.6" },
		{ 1e16, "1e+16" },
		{ 2.7849858e23, "2.7849858e+23" },
		{ 3.439070283483335e+35, "3.439070283483335e+35" },
		{ -6.079537928711555e+61, "-6.079537928711555e+61" },
		{ 1.7976931348623157e308, "1.7976931348623157e+308" },
		{ 0.001, "0.001" },
		{ 1e-5, "0.00001" },
		{ -1.5e-5, "-0.000015" },
		{ 1e-6, "1e-6" },
		{ -2.9802322387695312e-08, "-2.9802322387695312e-8" },
		{ 6.62607015e-34, "6.62607015e-34" },
		{ 5e-324, "5e-324" },
	};
	struct F32Case {
		float value;
		const char *text;
	};

	static const F32Case kF32[] = {
		{ 1.58203125f, "1.5820313" },
		{ -3353.03125f, "-3353.0313" },
		{ 0.05078125f, "0.05078125" },
		{ 2.7849858e23f, "2.7849858e+23" },
		{ -2.7849858e23f, "-2.7849858e+23" },
		{ 0.1f, "0.1" },
		{ -0.3f, "-0.3" },
		{ 1.0f, "1.0" },
		{ 16777216.0f, "16777216.0" },
		{ 1e-7f, "1e-7" },
		{ 3.4028235e38f, "3.4028235e+38" },
		{ 1e-45f, "1e-45" },
	};
	int failed = 0;
	for (const F64Case &c : kF64) {
		const std::string got = json_f64_text(c.value);
		if (got != c.text) {
			std::fprintf(stderr, "f64 %.17g: got %s, want %s\n", c.value, got.c_str(), c.text);
			++failed;
		}
	}
	for (const F32Case &c : kF32) {
		const std::string got = json_f32_text(c.value);
		if (got != c.text) {
			std::fprintf(stderr, "f32 %.9g: got %s, want %s\n", static_cast<double>(c.value), got.c_str(), c.text);
			++failed;
		}
	}
	return failed;
}

}

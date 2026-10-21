#include "bf3/core/util/format.h"

#include <cstdarg>
#include <cstdio>

namespace bf3::core {

std::string fmt(const char *p_format, ...) {
	va_list args;
	va_start(args, p_format);
	va_list measure;
	va_copy(measure, args);
	const int n = std::vsnprintf(nullptr, 0, p_format, measure);
	va_end(measure);
	std::string out;
	if (n > 0) {
		out.resize(size_t(n));
		std::vsnprintf(out.data(), out.size() + 1, p_format, args);
	}
	va_end(args);
	return out;
}

}

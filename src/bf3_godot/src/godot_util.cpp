#include "godot_util.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace bf3::godot_side {

using namespace godot;

String to_godot(const std::string &p_utf8) {
	return String::utf8(p_utf8.c_str(), static_cast<int64_t>(p_utf8.size()));
}

std::string to_std(const String &p_string) {
	const CharString cs = p_string.utf8();
	return std::string(cs.get_data(), static_cast<size_t>(cs.length()));
}

std::string user_arg_value(const char *p_name) {
	const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	const std::string prefix = std::string("--") + p_name + "=";
	for (int64_t i = 0; i < args.size(); ++i) {
		const std::string a = to_std(args[i]);
		if (a.rfind(prefix, 0) == 0) {
			return a.substr(prefix.size());
		}
	}
	return {};
}

bool has_user_arg(const char *p_name) {
	const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	const std::string flag = std::string("--") + p_name;
	for (int64_t i = 0; i < args.size(); ++i) {
		const std::string a = to_std(args[i]);
		if (a == flag || a.rfind(flag + "=", 0) == 0) {
			return true;
		}
	}
	return false;
}

bool has_check_arg(const char *p_name) {
	if (!has_user_arg(p_name)) {
		return false;
	}
	if (!kBuildChecks) {
		const std::string why = ": this build has no checks (OPENBF3_BUILD_CHECKS=OFF); ignored";
		UtilityFunctions::push_error(to_godot(std::string("--") + p_name + why));
	}
	return kBuildChecks;
}

std::string engine_summary() {
	const Dictionary v = Engine::get_singleton()->get_version_info();
	const std::string hash = to_std(String(v["hash"]));
	const bool single = OS::get_singleton()->has_feature("single");
	return to_std(String(v["string"])) + " " + hash.substr(0, 9) + (single ? " single" : " double");
}

}

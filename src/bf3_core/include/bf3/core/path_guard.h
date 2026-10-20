#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core {

std::optional<std::filesystem::path> canonicalize_path(const std::filesystem::path &p_path);

std::filesystem::path to_extended_length(const std::filesystem::path &p_path);

bool path_is_within(const std::filesystem::path &p_child, const std::filesystem::path &p_root);

class PathGuard {
public:
	enum class Verdict {
		Allowed,
		UnderProtectedRoot,
		HardLinkedFile,
		InvalidName,
		Unresolvable,
		NoRoots,
	};

	PathGuard() = default;
	bool add_protected_root(const std::filesystem::path &p_root);
	const std::vector<std::filesystem::path> &protected_roots() const { return roots_; }

	Verdict check_write_target(const std::filesystem::path &p_target) const;
	bool allows_write(const std::filesystem::path &p_target) const { return check_write_target(p_target) == Verdict::Allowed; }
	static const char *verdict_name(Verdict p_verdict);

private:
	std::vector<std::filesystem::path> roots_;
};

}

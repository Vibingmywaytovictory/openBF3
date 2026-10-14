#pragma once
// Write-target guard (plan §2.1): every path the cache writer (and anything else that writes)
// is about to create or replace is canonicalised and refused when it lies under a protected root
// (the BF3 install). Canonical form: absolute, long names, ".." folded, links and junctions of
// the existing part resolved, so aliases such as 8.3 names, other casing, forward slashes or a
// junction pointing into the install all compare equal to the install root.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core {

// Canonical absolute form of p_path, or nullopt if it cannot be formed (empty path, API failure).
// The deepest existing ancestor is resolved by the file system; the remaining (not yet existing)
// components are appended lexically.
std::optional<std::filesystem::path> canonicalize_path(const std::filesystem::path &p_path);

// True when p_child equals p_root or lies below it. Both must be canonical. Component-wise and
// case-insensitive (so "Battlefield 3x" is not inside "Battlefield 3").
bool path_is_within(const std::filesystem::path &p_child, const std::filesystem::path &p_root);

class PathGuard {
public:
	enum class Verdict {
		Allowed,
		UnderProtectedRoot, // the target is the install or lies inside it
		HardLinkedFile, // the target exists with more than one name; could alias an install file
		InvalidName, // stream syntax (':' after the drive), device or empty path
		Unresolvable, // the path could not be canonicalised
		NoRoots, // the guard protects nothing: refuse rather than allow by accident
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

} // namespace bf3::core

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::test {

constexpr std::string_view kScratchRootName = "openbf3-tests";
constexpr std::string_view kScratchTagVariable = "OPENBF3_SCRATCH_TAG";
constexpr std::chrono::hours kLegacyScratchAge{ 6 };

std::filesystem::path temp_dir();
std::filesystem::path scratch_root();
std::string scratch_tag();
std::string_view scratch_name_tag(std::string_view p_name);

class ScratchDir {
public:
	explicit ScratchDir(std::string_view p_name);
	~ScratchDir();
	ScratchDir(ScratchDir &&p_other) noexcept;
	ScratchDir &operator=(ScratchDir &&p_other) noexcept;
	ScratchDir(const ScratchDir &) = delete;
	ScratchDir &operator=(const ScratchDir &) = delete;

	const std::filesystem::path &path() const { return path_; }

private:
	std::filesystem::path path_;
};

std::filesystem::path process_scratch(std::string_view p_name);

enum class ScratchOwner : uint8_t {
	Alive,
	Dead,
	Unknown,
};

ScratchOwner scratch_owner(const std::filesystem::path &p_entry);
bool is_legacy_scratch_name(std::string_view p_name);

struct SweepReport {
	bool ran = false;
	std::vector<std::filesystem::path> removed;
	std::vector<std::filesystem::path> kept;
};

SweepReport sweep_scratch(const std::filesystem::path &p_temp, std::chrono::seconds p_legacy_age,
		std::chrono::milliseconds p_budget, std::string_view p_pass_tag);
void scratch_startup();

}

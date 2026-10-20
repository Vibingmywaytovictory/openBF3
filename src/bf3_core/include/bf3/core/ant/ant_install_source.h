#pragma once

#include "bf3/core/ant/ant_source.h"
#include "bf3/core/install.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf3::core::ant {

inline constexpr uint32_t kAssetBankResType = 0x51A3C853;
inline constexpr const char *kAntPackagePrefix = "animations/antanimations/";

class AntInstallSource final : public AntPackageSource {
public:
	struct OpenReport {
		size_t superbundles_scanned = 0;
		size_t superbundles_used = 0;
		size_t partitions = 0;
		std::vector<std::string> warnings;
		double seconds = 0.0;
	};

	static Result<std::unique_ptr<AntInstallSource>> open(std::shared_ptr<const Install> p_install);

	AntInstallSource(const AntInstallSource &) = delete;
	AntInstallSource &operator=(const AntInstallSource &) = delete;

	const std::vector<AntPackageInfo> &packages() const override { return packages_; }
	const std::vector<AntPackageInfo> &all_banks() const override { return banks_; }
	bool read(const AntPackageInfo &p_info, std::vector<uint8_t> &r_bytes, std::string &r_error) const override;

	const OpenReport &report() const { return report_; }
	const std::shared_ptr<const Install> &install() const { return install_; }

private:
	struct ResSite {
		uint32_t mount = 0;
		std::string superbundle;
		std::string bundle;
	};

	AntInstallSource() = default;
	const LevelMount *weapons_mount(std::string &r_error) const;

	std::shared_ptr<const Install> install_;
	std::vector<std::unique_ptr<LevelMount>> mounts_;
	std::unordered_map<std::string, ResSite> res_;
	std::unordered_map<std::string, uint32_t> package_mount_;
	std::vector<AntPackageInfo> packages_;
	std::vector<AntPackageInfo> banks_;
	OpenReport report_;

	mutable std::once_flag weapons_once_;
	mutable std::unique_ptr<LevelMount> weapons_;
	mutable std::string weapons_error_;
};

}

#pragma once
// AntInstallSource: Ant package bytes read from the install through the container layer (Install +
// LevelMount), with the same selection as the research export (tools/antdump/README.md "Selection rules"):
//
//   * superbundles in layout order, except chunk superbundles (name contains "chunks") and Win32/Loc/*;
//   * per superbundle, every bundle manifest in bundle order; an AssetBank res (type 0x51A3C853) or an EBX
//     partition under "animations/antanimations/" is taken from the FIRST superbundle that lists it (so
//     s_basicassets comes from Win32/Globals), and within it from the first bundle that lists it;
//   * the bundles a superbundle supplies are loaded in sorted name order (the export's BTreeSet), so a res
//     read resolves exactly as the export's read did;
//   * a partition is taken where it reads and parses (else the next superbundle is tried); only primary
//     instances of type AntPackageAsset become packages (Name, PackagingType, StreamingGuid, ChunkSize);
//   * a Chunk package's chunk is read from the mount of its partition's superbundle, and when that fails from
//     Win32/Weapons with every bundle loaded.
//
// The selection runs once in open() (bundle manifests and the AntPackageAsset partitions only); payloads are
// read on demand. Every install file is opened through Install / InstallReader (read-only).
// Clean-room implementation from research/probes/ant-animation-probe.md (§2).

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
		size_t superbundles_scanned = 0; // non-chunk, non-Loc superbundles whose manifests were read
		size_t superbundles_used = 0; // that supplied at least one res or partition
		size_t partitions = 0; // EBX partitions taken under animations/antanimations/ (any primary type)
		std::vector<std::string> warnings; // mount / manifest / partition failures that were skipped
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
	std::vector<std::unique_ptr<LevelMount>> mounts_; // superbundles that supplied something, in layout order
	std::unordered_map<std::string, ResSite> res_; // lowercase res name -> where it is read
	std::unordered_map<std::string, uint32_t> package_mount_; // partition -> mount of its superbundle
	std::vector<AntPackageInfo> packages_;
	std::vector<AntPackageInfo> banks_;
	OpenReport report_;

	mutable std::once_flag weapons_once_;
	mutable std::unique_ptr<LevelMount> weapons_;
	mutable std::string weapons_error_;
};

} // namespace bf3::core::ant

#pragma once

#include "ant_export_source.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ant/ant_source.h"
#include "bf3/core/install.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bf3::tools::antdump {

using namespace bf3::core::ant;

struct SourceOptions {
	bool install = false;
	std::string install_dir;
	std::string export_dir;
};
struct OpenedSource {
	std::unique_ptr<AntPackageSource> source;
	std::shared_ptr<const bf3::core::Install> install;
	std::string description;
};

bool open_source(const SourceOptions &p_options, OpenedSource &r_opened, std::string &r_error, bool *r_unavailable = nullptr);

struct ParityResult {
	size_t packages_a = 0, packages_b = 0, package_rows_equal = 0;
	size_t banks_a = 0, banks_b = 0, bank_rows_equal = 0;
	size_t payloads = 0, payloads_equal = 0, payload_errors = 0;
	uint64_t payload_bytes = 0;
	std::vector<std::string> differences;
	bool ok() const;
};
void compare_sources(const AntPackageSource &p_a, const AntPackageSource &p_b, bool p_payloads, ParityResult &r_result);
std::string format_parity(const ParityResult &p_result);

bool load_library(const AntPackageSource &p_source, AntLibrary &r_library, std::string &r_error);
std::unique_ptr<AntPackage> load_package(const AntPackageSource &p_source, const AntPackageInfo &p_info, std::string &r_error);

struct CodecCensus {
	uint32_t clips = 0;
	uint32_t decoded = 0;
	uint32_t unsupported = 0;
	uint32_t errors = 0;
	uint32_t key_count_mismatch = 0;
	uint32_t non_unit_quats = 0;
	uint64_t keys = 0;
	uint64_t channel_values = 0;
	uint32_t mapped = 0;
	uint32_t mapped_perfect = 0;
};

struct Census {
	bool mp_only = false;
	uint32_t packages = 0;
	uint32_t packages_failed = 0;
	std::vector<std::string> failures;
	uint32_t chunk_size_ok = 0;
	uint32_t chunk_packages = 0;
	uint64_t static_objects = 0;
	uint64_t static_layouts = 0;
	std::map<std::string, CodecCensus> codecs;
	std::map<std::string, uint32_t> static_codecs;

	uint32_t dct_ok = 0;
	uint32_t dct_q_off = 0;
	double dct_worst_mean_q = 0.0;
	uint32_t dct_overruns = 0;
	std::map<int64_t, uint32_t> dct_datasize_residual;
	uint64_t dct_bitstream_bytes = 0;
	uint64_t dct_bits_read = 0;
	double dct_decode_seconds = 0.0;

	uint32_t raw_exact = 0, raw_padded = 0, raw_other = 0;
	uint32_t frame_exact = 0, frame_other = 0;

	uint32_t clips_with_controller = 0;
	uint32_t clips_without_controller = 0;
	uint32_t clips_kind_consistent = 0;
	uint64_t channels_total = 0;
	uint64_t channels_ok = 0;
	uint64_t channels_on_delta_traj = 0;
	uint32_t bind_errors = 0;
	double total_seconds = 0.0;
	std::vector<std::string> notes;
};

bool run_census(const AntPackageSource &p_source, const AntLibrary &p_library, bool p_mp_only, Census &r_census, std::string &r_error);

using PackageLoader = std::function<const AntPackage *(const AntPackageInfo &p_info, std::string &r_error)>;

bool run_censuses(const AntPackageSource &p_source, const AntLibrary &p_library, Census *r_mp, Census *r_all, std::string &r_error,
		const PackageLoader &p_load = {});
std::string format_census(const Census &p_census);

bool clip_to_json(const AntLibrary &p_library, const AntPackage &p_package, const GdStruct &p_clip, int p_digits, std::string &r_json,
		std::string &r_error);

struct CompareResult {
	bool shape_ok = false;
	std::vector<std::string> differences;
	uint64_t values = 0;
	double max_track_diff = 0.0;
	uint64_t joint_values = 0;
	double max_joint_diff = 0.0;
};

bool compare_clip_json(const std::string &p_ours, const std::string &p_oracle, CompareResult &r_result, std::string &r_error);

bool oracle_package(const std::string &p_oracle, std::string &r_package, std::string &r_error);
bool find_oracle_clip(const AntLibrary &p_library, const AntPackage &p_package, const std::string &p_oracle, GdStruct &r_clip, std::string &r_error);

}

#pragma once
// antdump support: census over every Ant package, clip-to-JSON dump (the probe's clip JSON shape) and the
// comparison against such a JSON. Used by the antdump CLI and by the core.ant tests. Godot-free.
// Clean-room implementation from research/probes/ant-animation-probe.md.

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

// The input of every command and of the core.ant tests: the install (AntInstallSource, located like the game:
// the given directory, else OPENBF3_INSTALL, else the default path) or an export directory (AntExportDirSource).
struct SourceOptions {
	bool install = false;
	std::string install_dir; // install: "" = locate as the game does
	std::string export_dir; // !install
};
struct OpenedSource {
	std::unique_ptr<AntPackageSource> source;
	std::shared_ptr<const bf3::core::Install> install; // install input only
	std::string description; // one line for logs
};
// r_unavailable (optional) is set when the input does not exist (no install found / no export manifests),
// as opposed to an input that exists but fails to open.
bool open_source(const SourceOptions &p_options, OpenedSource &r_opened, std::string &r_error, bool *r_unavailable = nullptr);

// Parity of two sources (e.g. the install against an export directory): the same packages and banks
// (every AntPackageInfo field, provenance included) and, with p_payloads, byte-identical payloads for every
// bank and every Chunk package (read in parallel).
struct ParityResult {
	size_t packages_a = 0, packages_b = 0, package_rows_equal = 0;
	size_t banks_a = 0, banks_b = 0, bank_rows_equal = 0;
	size_t payloads = 0, payloads_equal = 0, payload_errors = 0;
	uint64_t payload_bytes = 0;
	std::vector<std::string> differences; // the first few
	bool ok() const;
};
void compare_sources(const AntPackageSource &p_a, const AntPackageSource &p_b, bool p_payloads, ParityResult &r_result);
std::string format_parity(const ParityResult &p_result);

// Loads s_BasicAssets into a library. Returns false (with a message) when the export lacks it.
bool load_library(const AntPackageSource &p_source, AntLibrary &r_library, std::string &r_error);
std::unique_ptr<AntPackage> load_package(const AntPackageSource &p_source, const AntPackageInfo &p_info, std::string &r_error);

struct CodecCensus {
	uint32_t clips = 0;
	uint32_t decoded = 0; // decode ok
	uint32_t unsupported = 0;
	uint32_t errors = 0;
	uint32_t key_count_mismatch = 0; // decoded keys != header key count
	uint32_t non_unit_quats = 0; // clips with any |q| - 1 >= 1e-4 after normalisation
	uint64_t keys = 0;
	uint64_t channel_values = 0; // keys * channels decoded
	uint32_t mapped = 0; // clips with a controller
	uint32_t mapped_perfect = 0; // every channel on a DOF of its own kind
};

struct Census {
	bool mp_only = false;
	uint32_t packages = 0; // non-static AntPackageAssets considered
	uint32_t packages_failed = 0;
	std::vector<std::string> failures;
	uint32_t chunk_size_ok = 0;
	uint32_t chunk_packages = 0;
	uint64_t static_objects = 0;
	uint64_t static_layouts = 0;
	std::map<std::string, CodecCensus> codecs; // all packages incl. the static one
	std::map<std::string, uint32_t> static_codecs;
	// DCT (§4)
	uint32_t dct_ok = 0;
	uint32_t dct_q_off = 0; // mean |q|-1 before normalisation > 2e-3
	double dct_worst_mean_q = 0.0;
	uint32_t dct_overruns = 0;
	std::map<int64_t, uint32_t> dct_datasize_residual;
	uint64_t dct_bitstream_bytes = 0;
	uint64_t dct_bits_read = 0;
	double dct_decode_seconds = 0.0;
	// Raw / Frame (§3.3)
	uint32_t raw_exact = 0, raw_padded = 0, raw_other = 0;
	uint32_t frame_exact = 0, frame_other = 0;
	// Mapping (§5)
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

// Parses every AntPackageAsset of the source (mp_only: the probe's MP-relevant set), decodes every clip and
// binds every clip of a mapped codec (DCT, Raw, Frame, VBR).
bool run_census(const AntPackageSource &p_source, const AntLibrary &p_library, bool p_mp_only, Census &r_census, std::string &r_error);
// The package for p_info, or null with r_error set; the pointer stays valid for the rest of the census.
using PackageLoader = std::function<const AntPackage *(const AntPackageInfo &p_info, std::string &r_error)>;
// Both censuses in one pass: every package is read and every clip decoded and bound once, then counted into r_all
// and, for the MP-relevant packages, into r_mp (either may be null). Equal to run_census(..., true, *r_mp) and
// run_census(..., false, *r_all) apart from the seconds fields. p_load (default: load_package from p_source) lets a
// caller reuse packages it already parsed.
bool run_censuses(const AntPackageSource &p_source, const AntLibrary &p_library, Census *r_mp, Census *r_all, std::string &r_error,
		const PackageLoader &p_load = {});
std::string format_census(const Census &p_census);

// The probe's clip JSON: source, fps, fpsScale, numTicks, endFrame, keyTimes, joints (rig rest pose),
// tracks (one per channel with the decoded value per key). p_digits < 0: shortest round-trip floats.
bool clip_to_json(const AntLibrary &p_library, const AntPackage &p_package, const GdStruct &p_clip, int p_digits, std::string &r_json,
		std::string &r_error);

struct CompareResult {
	bool shape_ok = false; // same source/keyTimes/joints/tracks structure
	std::vector<std::string> differences; // structural differences (first few)
	uint64_t values = 0; // numbers compared in tracks
	double max_track_diff = 0.0;
	uint64_t joint_values = 0;
	double max_joint_diff = 0.0;
};
// Compares two clip JSON documents (ours vs the oracle).
bool compare_clip_json(const std::string &p_ours, const std::string &p_oracle, CompareResult &r_result, std::string &r_error);

// The clip an oracle JSON describes: its "source.clip" is either the clip's __name or its controller's
// name, and bundle packages hold many clips of one name, so candidates are narrowed by codec, endFrame and
// keyTimes (all from the oracle). Returns the package short name in r_package.
bool oracle_package(const std::string &p_oracle, std::string &r_package, std::string &r_error);
bool find_oracle_clip(const AntLibrary &p_library, const AntPackage &p_package, const std::string &p_oracle, GdStruct &r_clip, std::string &r_error);

} // namespace bf3::tools::antdump

#pragma once
// antdump support: census over every Ant package, clip-to-JSON dump (the probe's clip JSON shape) and the
// comparison against such a JSON. Used by the antdump CLI and by the core.ant tests. Godot-free.
// Clean-room implementation from research/probes/ant-animation-probe.md.

#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ant/ant_source.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bf3::tools::antdump {

using namespace bf3::core::ant;

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

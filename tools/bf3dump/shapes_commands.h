#pragma once
// bf3dump shapes, and the shapes pass of `prepare --shapes` (M1 T1; docs/design/m1.md §5.5, §12.4): one level x
// mode's static world (bf3_sim static_world.h) through the derived-data cache.

// (bf3dump.cpp includes this; it must not pull in bf3/core/cache/digest.h, whose sha1_of differs from
// sha1_digest.h's, so the shapes are only forward declared here.)

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/result.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
}
namespace bf3::sim {
class StaticWorldShapes;
}

namespace bf3dump {

struct ShapesPass {
	std::shared_ptr<const bf3::sim::StaticWorldShapes> shapes;
	std::string source_digest; // hex; empty when unavailable
	size_t digest_bundles = 0;
	size_t digest_entries = 0;
	double seconds_load = 0.0; // load_level + traverse
	double seconds_digest = 0.0;
};

// Opens p_level x p_mode and loads its static world through p_cache (may be null: always cold, nothing stored).
bf3::core::Result<ShapesPass> run_shapes_pass(const std::shared_ptr<bf3::core::Install> &p_install, std::string_view p_level, std::string_view p_mode,
		bf3::core::Cache *p_cache, unsigned p_threads, bool p_raycast);
// "shapes   170 tiles, 1,114,171 triangles (...); world hit / N of N tiles hit, ...; X s" lines.
void print_shapes_pass(const ShapesPass &p_pass);

// `shapes <LEVEL> <MODE> [--cache=<dir> | --no-cache] [--threads=N] [--no-raycast] [--tiles]`.
int cmd_shapes(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

} // namespace bf3dump

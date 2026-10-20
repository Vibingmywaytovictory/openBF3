#pragma once

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
	std::string source_digest;
	size_t digest_bundles = 0;
	size_t digest_entries = 0;
	double seconds_load = 0.0;
	double seconds_digest = 0.0;
};

bf3::core::Result<ShapesPass> run_shapes_pass(const std::shared_ptr<bf3::core::Install> &p_install, std::string_view p_level, std::string_view p_mode,
		bf3::core::Cache *p_cache, unsigned p_threads, bool p_raycast);

void print_shapes_pass(const ShapesPass &p_pass);

int cmd_shapes(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

}

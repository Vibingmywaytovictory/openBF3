#pragma once

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

struct EbxContext {
	const bf3::core::InstallReader *reader = nullptr;
	std::shared_ptr<bf3::core::Install> install;
};

int cmd_find(const EbxContext &p_ctx, const std::string &p_spec, const std::vector<std::string> &p_needles);
int cmd_types(const EbxContext &p_ctx, const std::string &p_spec, const std::vector<std::string> &p_types);
int cmd_dump(const EbxContext &p_ctx, const std::string &p_spec, const std::string &p_out_dir, size_t p_depth, const std::string &p_follow,
		const std::vector<std::string> &p_roots);
int cmd_ebxload(const EbxContext &p_ctx, const std::string &p_spec, bool p_walk, bool p_lookups = false);

}

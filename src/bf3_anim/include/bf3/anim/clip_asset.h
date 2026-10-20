#pragma once

#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ant/ant_package.h"
#include "bf3/core/ant/ant_source.h"
#include "bf3/core/result.h"

#include <memory>
#include <string_view>

namespace bf3::anim {

core::Result<std::unique_ptr<core::ant::AntLibrary>> load_ant_library(const core::ant::AntPackageSource &p_source);

core::Result<std::unique_ptr<core::ant::AntPackage>> load_ant_package(const core::ant::AntPackageSource &p_source, std::string_view p_short_name);

struct LoadedClip {
	std::unique_ptr<core::ant::AntPackage> package;
	core::ant::ClipBinding binding;
	core::ant::DecodedClip clip;
};

core::Result<LoadedClip> load_clip(std::unique_ptr<core::ant::AntPackage> p_package, const core::ant::AntLibrary &p_library, std::string_view p_clip);
core::Result<LoadedClip> load_clip(const core::ant::AntPackageSource &p_source, const core::ant::AntLibrary &p_library, std::string_view p_package,
		std::string_view p_clip);

}
